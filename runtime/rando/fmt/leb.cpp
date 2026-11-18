// Room files (leb.h): ported from LAS-Randomizer's RandomizerCore/Tools/leb.py (MIT, Owen-Splat and contributors).
#include "leb.h"

#include <stdlib.h>

#include <stdexcept>

namespace rando {

bool LebParam::operator==(const LebParam &o) const {
    if (kind != o.kind) return false;
    return kind == Int ? i == o.i : kind == Float ? f == o.f : s == o.s;
}

// a parameter: value at +0, type at +4 (2 float, 4 string = offset into the names, anything else a number)
static LebParam read_param(const Bytes &d, size_t at, const Bytes &names) {
    uint32_t type = (uint32_t)read_le(d, at + 4, 4);
    if (type == 2) return LebParam::of_float(read_f32(d, at));
    uint32_t v = (uint32_t)read_le(d, at, 4);
    if (type == 4) return LebParam::of_str(read_cstr(names, v));
    return LebParam::of_int(v);
}

// written as leb.py packs one: a string goes into `repr` (the names this actor adds) at the next offset
static void write_param(Bytes &out, const LebParam &p, Bytes &repr, size_t name_off) {
    if (p.kind == LebParam::Str) {
        put_le(out, repr.size() + name_off, 4);
        put_le(out, 4, 4);
        repr += p.s;
        repr.push_back(0);
    } else if (p.kind == LebParam::Float) {
        put_f32(out, p.f);
        put_le(out, 2, 4);
    } else {
        put_le(out, p.i, 4);
        put_le(out, 3, 4);
    }
}

static LebActor read_actor(const Bytes &d, const Bytes &names) {
    LebActor a;
    a.key = read_le(d, 0, 8);
    a.name = read_cstr(names, (size_t)read_le(d, 8, 4));
    if (a.name.size() < 16 || strtoull(a.name.substr(a.name.size() - 16).c_str(), nullptr, 16) != a.key)
        throw std::runtime_error("leb: actor key does not match its name");
    a.type = (uint16_t)read_le(d, 0xC, 2);
    a.xE = (uint16_t)read_le(d, 0xE, 2);
    a.room_id = (uint32_t)read_le(d, 0x10, 4);
    for (int i = 0; i < 3; i++) {
        a.pos[i] = read_f32(d, 0x14 + 4 * i);
        a.rot[i] = read_f32(d, 0x20 + 4 * i);
        a.scale[i] = read_f32(d, 0x2C + 4 * i);
    }
    for (int i = 0; i < 8; i++) a.params[i] = read_param(d, 0x38 + 8 * i, names);
    for (int i = 0; i < 4; i++) a.switches[i] = {(uint8_t)read_le(d, 0x78 + i, 1), (uint16_t)read_le(d, 0x7C + 2 * i, 2)};
    LebLinks &l = a.links;
    l.e = (uint8_t)read_le(d, 0x84, 1), l.k = (uint8_t)read_le(d, 0x85, 1), l.b = (uint8_t)read_le(d, 0x86, 1);
    l.x = (uint8_t)read_le(d, 0x87, 1), l.y = (uint8_t)read_le(d, 0x88, 1), l.z = (uint8_t)read_le(d, 0x89, 1);
    l.null6 = d.size() >= 0x90 ? d.substr(0x8A, 6) : d.substr(std::min(d.size(), (size_t)0x8A));
    size_t pos = 0x90;
    for (int i = 0; i < l.x; i++, pos += 20) {
        LebLinks::ActorLink k;
        k.p[0] = read_param(d, pos, names), k.p[1] = read_param(d, pos + 8, names);
        k.actor = (uint32_t)read_le(d, pos + 0x10, 4);
        l.actors.push_back(k);
    }
    for (int i = 0; i < l.z; i++, pos += 24) {
        LebLinks::RailLink k;
        k.p[0] = read_param(d, pos, names), k.p[1] = read_param(d, pos + 8, names);
        k.rail = (uint32_t)read_le(d, pos + 0x10, 4), k.point = (uint32_t)read_le(d, pos + 0x14, 4);
        l.rails.push_back(k);
    }
    for (int i = 0; i < l.y; i++) l.ids.push_back((uint32_t)read_le(d, pos + 4 * i, 4));
    return a;
}

// leb.py Actor.pack: the record, its strings starting at name_off (the actor's own name first)
static Bytes pack_actor(const LebActor &a, size_t name_off) {
    Bytes out, repr = a.name;
    repr.push_back(0);
    put_le(out, a.key, 8);
    put_le(out, name_off, 4);
    put_le(out, a.type, 2);
    put_le(out, a.xE, 2);
    put_le(out, a.room_id, 4);
    for (double v : a.pos) put_f32(out, v);
    for (double v : a.rot) put_f32(out, v);
    for (double v : a.scale) put_f32(out, v);
    for (const LebParam &p : a.params) write_param(out, p, repr, name_off);
    Bytes sw;
    for (int i = 0; i < 4; i++) {
        put_le(out, a.switches[i].first, 1);
        put_le(sw, a.switches[i].second, 2);
    }
    out += sw;
    const LebLinks &l = a.links;
    for (uint8_t v : {l.e, l.k, l.b, l.x, l.y, l.z}) put_le(out, v, 1);
    out += l.null6;
    for (int i = 0; i < l.x; i++) {  // (the counts, as the Python packs by them)
        const LebLinks::ActorLink &k = l.actors.at(i);
        write_param(out, k.p[0], repr, name_off), write_param(out, k.p[1], repr, name_off);
        put_le(out, k.actor, 4);
    }
    for (int i = 0; i < l.z; i++) {
        const LebLinks::RailLink &k = l.rails.at(i);
        write_param(out, k.p[0], repr, name_off), write_param(out, k.p[1], repr, name_off);
        put_le(out, k.rail, 4), put_le(out, k.point, 4);
    }
    for (int i = 0; i < l.y; i++) put_le(out, l.ids.at(i), 4);
    return out;
}

static void add_str(Bytes &names, const LebParam &p) {
    if (p.kind == LebParam::Str) names += p.s, names.push_back(0);
}

LebRoom LebRoom::parse(const Bytes &data, bool with_grid) {
    LebRoom r;
    r.fh = FixedHash::parse(data);
    const FhEntry *act = r.fh.find("actor");
    if (!act || !act->sub) throw std::runtime_error("leb: no actor entry");
    for (const FhEntry &e : act->sub->entries) r.actors.push_back(read_actor(e.data, r.fh.names));
    if (with_grid) {
        const FhEntry *g = r.fh.find("grid");
        if (g && g->sub) {
            LebGrid &G = r.grid;
            const FhEntry *info = nullptr;
            for (const FhEntry &e : g->sub->entries) {
                if (e.name == "data") G.data_entry = &e;
                else if (e.name == "chain") G.chain_entry = &e;
                else if (e.name == "info") info = &e;
            }
            if (!info || !G.data_entry) throw std::runtime_error("leb: grid without data / info");
            G.room_height = (uint16_t)read_le(info->data, 0, 2);
            G.room_width = (uint16_t)read_le(info->data, 2, 2);
            G.tile_size = read_f32(info->data, 4), G.x = read_f32(info->data, 8), G.z = read_f32(info->data, 0xC);
            if (G.room_height != 8 && G.room_height != 2) throw std::runtime_error("leb: cannot determine room type");
            int n = G.room_height == 8 ? 80 : 20;
            for (int i = 0; i < n; i++) {
                LebTile t;
                for (int k = 0; k < 4; k++) t.flags[k] = (uint8_t)read_le(G.data_entry->data, 0x10 * i + k, 1);
                t.chain = (uint32_t)read_le(G.data_entry->data, 0x10 * i + 8, 4);
                t.elevation = read_f32(G.data_entry->data, 0x10 * i + 0xC);
                G.tiles.push_back(t);
            }
            r.has_grid = true;
        }
    }
    return r;
}

Bytes LebRoom::repack() {
    Bytes names;
    for (FhEntry &entry : fh.entries) {
        if (entry.name == "actor") {
            std::vector<FhEntry> list;
            for (const LebActor &a : actors) {
                FhEntry e;
                e.node = 0xFFF0, e.next = 0xFFFFFFFF;
                e.data = pack_actor(a, names.size());
                list.push_back(std::move(e));
                names += a.name, names.push_back(0);
                for (const LebParam &p : a.params) add_str(names, p);
                for (const auto &k : a.links.actors) add_str(names, k.p[0]), add_str(names, k.p[1]);
                for (const auto &k : a.links.rails) add_str(names, k.p[0]), add_str(names, k.p[1]);
            }
            auto sub = std::make_shared<FixedHash>(*entry.sub);  // (the nested header kept, its entries replaced)
            sub->entries = std::move(list);
            entry.sub = sub;
        }
        if (entry.name == "grid" && has_grid) {
            auto sub = std::make_shared<FixedHash>(*entry.sub);
            Bytes chain = grid.chain_entry ? grid.chain_entry->data : Bytes();
            bool has_chain = grid.chain_entry != nullptr;
            sub->entries.clear();
            FhEntry d;
            d.node = 0xFFF0, d.name = "data", d.next = 0xFFFFFFFF;
            for (const LebTile &t : grid.tiles) {
                for (int k = 0; k < 4; k++) put_le(d.data, t.flags[k], 1);
                for (int k = 0; k < 4; k++) put_le(d.data, t.flags[k], 1);
                put_le(d.data, t.chain, 4);
                put_f32(d.data, t.elevation);
            }
            sub->entries.push_back(d);
            if (has_chain) {
                FhEntry c;
                c.node = 0xFFF0, c.name = "chain", c.next = 0xFFFFFFFF, c.data = chain;
                sub->entries.push_back(c);
            }
            FhEntry in;
            in.node = 0xFFF0, in.name = "info", in.next = 0;
            put_le(in.data, grid.room_height, 2);
            put_le(in.data, grid.room_width, 2);
            put_f32(in.data, grid.tile_size), put_f32(in.data, grid.x), put_f32(in.data, grid.z);
            sub->entries.push_back(in);
            entry.sub = sub;
            grid.data_entry = &entry.sub->entries[0];  // (the grid now reads from the new entries)
            grid.chain_entry = has_chain ? &entry.sub->entries[1] : nullptr;
        }
        names += entry.name, names.push_back(0);
    }
    fh.names = names;
    return fh.to_binary();
}

}  // namespace rando
