// Level config files (lvb.h): ported from LAS-Randomizer's RandomizerCore/Tools/lvb.py (MIT, Owen-Splat and
// contributors).
#include "lvb.h"

#include <stdexcept>

namespace rando {

static Bytes slice(const Bytes &d, size_t a, size_t b) {
    if (a >= d.size()) return Bytes();
    return d.substr(a, std::min(b, d.size()) - a);
}

static void put_name32(Bytes &out, const Bytes &s) {  // (no padding past 32: as the Python, which pads by length)
    out += s;
    if (s.size() < 32) out.append(32 - s.size(), '\0');
}

LvbLevel LvbLevel::parse(const Bytes &data) {
    LvbLevel l;
    l.fh = FixedHash::parse(data);
    const FhEntry *z = l.fh.find("zone"), *c = l.fh.find("config");
    if (!z || !z->sub || !c) throw std::runtime_error("lvb: no zone / config entry");
    for (const FhEntry &e : z->sub->entries) {
        const Bytes &d = e.data;
        LvbZone zn;
        zn.room_id = (uint32_t)read_le(d, 0, 4);
        zn.unknown1 = slice(d, 0x4, 0x3C);
        zn.bgm = read_cstr(d, 0x3C), zn.se_amb = read_cstr(d, 0x5C), zn.group_amb = read_cstr(d, 0x7C);
        zn.unknown2 = slice(d, 0x9C, 0xB0);
        zn.room_type = read_cstr(d, 0xB0);
        l.zones.push_back(zn);
    }
    const Bytes &d = c->data;
    l.config.attr1 = (uint8_t)read_le(d, 0, 1);
    l.config.allow_companions = read_le(d, 1, 1) != 0;
    l.config.attr3 = (uint8_t)read_le(d, 2, 1), l.config.attr4 = (uint8_t)read_le(d, 3, 1);
    l.config.attr5 = (uint8_t)read_le(d, 4, 1), l.config.attr6 = (uint8_t)read_le(d, 5, 1);
    return l;
}

Bytes LvbLevel::repack() {
    for (FhEntry &e : fh.entries) {
        if (e.name == "zone") {
            auto sub = std::make_shared<FixedHash>(*e.sub);
            sub->entries.clear();
            for (const LvbZone &z : zones) {
                FhEntry n;
                n.node = 0xFFF0, n.next = 0xFFFFFFFF;
                put_le(n.data, z.room_id, 4);
                n.data += z.unknown1;
                put_name32(n.data, z.bgm), put_name32(n.data, z.se_amb), put_name32(n.data, z.group_amb);
                n.data += z.unknown2;
                put_name32(n.data, z.room_type);
                sub->entries.push_back(n);
            }
            e.sub = sub;
        }
        if (e.name == "config") {
            Bytes c;
            for (uint8_t v : {config.attr1, (uint8_t)config.allow_companions, config.attr3, config.attr4, config.attr5,
                              config.attr6})
                put_le(c, v, 1);
            c.push_back((char)0xFF);
            e.data = c;
        }
    }
    return fh.to_binary();
}

}  // namespace rando
