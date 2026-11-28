// NSO loader: decompress segments, build the memory image, parse MOD0/dynamic info (port of tools/nso.py), and the
// function starts from .eh_frame_hdr (tools/analyze.py eh_frame_functions).
#include "gen_internal.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace lasgen {

// LZ4 block format (the NSO segments): sequences of a token, literals and a back reference, ending in literals.
// Written for this project from the format description; every read and write is bounds-checked.
std::vector<uint8_t> lz4_block_decompress(const uint8_t *src, size_t n, size_t out_size) {
    std::vector<uint8_t> out;
    out.reserve(out_size);
    size_t i = 0;
    auto bad = [] { throw GenError("corrupt LZ4 data in the game code"); };
    auto length = [&](size_t base) {
        size_t len = base;
        if (base == 15) {
            for (;;) {
                if (i >= n) bad();
                uint8_t b = src[i++];
                len += b;
                if (b != 255) break;
            }
        }
        return len;
    };
    while (i < n) {
        uint8_t token = src[i++];
        size_t lit = length(token >> 4);
        if (lit > n - i || lit > out_size - out.size()) bad();
        out.insert(out.end(), src + i, src + i + lit);
        i += lit;
        if (i == n) break;  // the last sequence has literals only
        if (n - i < 2) bad();
        size_t off = src[i] | (size_t)src[i + 1] << 8;
        i += 2;
        if (off == 0 || off > out.size()) bad();
        size_t len = length(token & 15) + 4;
        if (len > out_size - out.size()) bad();
        size_t from = out.size() - off;
        for (size_t k = 0; k < len; k++) out.push_back(out[from + k]);  // (overlapping copies repeat bytes)
    }
    if (out.size() != out_size) bad();
    return out;
}

static uint32_t rd32(const std::vector<uint8_t> &b, size_t off) {
    if (off + 4 > b.size()) throw GenError("the game code file is truncated");
    uint32_t v;
    memcpy(&v, &b[off], 4);
    return v;
}

static uint64_t rd64(const std::vector<uint8_t> &b, size_t off) {
    if (off + 8 > b.size()) throw GenError("the game code file is truncated");
    uint64_t v;
    memcpy(&v, &b[off], 8);
    return v;
}

uint32_t Nso::u32(uint64_t off) const { return rd32(image, off); }
uint64_t Nso::u64(uint64_t off) const { return rd64(image, off); }

std::string Nso::cstr(uint64_t off) const {
    std::string s;
    for (uint64_t i = off;; i++) {
        if (i >= image.size()) throw GenError("ValueError: subsection not found");
        if (!image[i]) break;
        s += (char)image[i];
    }
    return s;  // (names here are ASCII: Python's utf-8 'replace' decoding gives the same text)
}

constexpr uint64_t DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7,
                   DT_RELASZ = 8, DT_JMPREL = 23;

Nso::Nso(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw GenError("cannot open " + path);
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() < 0x100 || memcmp(raw.data(), "NSO0", 4) != 0) throw GenError(path + " is not an NSO file");
    uint32_t flags = rd32(raw, 0xC);
    // build_id: raw[0x40:0x60].rstrip(b'\0').hex()
    size_t idlen = 0x20;
    while (idlen && raw[0x40 + idlen - 1] == 0) idlen--;
    for (size_t i = 0; i < idlen; i++) {
        char b[3];
        snprintf(b, sizeof b, "%02x", raw[0x40 + i]);
        build_id += b;
    }
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> segs;
    for (int i = 0; i < 3; i++) {
        uint32_t file_off = rd32(raw, 0x10 + i * 0x10), mem_off = rd32(raw, 0x14 + i * 0x10),
                 dec_size = rd32(raw, 0x18 + i * 0x10);
        uint32_t comp_size = rd32(raw, 0x60 + i * 4);
        if ((uint64_t)file_off + comp_size > raw.size()) throw GenError("the game code file is truncated");
        std::vector<uint8_t> data(raw.begin() + file_off, raw.begin() + file_off + comp_size);
        if (flags & (1u << i)) data = lz4_block_decompress(data.data(), data.size(), dec_size);
        segs.push_back({mem_off, std::move(data)});
    }
    bss_size = rd32(raw, 0x3C);
    text = {segs[0].first, segs[0].second.size()};
    rodata = {segs[1].first, segs[1].second.size()};
    data = {segs[2].first, segs[2].second.size()};
    uint64_t end = segs[2].first + segs[2].second.size() + bss_size;
    size = (end + 0xFFF) & ~0xFFFull;
    image.assign(size, 0);
    for (auto &s : segs) {
        if (s.first + s.second.size() > size) throw GenError("bad NSO segment layout");
        memcpy(&image[s.first], s.second.data(), s.second.size());
    }

    // _parse_mod0
    uint32_t mod0 = u32(4);
    if (mod0 + 4 > image.size() || memcmp(&image[mod0], "MOD0", 4) != 0) throw GenError("no MOD0 header");
    auto rel = [&](int i) -> uint64_t { return mod0 + (int64_t)(int32_t)u32(mod0 + 4 + i * 4); };
    dynamic = rel(0);
    eh_start = rel(3);
    eh_end = rel(4);
    std::map<uint64_t, uint64_t> dyn;
    uint64_t off = dynamic;
    for (;;) {
        uint64_t tag = u64(off), val = u64(off + 8);
        off += 16;
        if (tag == DT_NULL) break;
        dyn.emplace(tag, val);  // (setdefault: the first one wins)
    }
    uint64_t strtab = dyn.count(DT_STRTAB) ? dyn[DT_STRTAB] : 0;
    // dynsym count from DT_HASH nchain
    if (dyn.count(DT_HASH) && dyn.count(DT_SYMTAB)) {
        uint32_t nchain = u32(dyn[DT_HASH] + 4);
        uint64_t st = dyn[DT_SYMTAB];
        for (uint32_t i = 0; i < nchain; i++) {
            uint64_t o = st + i * 24ull;
            NsoSym s;
            uint32_t name = u32(o);
            if (o + 24 > image.size()) throw GenError("bad symbol table");
            s.info = image[o + 4];
            memcpy(&s.shndx, &image[o + 6], 2);
            s.value = u64(o + 8);
            s.size = u64(o + 16);
            s.name = cstr(strtab + name);
            syms.push_back(std::move(s));
        }
    }
    for (auto [base_tag, size_tag] : {std::pair<uint64_t, uint64_t>{DT_RELA, DT_RELASZ}, {DT_JMPREL, DT_PLTRELSZ}}) {
        if (!dyn.count(base_tag)) continue;
        uint64_t b = dyn[base_tag], n = dyn.at(size_tag) / 24;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t r_off = u64(b + i * 24), r_info = u64(b + i * 24 + 8);
            int64_t r_add = (int64_t)u64(b + i * 24 + 16);
            relocs.push_back({r_off, (uint32_t)(r_info & 0xFFFFFFFF), (uint32_t)(r_info >> 32), r_add});
        }
    }
}

// analyze.read_encoded
static std::pair<int64_t, int> read_encoded(const std::vector<uint8_t> &img, uint64_t off, int enc, uint64_t pc_base) {
    int fmt = enc & 0x0F, app = enc & 0x70;
    int64_t v;
    int size;
    if (fmt == 0x0B) v = (int32_t)rd32(img, off), size = 4;        // sdata4
    else if (fmt == 0x03) v = rd32(img, off), size = 4;            // udata4
    else if (fmt == 0x0C || fmt == 0x04) v = (int64_t)rd64(img, off), size = 8;
    else {
        char b[64];
        snprintf(b, sizeof b, "unsupported encoding %#x", enc);
        throw GenError(b);
    }
    if (app == 0x10) v += (int64_t)off;
    else if (app == 0x30) v += (int64_t)pc_base;
    return {v, size};
}

// Function start addresses from .eh_frame_hdr's binary search table.
std::vector<int64_t> eh_frame_functions(const Nso &n) {
    const auto &img = n.image;
    uint64_t hdr = n.eh_start;
    if (!hdr || hdr >= img.size()) return {};
    if (hdr + 4 > img.size()) throw GenError("bad eh_frame_hdr");
    int ptr_enc = img[hdr + 1], cnt_enc = img[hdr + 2], tbl_enc = img[hdr + 3];
    uint64_t off = hdr + 4;
    off += read_encoded(img, off, ptr_enc, hdr).second;
    auto [count, sz] = read_encoded(img, off, cnt_enc, hdr);
    off += sz;
    std::vector<int64_t> starts;
    for (int64_t i = 0; i < count; i++) {
        auto [loc, s1] = read_encoded(img, off, tbl_enc, hdr);
        off += s1;
        off += read_encoded(img, off, tbl_enc, hdr).second;
        starts.push_back(loc);
    }
    return starts;
}

}  // namespace lasgen
