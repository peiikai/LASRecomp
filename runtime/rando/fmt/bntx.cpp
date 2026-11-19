// Texture banks (bntx.h). Written for LAS Recomp from the format; the layout follows the BNTX editor the Python
// generator saves banks with (see the header), so banks come out byte for byte the same.
#include "bntx.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace rando {

static uint64_t rd(const std::string &d, uint64_t at, int n) {  // (n <= 8)
    if (at > d.size() || (uint64_t)n > d.size() - at) throw std::out_of_range("bntx: read past the end");
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)(uint8_t)d[at + i] << (8 * i);
    return v;
}
static void need(const std::string &d, uint64_t at, uint64_t n) {
    if (at > d.size() || n > d.size() - at) throw std::out_of_range("bntx: read past the end");
}
static int64_t rd64(const std::string &d, uint64_t at) { return (int64_t)rd(d, at, 8); }

static void put(std::string &o, uint64_t v, int n) {
    for (int i = 0; i < n; i++) o.push_back((char)(uint8_t)(v >> (8 * i)));
}
static void put_u(std::string &o, int64_t v, int n, const char *what) {  // (unsigned field: must fit)
    if (v < 0 || (n < 8 && (uint64_t)v >> (8 * n))) throw std::range_error(std::string("bntx: ") + what);
    put(o, (uint64_t)v, n);
}
static void pad(std::string &o, size_t n) { o.append(n, '\0'); }
static void poke(std::string &o, size_t at, uint64_t v, int n) {
    for (int i = 0; i < n; i++) o[at + i] = (char)(uint8_t)(v >> (8 * i));
}
// ((x - 1) | (y - 1)) + 1: x rounded up to y for a power of two; an alignment of 0 gives 0 (no padding then)
static int64_t round_up(int64_t x, int64_t y) { return ((x - 1) | (y - 1)) + 1; }
static int64_t pad_to(int64_t x, int64_t y) { int64_t r = round_up(x, y) - x; return r > 0 ? r : 0; }

// ---- name dictionary (ported from tools/rando/resdic.py) ----
// Bits are numbered from the end of the key: bit b is bit (b & 7) of byte len-1-(b >> 3); past a shorter key's
// start they read as 0.
static int key_bit(const std::string &k, int64_t b) {
    if (b < 0) throw std::invalid_argument("bntx: dictionary bit of a duplicate or empty name");
    uint64_t i = (uint64_t)b >> 3;
    return i >= k.size() ? 0 : ((uint8_t)k[k.size() - 1 - i] >> (b & 7)) & 1;
}
static int64_t first_diff(const std::string &a, const std::string &b) {
    int64_t n = (int64_t)std::max(a.size(), b.size()) * 8;
    for (int64_t i = 0; i < n; i++)
        if (key_bit(a, i) != key_bit(b, i)) return i;
    return -1;
}

std::vector<std::pair<int64_t, std::pair<uint16_t, uint16_t>>> resdic_build(const std::vector<std::string> &keys) {
    std::vector<int64_t> bit{-1};
    std::vector<size_t> left{0}, right{0};
    std::vector<const std::string *> key;
    static const std::string empty;
    key.push_back(&empty);
    for (const std::string &k : keys) {
        size_t prev = 0, cur = left[0];  // closest existing key
        while (bit[prev] < bit[cur]) {
            prev = cur;
            cur = key_bit(k, bit[cur]) ? right[cur] : left[cur];
        }
        int64_t nb = first_diff(k, *key[cur]);
        prev = 0, cur = left[0];  // insertion point: down while the bit grows and stays below nb
        while (bit[prev] < bit[cur] && bit[cur] < nb) {
            prev = cur;
            cur = key_bit(k, bit[cur]) ? right[cur] : left[cur];
        }
        size_t n = bit.size();
        bit.push_back(nb);
        key.push_back(&k);
        if (key_bit(k, nb)) left.push_back(cur), right.push_back(n);
        else left.push_back(n), right.push_back(cur);
        if (prev == 0) left[0] = n;
        else if (key_bit(k, bit[prev])) right[prev] = n;
        else left[prev] = n;
    }
    std::vector<std::pair<int64_t, std::pair<uint16_t, uint16_t>>> out;
    for (size_t i = 0; i < bit.size(); i++) {
        if (left[i] > 0xFFFF || right[i] > 0xFFFF) throw std::range_error("bntx: dictionary too large");
        out.push_back({bit[i], {(uint16_t)left[i], (uint16_t)right[i]}});
    }
    return out;
}

// ---- reading ----

Bntx Bntx::parse(const std::string &d, const std::unordered_set<std::string> *data_of) {
    if (d.size() < 0x20) throw std::runtime_error("bntx: too short");
    uint32_t bom = (uint32_t)rd(d, 0xC, 2);
    if (bom == 0xFFFE) throw std::runtime_error("bntx: big-endian banks are not supported");
    if (bom != 0xFEFF) throw std::runtime_error("bntx: bad byte order mark");
    Bntx b;
    b.magic = d.substr(0, 8);
    if (b.magic != std::string("BNTX\0\0\0\0", 8)) throw std::runtime_error("bntx: bad magic");
    uint64_t name_addr = rd(d, 0x10, 4), first_blk = rd(d, 0x16, 2), reloc = rd(d, 0x18, 4);

    need(d, 0x20, 0x38);  // (the container must be there)
    b.target = d.substr(0x20, 4);
    if (b.target != "NX  " && b.target != "Gen ") throw std::runtime_error("bntx: unknown target");
    uint32_t count = (uint32_t)rd(d, 0x24, 4);
    int64_t info_ptrs = rd64(d, 0x28), dict_addr = rd64(d, 0x38);

    if (rd(d, first_blk, 4) != 0x5254535F) throw std::runtime_error("bntx: no string table");  // "_STR"
    uint64_t st = first_blk + 16;
    uint32_t nstr = (uint32_t)rd(d, st, 4);
    std::unordered_map<int64_t, int> by_pos;
    uint64_t at = st + 8;
    for (uint32_t i = 0; i < nstr; i++) {
        uint64_t len = rd(d, at, 2);
        need(d, at + 2, len);
        b.string_pos.push_back((int64_t)at);
        b.strings.push_back(d.substr(at + 2, len));
        by_pos.emplace((int64_t)at, (int)i);
        at += len + 3;
        at = ((at - 1) | 1) + 1;  // (entries start on even offsets)
    }
    auto index_at = [&](int64_t pos) {
        auto it = by_pos.find(pos);
        if (it == by_pos.end()) throw std::runtime_error("bntx: pointer to no string");
        return it->second;
    };
    b.name_idx = index_at((int64_t)name_addr - 2);

    b.dict_pos = dict_addr;
    need(d, dict_addr, 8);
    b.dict_magic = d.substr(dict_addr, 4);
    uint32_t ndict = (uint32_t)rd(d, dict_addr + 4, 4);
    for (uint64_t i = 0; i <= ndict; i++) {
        uint64_t e = dict_addr + 8 + 16 * i;
        BntxDictEntry x;
        x.ref_bit = (int64_t)rd(d, e, 4);
        x.left = (uint16_t)rd(d, e + 4, 2);
        x.right = (uint16_t)rd(d, e + 6, 2);
        int64_t sp = rd64(d, e + 8);
        x.str_idx = i ? index_at(sp) : -1;
        b.dict.push_back(x);
    }

    for (uint32_t i = 0; i < count; i++) {
        int64_t blk = rd64(d, info_ptrs + 8 * (int64_t)i);
        if (rd(d, blk, 4) != 0x49545242) throw std::runtime_error("bntx: no texture info");  // "BRTI"
        uint64_t p = blk + 16;
        need(d, p, 0x90);
        BntxTexture t;
        t.flags = (uint8_t)rd(d, p, 1);
        t.dim = (uint8_t)rd(d, p + 1, 1);
        t.tile_mode = (uint16_t)rd(d, p + 2, 2);
        t.swizzle = (uint16_t)rd(d, p + 4, 2);
        t.num_mips = (uint16_t)rd(d, p + 6, 2);
        t.num_samples = (uint16_t)rd(d, p + 8, 2);
        t.format = (uint32_t)rd(d, p + 12, 4);
        t.access_flags = (uint32_t)rd(d, p + 16, 4);
        t.width = (int32_t)rd(d, p + 20, 4);
        t.height = (int32_t)rd(d, p + 24, 4);
        t.depth = (int32_t)rd(d, p + 28, 4);
        t.array_length = (uint32_t)rd(d, p + 32, 4);
        t.texture_layout = (uint32_t)rd(d, p + 36, 4);
        t.texture_layout2 = (uint32_t)rd(d, p + 40, 4);
        t.image_size = (uint32_t)rd(d, p + 64, 4);
        t.alignment = (uint32_t)rd(d, p + 68, 4);
        t.comp_sel = (uint32_t)rd(d, p + 72, 4);
        t.img_dim = (uint8_t)rd(d, p + 76, 1);
        int64_t name_at = rd64(d, p + 80);
        t.ptrs_addr = rd64(d, p + 96);
        t.user_data_addr = rd64(d, p + 104);
        t.user_dict_addr = rd64(d, p + 136);
        t.name_idx = index_at(name_at);
        t.name = b.strings[t.name_idx];
        int64_t first = rd64(d, t.ptrs_addr);
        t.mip_offsets.push_back(0);
        for (int m = 1; m < t.num_mips; m++) t.mip_offsets.push_back(rd64(d, t.ptrs_addr + 8 * m) - first);
        if (first < 0) throw std::runtime_error("bntx: negative data pointer");
        uint64_t from = std::min<uint64_t>((uint64_t)first, d.size());
        if (data_of && !data_of->count(t.name)) t.data = std::make_shared<const std::string>();  // (not wanted)
        else t.data = std::make_shared<const std::string>(d.substr(from, std::min<uint64_t>(t.image_size, d.size() - from)));
        b.textures.push_back(std::move(t));
    }

    // the relocation table must be there and readable (save writes its own)
    if (rd(d, reloc, 4) != 0x544C525F) throw std::runtime_error("bntx: no relocation table");  // "_RLT"
    uint32_t nsec = (uint32_t)rd(d, reloc + 8, 4);
    int64_t nent = 0;
    for (uint32_t s = 0; s < nsec; s++) {
        uint64_t q = reloc + 16 + 0x18 * (uint64_t)s;
        need(d, q, 0x18);
        nent = std::max(nent, (int64_t)(int32_t)rd(d, q + 16, 4) + (int64_t)(int32_t)rd(d, q + 20, 4));
    }
    if (nent > 0) need(d, reloc + 16 + 0x18 * (uint64_t)nsec, (uint64_t)nent * 8);
    return b;
}

// ---- writing ----

void Bntx::rebuild_dict() {
    if (dict.empty()) throw std::runtime_error("bntx: no dictionary root");
    std::vector<std::string> names;
    for (const BntxTexture &t : textures) names.push_back(t.name);
    auto tree = resdic_build(names);
    dict.resize(1);
    dict[0].ref_bit = 0xFFFFFFFF;
    dict[0].left = tree[0].second.first;
    dict[0].right = tree[0].second.second;
    for (size_t i = 1; i < tree.size(); i++) {
        BntxDictEntry x;
        x.ref_bit = tree[i].first;
        x.left = tree[i].second.first;
        x.right = tree[i].second.second;
        x.str_idx = textures[i - 1].name_idx;
        dict.push_back(x);
    }
}

namespace {
struct Reloc {  // one relocation entry: `structs` runs of `offsets` pointers from pos, `padding` pointers between
    int64_t pos;
    size_t structs;
    size_t offsets;
    uint8_t padding;
};
}  // namespace

std::string Bntx::save() const {
    const int64_t n = (int64_t)textures.size();
    const int64_t ptrs = 0x198;  // (after the header, the container and its memory pool)
    std::vector<Reloc> rel;
    // section 0: container pointers (info pointers and dictionary, then the memory pool), the info pointers
    rel.push_back({0x28, 2, 1, 1});
    rel.push_back({0x40, 1, 1, 0});
    for (int64_t pos = ptrs, left = n; left > 0; left -= 0xFF) {
        int64_t k = std::min<int64_t>(left, 0xFF);
        rel.push_back({pos, 1, (size_t)k, 0});
        pos += k * 8;
    }

    // string table
    const int64_t str_pos = ptrs + 8 * n + 16;
    std::string str;
    std::vector<int64_t> spos;
    put_u(str, (int64_t)strings.size(), 4, "string count");
    pad(str, 4);
    for (const std::string &s : strings) {
        int64_t at = str_pos + (int64_t)str.size();
        spos.push_back(at);
        put_u(str, (int64_t)s.size(), 2, "string length");
        str += s;
        str.push_back('\0');
        int64_t end = at + (int64_t)s.size() + 3;
        pad(str, (size_t)((((end - 1) | 1) + 1) - end));
    }
    auto str_at = [&](int idx) -> int64_t {
        if (idx == -1) return str_pos + 4;
        if (idx < 0 || (size_t)idx >= spos.size()) throw std::out_of_range("bntx: string index");
        return spos[idx];
    };

    // name dictionary, aligned to 8
    int64_t dpos = str_pos + (int64_t)str.size();
    std::string dic(pad_to(dpos, 8), '\0');
    dpos += (int64_t)dic.size();
    if (dict.empty()) throw std::runtime_error("bntx: no dictionary root");
    dic += dict_magic;
    put_u(dic, (int64_t)dict.size() - 1, 4, "dictionary count");
    for (const BntxDictEntry &x : dict) {
        put_u(dic, x.ref_bit, 4, "dictionary bit");
        put(dic, x.left, 2);
        put(dic, x.right, 2);
        put(dic, (uint64_t)str_at(x.str_idx), 8);
    }
    for (int64_t pos = dpos + 16, left = (int64_t)dict.size(); left > 0; left -= 0xFF) {
        int64_t k = std::min<int64_t>(left, 0xFF);  // (one pointer per 16-byte entry)
        rel.push_back({pos, (size_t)k, 1, 1});
        pos += k * 16;
    }
    const int64_t str_blk = (int64_t)str.size() + (int64_t)dic.size() + 16;

    // info blocks, then the data block
    const int64_t info_pos = str_pos - 16 + str_blk;
    int64_t data_pos = info_pos + 16;
    for (const BntxTexture &t : textures) data_pos += 0x2A0 + 8 * (int64_t)t.num_mips;
    const int64_t sec0_size = data_pos - 16;
    const int shift = target == "NX  " ? 0xC : 3;
    const int64_t data_align = (int64_t)1 << shift;
    const int64_t data_pad = pad_to(data_pos, data_align);
    data_pos += data_pad;
    const int64_t data_blk = data_pos - 16;

    size_t data_len = 0;
    for (const BntxTexture &t : textures) data_len += (t.data ? t.data->size() : 0) + t.alignment;
    std::string info, data;
    data.reserve(data_len + data_align);
    std::vector<int64_t> tex_pos;
    for (int64_t i = 0; i < n; i++) {
        const BntxTexture &t = textures[i];
        const int64_t tp = info_pos + (int64_t)info.size() + 16;  // (the description, after the block header)
        tex_pos.push_back(tp);
        rel.push_back({tp + 0x50, 1, 3, 0});  // name, parent, mip pointers
        rel.push_back({tp + 0x68, 1, 1, 0});  // user data
        rel.push_back({tp + 0x70, 1, 2, 0});  // texture, texture view
        rel.push_back({tp + 0x88, 1, 1, 0});  // user dictionary
        int64_t bs = 0x2A0 + 8 * (int64_t)t.num_mips + (i == n - 1 ? data_pad : 0);
        info += "BRTI";
        put_u(info, bs, 4, "info block size");
        put_u(info, bs, 4, "info block size");
        pad(info, 4);
        put(info, t.flags, 1);
        put(info, t.dim, 1);
        put(info, t.tile_mode, 2);
        put(info, t.swizzle, 2);
        put(info, t.num_mips, 2);
        put(info, t.num_samples, 2);
        pad(info, 2);
        put(info, t.format, 4);
        put(info, t.access_flags, 4);
        put(info, (uint32_t)t.width, 4);
        put(info, (uint32_t)t.height, 4);
        put(info, (uint32_t)t.depth, 4);
        put(info, t.array_length, 4);
        put(info, t.texture_layout & 7, 4);  // (the block height only)
        put(info, t.texture_layout2, 4);
        pad(info, 20);
        put(info, t.image_size, 4);
        put(info, t.alignment, 4);
        put(info, t.comp_sel, 4);
        put(info, t.img_dim, 1);
        pad(info, 3);
        put(info, (uint64_t)str_at(t.name_idx), 8);
        put(info, 0x20, 8);  // parent: the container
        put(info, (uint64_t)(tp + 0x290), 8);
        put(info, 0, 8);  // user data (dropped)
        put(info, (uint64_t)(tp + 0x90), 8);
        put(info, (uint64_t)(tp + 0x190), 8);
        put(info, 0, 8);  // descriptor slot
        put(info, 0, 8);  // user dictionary (dropped)
        pad(info, 0x200);
        int64_t at = data_pos + (int64_t)data.size();
        int64_t p = pad_to(at, (int64_t)t.alignment);
        pad(data, (size_t)p);
        at += p;
        for (int64_t off : t.mip_offsets) put(info, (uint64_t)(at + off), 8);
        if (t.data) data += *t.data;
    }
    int64_t reloc = data_pos + (int64_t)data.size();
    int64_t rp = pad_to(reloc, data_align);
    pad(data, (size_t)rp);
    reloc += rp;
    const int64_t data_size = (int64_t)data.size() + 16;
    const size_t sec0_count = rel.size();

    // section 1: the container's data pointer, every texture's mip pointers
    rel.push_back({0x30, 1, 1, 0});
    for (int64_t i = 0; i < n; i++) {
        for (int64_t pos = tex_pos[i] + 0x290, left = textures[i].num_mips; left > 0; left -= 0xFF) {
            int64_t k = std::min<int64_t>(left, 0xFF);
            rel.push_back({pos, 1, (size_t)k, 0});
            pos += k * 8;
        }
    }
    std::string rlt = "_RLT";
    put_u(rlt, reloc, 4, "relocation table offset");
    put(rlt, 2, 4);
    pad(rlt, 4);
    put(rlt, 0, 8);  // section 0: from the file start up to the data
    put(rlt, 0, 4);
    put_u(rlt, sec0_size, 4, "section size");
    put(rlt, 0, 4);
    put(rlt, (uint32_t)sec0_count, 4);
    put(rlt, 0, 8);  // section 1: the data block
    put_u(rlt, data_blk, 4, "data block offset");
    put_u(rlt, data_size, 4, "section size");
    put(rlt, (uint32_t)sec0_count, 4);
    put(rlt, (uint32_t)(rel.size() - sec0_count), 4);
    for (const Reloc &r : rel) {
        put_u(rlt, r.pos, 4, "relocation offset");
        put_u(rlt, (int64_t)r.structs, 2, "relocation count");
        put_u(rlt, (int64_t)r.offsets, 1, "relocation count");
        put(rlt, r.padding, 1);
    }

    // header, container, memory pool, info pointers, then the blocks
    std::string o;
    o.reserve(0x198 + 8 * n + 16 + str.size() + dic.size() + info.size() + data_pad + 16 + data.size() + rlt.size());
    o += magic;
    put(o, 0x40100, 4);
    put(o, 0xFEFF, 2);
    put(o, (uint64_t)shift, 1);
    put(o, 0x40, 1);
    put_u(o, str_at(name_idx) + 2, 4, "file name offset");
    put(o, 0, 2);
    put_u(o, str_pos - 16, 2, "first block offset");
    put_u(o, reloc, 4, "relocation table offset");
    put(o, 0, 4);  // (file size, below)
    o += target;
    put_u(o, n, 4, "texture count");
    put(o, (uint64_t)ptrs, 8);
    put(o, (uint64_t)data_blk, 8);
    put(o, (uint64_t)dpos, 8);
    put(o, 0x58, 8);  // memory pool
    put(o, 0, 8);
    put(o, 0, 4);
    pad(o, 4);
    pad(o, 0x140);
    for (int64_t tp : tex_pos) put(o, (uint64_t)(tp - 16), 8);
    o += "_STR";
    put_u(o, str_blk, 4, "string block size");
    put_u(o, str_blk, 4, "string block size");
    pad(o, 4);
    o += str;
    o += dic;
    o += info;
    pad(o, (size_t)data_pad);
    o += "BRTD";
    put(o, 0, 4);
    put_u(o, data_size, 4, "data block size");
    pad(o, 4);
    o += data;
    std::string().swap(data);
    o += rlt;
    if (o.size() >> 32) throw std::range_error("bntx: file too large");
    poke(o, 0x1C, o.size(), 4);
    return o;
}

// ---- tools/rando/bntx_merge.py ----

// (strings looked up by value, first match, as the Python does; one index per call to keep that rule)
struct FirstIndex {
    std::unordered_map<std::string, int> m;
    explicit FirstIndex(const std::vector<std::string> &strings) {
        for (size_t i = 0; i < strings.size(); i++) m.emplace(strings[i], (int)i);
    }
    int operator()(const std::string &s) const {
        auto it = m.find(s);
        if (it == m.end()) throw std::runtime_error("bntx: string is not in the string table: " + s);
        return it->second;
    }
};

static void add_named(Bntx &b, BntxTexture t) {  // (a new string table entry for its name, appended)
    b.strings.push_back(t.name);
    b.string_pos.push_back(0);
    t.name_idx = (int)b.strings.size() - 1;
    b.textures.push_back(std::move(t));
}

std::string bntx_merge(const std::string &base, const std::vector<const std::string *> &others,
                       const std::unordered_set<std::string> *only) {
    Bntx b = Bntx::parse(base);
    std::unordered_set<std::string> have;
    for (const BntxTexture &t : b.textures) have.insert(t.name);
    for (const std::string *data : others) {
        Bntx o = Bntx::parse(*data);
        for (BntxTexture &t : o.textures) {
            if (have.count(t.name) || (only && !only->count(t.name))) continue;
            have.insert(t.name);
            add_named(b, std::move(t));
        }
    }
    b.rebuild_dict();
    return b.save();
}

std::string bntx_subset(const std::vector<const std::string *> &sources, const std::unordered_set<std::string> &only) {
    return bntx_subset_from(
        sources.size(), [&](size_t i, std::string &out) { return out = *sources[i], true; }, only);
}

std::string bntx_subset_from(size_t count, const std::function<bool(size_t, std::string &)> &read,
                             const std::unordered_set<std::string> &only) {
    if (!count) throw std::invalid_argument("bntx: subset of nothing");
    std::string first;
    {
        std::string src;
        if (!read(0, src)) throw std::runtime_error("bntx: a source bank can't be read");
        Bntx b = Bntx::parse(src, &only);
        std::string().swap(src);
        std::unordered_set<std::string> drop;
        for (const BntxTexture &t : b.textures)
            if (!only.count(t.name)) drop.insert(t.name);
        std::string fname = b.strings.at(b.name_idx);
        std::vector<std::string> strings;
        for (std::string &s : b.strings)
            if (!drop.count(s)) strings.push_back(std::move(s));
        b.strings = std::move(strings);
        b.string_pos.assign(b.strings.size(), 0);
        std::vector<BntxTexture> keep;
        for (BntxTexture &t : b.textures)
            if (!drop.count(t.name)) keep.push_back(std::move(t));
        b.textures = std::move(keep);
        FirstIndex idx(b.strings);
        for (BntxTexture &t : b.textures) t.name_idx = idx(t.name);
        b.name_idx = idx(fname);
        b.rebuild_dict();
        first = b.save();
    }
    // (the Python saves this and merges the other sources into the re-read file: bntx_merge, one source at a time)
    Bntx b = Bntx::parse(first);
    std::string().swap(first);
    std::unordered_set<std::string> have;
    for (const BntxTexture &t : b.textures) have.insert(t.name);
    for (size_t i = 1; i < count; i++) {
        std::string src;
        if (!read(i, src)) throw std::runtime_error("bntx: a source bank can't be read");
        Bntx o = Bntx::parse(src, &only);
        std::string().swap(src);
        for (BntxTexture &t : o.textures) {
            if (have.count(t.name) || !only.count(t.name)) continue;
            have.insert(t.name);
            add_named(b, std::move(t));
        }
    }
    b.rebuild_dict();
    return b.save();
}

std::string bntx_copy_renamed(const std::string &base, const std::vector<std::pair<std::string, std::string>> &pairs) {
    Bntx b = Bntx::parse(base);
    std::unordered_map<std::string, size_t> by;  // (the last texture of a name, as a Python dict keeps it)
    for (size_t i = 0; i < b.textures.size(); i++) by[b.textures[i].name] = i;
    std::unordered_set<std::string> targets;
    for (const auto &p : pairs) targets.insert(p.second);
    std::vector<BntxTexture> copies;
    for (const auto &p : pairs) {
        auto it = by.find(p.first);
        if (it == by.end()) continue;
        BntxTexture t = b.textures[it->second];
        t.name = p.second;
        copies.push_back(std::move(t));
    }
    std::string fname = b.strings.at(b.name_idx);
    std::vector<std::string> strings;
    for (std::string &s : b.strings)
        if (!targets.count(s)) strings.push_back(std::move(s));
    b.strings = std::move(strings);
    b.string_pos.assign(b.strings.size(), 0);
    std::vector<BntxTexture> keep;
    for (BntxTexture &t : b.textures)
        if (!targets.count(t.name)) keep.push_back(std::move(t));
    b.textures = std::move(keep);
    {
        FirstIndex idx(b.strings);
        for (BntxTexture &t : b.textures) t.name_idx = idx(t.name);
        b.name_idx = idx(fname);
    }
    for (BntxTexture &t : copies) add_named(b, std::move(t));
    b.rebuild_dict();
    return b.save();
}

std::string bntx_patch_in_place(const std::string &base,
                                const std::vector<std::pair<std::string, std::string>> &set_from,
                                const std::vector<std::pair<std::string, std::string>> &rename) {
    Bntx b = Bntx::parse(base);
    std::string out = base;
    std::unordered_map<std::string, size_t> by;
    for (size_t i = 0; i < b.textures.size(); i++) by[b.textures[i].name] = i;
    for (const auto &r : rename) {
        auto it = by.find(r.first);
        if (it == by.end()) throw std::runtime_error("bntx: no texture " + r.first);
        BntxTexture &t = b.textures[it->second];
        const std::string &s = b.strings.at(t.name_idx);
        if (r.second.size() != s.size()) throw std::invalid_argument("bntx: rename must keep the length: " + r.first);
        out.replace((size_t)b.string_pos[t.name_idx] + 2, s.size(), r.second);
        b.strings[t.name_idx] = r.second;
        t.name = r.second;
    }
    by.clear();
    for (size_t i = 0; i < b.textures.size(); i++) by[b.textures[i].name] = i;
    for (const auto &sf : set_from) {
        auto di = by.find(sf.first), si = by.find(sf.second);
        if (di == by.end() || si == by.end()) throw std::runtime_error("bntx: no texture " + sf.first + " / " + sf.second);
        const BntxTexture &td = b.textures[di->second], &ts = b.textures[si->second];
        if (td.format != ts.format || td.width != ts.width || td.height != ts.height || td.num_mips != ts.num_mips ||
            td.image_size != ts.image_size)
            throw std::invalid_argument("bntx: textures differ in shape: " + sf.first + " / " + sf.second);
        uint64_t od = rd(base, td.ptrs_addr, 8), os = rd(base, ts.ptrs_addr, 8);
        // (slice semantics: clipped at the end of the file)
        size_t a = (size_t)std::min<uint64_t>(od, out.size());
        size_t e = (size_t)std::min<uint64_t>(od + td.image_size, out.size());
        size_t sa = (size_t)std::min<uint64_t>(os, base.size());
        size_t se = (size_t)std::min<uint64_t>(os + ts.image_size, base.size());
        out.replace(a, e - a, base, sa, se - sa);
    }
    if (!rename.empty()) {
        std::vector<std::string> names;
        for (const BntxTexture &t : b.textures) names.push_back(t.name);
        auto tree = resdic_build(names);
        for (size_t i = 0; i < tree.size(); i++) {
            size_t at = (size_t)b.dict_pos + 8 + 16 * i;
            if (at + 8 > out.size()) throw std::out_of_range("bntx: dictionary past the end");
            poke(out, at, (uint64_t)tree[i].first & 0xFFFFFFFF, 4);
            poke(out, at + 4, tree[i].second.first, 2);
            poke(out, at + 6, tree[i].second.second, 2);
        }
    }
    return out;
}

}  // namespace rando
