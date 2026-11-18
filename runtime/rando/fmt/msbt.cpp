// Message files (msbt.h): ported from LAS Recomp's tools/rando/msbt.py.
#include "msbt.h"

#include <algorithm>
#include <stdexcept>

namespace rando {

static uint32_t u32(const std::string &d, size_t at) {
    if (at + 4 > d.size()) throw std::out_of_range("msbt: read past the end");
    return (uint8_t)d[at] | (uint8_t)d[at + 1] << 8 | (uint8_t)d[at + 2] << 16 | (uint32_t)(uint8_t)d[at + 3] << 24;
}
static void put32(std::string &o, uint32_t v) {
    for (int i = 0; i < 4; i++) o.push_back((char)(uint8_t)(v >> (8 * i)));
}

static uint32_t label_hash(const std::string &label, uint32_t slots) {
    uint32_t h = 0;
    for (unsigned char c : label) h = h * 0x492 + c;
    return h % slots;
}

Msbt Msbt::parse(const std::string &d) {
    if (d.compare(0, 8, "MsgStdBn")) throw std::runtime_error("msbt: not an msbt");
    if (d.size() < 0x20) throw std::runtime_error("msbt: short header");
    Msbt m;
    m.bom = d.substr(8, 2);
    m.enc = (uint8_t)d[0xC], m.version = (uint8_t)d[0xD];
    unsigned nsec = (uint8_t)d[0xE] | (uint8_t)d[0xF] << 8;
    size_t off = 0x20;
    for (unsigned i = 0; i < nsec; i++) {
        if (off + 0x10 > d.size()) throw std::out_of_range("msbt: section past the end");
        uint32_t size = u32(d, off + 4);
        if (size > d.size() - off - 0x10) throw std::out_of_range("msbt: section size");
        m.sections.emplace_back(d.substr(off, 4), d.substr(off + 0x10, size));
        off = (off + 0x10 + size + 15) & ~(size_t)15;
    }
    auto sec = [&](const char *magic) -> const std::string * {  // (as dict(): the last of a magic wins)
        const std::string *r = nullptr;
        for (auto &s : m.sections)
            if (s.first == magic) r = &s.second;
        return r;
    };
    const std::string *lbl = sec("LBL1"), *txt = sec("TXT2"), *atr = sec("ATR1");
    if (!lbl || !txt) throw std::runtime_error("msbt: no LBL1 / TXT2");
    m.slots = u32(*lbl, 0);
    for (uint32_t i = 0; i < m.slots; i++) {
        uint32_t n = u32(*lbl, 4 + i * 8), o = u32(*lbl, 8 + i * 8);
        for (uint32_t k = 0; k < n; k++) {
            if (o >= lbl->size()) throw std::out_of_range("msbt: label past the end");
            uint8_t ln = (uint8_t)(*lbl)[o];
            std::string name = lbl->substr(o + 1, ln);
            m.labels[name] = u32(*lbl, o + 1 + ln);
            o += 1 + ln + 4;
        }
    }
    uint32_t cnt = u32(*txt, 0);
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < cnt; i++) offs.push_back(u32(*txt, 4 + 4 * i));
    offs.push_back((uint32_t)txt->size());
    for (uint32_t i = 0; i < cnt; i++) {
        if (offs[i] > txt->size() || offs[i + 1] < offs[i]) throw std::out_of_range("msbt: string offsets");
        m.strings.push_back(txt->substr(offs[i], offs[i + 1] - offs[i]));
    }
    if (atr) m.has_atr = true, m.atr = *atr;
    return m;
}

const std::string *Msbt::text(const std::string &label) const {
    auto it = labels.find(label);
    return it == labels.end() || it->second >= strings.size() ? nullptr : &strings[it->second];
}

void Msbt::set(const std::string &label, std::string raw) {
    if (raw.empty() || raw.back() != '\0') raw.push_back('\0');
    auto it = labels.find(label);
    if (it != labels.end()) { strings.at(it->second) = raw; return; }
    labels[label] = (uint32_t)strings.size();
    strings.push_back(raw);
    if (has_atr) {  // one attribute record per string: a zero one for the new string
        uint32_t n = u32(atr, 0), size = u32(atr, 4);
        std::string a;
        put32(a, n + 1), put32(a, size);
        a += atr.substr(8, (size_t)n * size);
        a.append(size, '\0');
        a += atr.substr(std::min(atr.size(), 8 + (size_t)n * size));
        atr = a;
    }
}

void Msbt::copy_attr(const std::string &label, const std::string &src) {
    if (!has_atr) return;
    uint32_t size = u32(atr, 4), a = labels.at(label), b = labels.at(src);
    std::string rec = atr.substr(8 + (size_t)b * size, size);
    atr = atr.substr(0, 8 + (size_t)a * size) + rec + atr.substr(std::min(atr.size(), 8 + (size_t)(a + 1) * size));
}

std::string Msbt::pack() const {
    std::vector<std::vector<std::pair<std::string, uint32_t>>> buckets(slots);
    std::vector<std::pair<std::string, uint32_t>> by_index(labels.begin(), labels.end());
    std::stable_sort(by_index.begin(), by_index.end(), [](auto &x, auto &y) { return x.second < y.second; });
    for (auto &kv : by_index) buckets.at(label_hash(kv.first, slots)).push_back(kv);
    std::string head, body;
    put32(head, slots);
    uint32_t base = 4 + slots * 8;
    for (auto &b : buckets) {
        put32(head, (uint32_t)b.size()), put32(head, base + (uint32_t)body.size());
        for (auto &kv : b) {
            body.push_back((char)(uint8_t)kv.first.size());
            body += kv.first;
            put32(body, kv.second);
        }
    }
    std::string lbl = head + body, txt;
    put32(txt, (uint32_t)strings.size());
    uint32_t o = 4 + 4 * (uint32_t)strings.size();
    for (auto &s : strings) put32(txt, o), o += (uint32_t)s.size();
    for (auto &s : strings) txt += s;

    std::string out;
    for (auto &s : sections) {
        const std::string &payload = s.first == "LBL1" ? lbl : s.first == "TXT2" ? txt
                                     : s.first == "ATR1" && has_atr ? atr : s.second;
        out += s.first;
        put32(out, (uint32_t)payload.size());
        out.append(8, '\0');
        out += payload;
        out.append((16 - out.size() % 16) % 16, (char)0xAB);
    }
    std::string hdr = "MsgStdBn" + bom;
    hdr.append(2, '\0');
    hdr.push_back((char)enc), hdr.push_back((char)version);
    hdr.push_back((char)(uint8_t)sections.size()), hdr.push_back((char)(uint8_t)(sections.size() >> 8));
    hdr.append(2, '\0');
    put32(hdr, 0x20 + (uint32_t)out.size());
    hdr.append(10, '\0');
    return hdr + out;
}

}  // namespace rando
