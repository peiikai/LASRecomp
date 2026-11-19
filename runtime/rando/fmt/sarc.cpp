// Archives (sarc.h). Written for LAS Recomp from the format; the layout follows oead 1.3's SarcWriter (see the
// header) so archives come out byte for byte the same.
#include "sarc.h"

#include <string.h>

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace rando {

static uint32_t rd(const std::string &d, size_t at, int n) {
    if (at + n > d.size()) throw std::out_of_range("sarc: read past the end");
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)(uint8_t)d[at + i] << (8 * i);
    return v;
}
static void wr(std::string &o, size_t at, uint32_t v, int n) {
    if (o.size() < at + n) o.resize(at + n, '\0');
    for (int i = 0; i < n; i++) o[at + i] = (char)(uint8_t)(v >> (8 * i));
}
static uint32_t name_hash(uint32_t mult, const std::string &name) {
    uint32_t h = 0;
    for (char c : name) h = h * mult + (uint32_t)(int32_t)(signed char)c;  // (char as the archive tools hash it)
    return h;
}
static bool pow2(uint32_t a) { return a && !(a & (a - 1)); }
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

Sarc Sarc::parse(const std::string &d, const std::function<bool(const std::string &)> &keep) {
    if (d.size() < 0x14 || d.compare(0, 4, "SARC")) throw std::runtime_error("sarc: bad magic");
    if (rd(d, 4, 2) != 0x14) throw std::runtime_error("sarc: header size");
    if (rd(d, 6, 2) != 0xFEFF) throw std::runtime_error("sarc: not little endian");
    if (rd(d, 0x10, 2) != 0x0100) throw std::runtime_error("sarc: unknown version");
    uint32_t data_off = rd(d, 0xC, 4);
    if (d.compare(0x14, 4, "SFAT") || rd(d, 0x18, 2) != 0xC) throw std::runtime_error("sarc: SFAT");
    uint32_t n = rd(d, 0x1A, 2);
    if (n >> 0xE) throw std::runtime_error("sarc: too many files");
    Sarc s;
    s.hash_multiplier = rd(d, 0x1C, 4);
    size_t entries = 0x20, fnt = entries + 0x10 * (size_t)n;
    if (d.compare(fnt, 4, "SFNT") || rd(d, fnt + 4, 2) != 8) throw std::runtime_error("sarc: SFNT");
    size_t names = fnt + 8;
    if (data_off < names) throw std::runtime_error("sarc: data before the names");
    uint32_t gcd = 4;
    for (uint32_t i = 0; i < n; i++) {
        size_t e = entries + 0x10 * (size_t)i;
        uint32_t name_off = rd(d, e + 4, 4), begin = rd(d, e + 8, 4), end = rd(d, e + 0xC, 4);
        std::string name;
        if (name_off) {
            size_t at = names + (size_t)(name_off & 0xFFFFFF) * 4, z = d.find('\0', at);
            if (at >= d.size() || z == std::string::npos) throw std::out_of_range("sarc: name");
            name = d.substr(at, z - at);
        }
        if (end < begin || (size_t)data_off + end > d.size()) throw std::out_of_range("sarc: file data");
        s.files.emplace_back(name, !keep || keep(name) ? d.substr(data_off + begin, end - begin) : std::string());
        gcd = std::gcd(gcd, data_off + begin);
    }
    s.min_alignment = pow2(gcd) ? gcd : 4;
    return s;
}

const std::string *Sarc::get(const std::string &name) const {
    for (auto &f : files)
        if (f.first == name) return &f.second;
    return nullptr;
}

void Sarc::put(const std::string &name, const std::string &data) {
    for (auto &f : files)
        if (f.first == name) { f.second = data; return; }
    files.emplace_back(name, data);
}

// a file's alignment: the minimum, or what its binary file header asks for
static uint32_t file_alignment(const std::string &name, const std::string &data, uint32_t min_align) {
    static const char *const kKnown[] = {"bflan", "bflyt", "bntx", "bnsh", "bushvt"};
    size_t dot = name.rfind('.');
    std::string ext = dot != std::string::npos && dot + 1 < name.size() ? name.substr(dot + 1) : "";
    if (std::find_if(std::begin(kKnown), std::end(kKnown), [&](const char *k) { return ext == k; }) == std::end(kKnown))
        throw std::runtime_error("sarc: can't align ." + ext + " files as oead does");
    uint32_t a = min_align;
    if (data.size() > 0x20) {
        uint32_t bom = (uint8_t)data[0xC] << 8 | (uint8_t)data[0xD];  // (read big endian: FE FF or FF FE)
        if (bom == 0xFEFF || bom == 0xFFFE) {
            bool le = bom == 0xFFFE;
            uint32_t size = 0;
            for (int i = 0; i < 4; i++) size |= (uint32_t)(uint8_t)data[0x1C + i] << (le ? 8 * i : 8 * (3 - i));
            if (size == data.size()) a = std::lcm(a, 1u << (uint8_t)data[0xE]);
        }
    }
    return a;
}

std::string Sarc::write() const {
    std::vector<size_t> order(files.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return name_hash(hash_multiplier, files[a].first) < name_hash(hash_multiplier, files[b].first);
    });
    std::string out(0x14, '\0');
    out += "SFAT";
    wr(out, out.size(), 0xC, 2), wr(out, out.size(), (uint32_t)files.size(), 2), wr(out, out.size(), hash_multiplier, 4);
    std::vector<uint32_t> aligns;
    uint32_t rel_name = 0, rel_data = 0;
    for (size_t i : order) {
        const auto &f = files[i];
        uint32_t a = file_alignment(f.first, f.second, min_alignment);
        aligns.push_back(a);
        uint32_t begin = align_up(rel_data, a), end = begin + (uint32_t)f.second.size();
        size_t e = out.size();
        wr(out, e, name_hash(hash_multiplier, f.first), 4);
        wr(out, e + 4, 1u << 24 | (rel_name / 4), 4);
        wr(out, e + 8, begin, 4), wr(out, e + 0xC, end, 4);
        rel_data = end;
        rel_name += align_up((uint32_t)f.first.size() + 1, 4);
    }
    out += "SFNT";
    wr(out, out.size(), 8, 2), wr(out, out.size(), 0, 2);
    for (size_t i : order) {
        out += files[i].first;
        out.push_back('\0');
        out.resize(align_up((uint32_t)out.size(), 4), '\0');
    }
    uint32_t all = 1;
    for (uint32_t a : aligns) all = std::lcm(all, a);
    out.resize(align_up((uint32_t)out.size(), all), '\0');
    uint32_t data_begin = (uint32_t)out.size();
    for (size_t k = 0; k < order.size(); k++) {
        out.resize(align_up((uint32_t)out.size(), aligns[k]), '\0');
        out += files[order[k]].second;
    }
    memcpy(&out[0], "SARC", 4);
    wr(out, 4, 0x14, 2), wr(out, 6, 0xFEFF, 2), wr(out, 8, (uint32_t)out.size(), 4), wr(out, 0xC, data_begin, 4);
    wr(out, 0x10, 0x0100, 2), wr(out, 0x12, 0, 2);
    return out;
}

}  // namespace rando
