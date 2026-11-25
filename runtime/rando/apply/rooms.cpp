// Dungeon room shuffle in the seed build (rooms.h): LAS Recomp's tools/rando/dungeon_gen.py write_all, rooms.py
// transplant and level_builder.py, ported. Each function keeps its Python's name; reads follow the Python's overlay
// romfs (las_headless.shuffle_dungeon_rooms): a level folder there is the game's files with the seed's own room files
// copied over them when write_all starts (a snapshot: later writes aren't seen), every other folder the game's own;
// the reads that name the seed's output folder see what the seed has written (SeedBuild::read_bytes).
#include "rooms.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

#include "../fmt/bntx.h"
#include "../fmt/fhash.h"
#include "decide.h"
#include "las_lib.h"
#include "ours_world_lib.h"
#include "../../../third_party/yyjson/yyjson.h"

namespace rando {

// ======== rooms.py: the grid
namespace rooms {

static const char *const TOWER = "Lv07EagleTower";

// Eagle's Tower: its 10 x 10 grid and file names that don't match positions (rooms._tower_positions)
static const std::map<std::string, std::pair<int, int>> &tower_positions() {
    static const std::map<std::string, std::pair<int, int>> pos = [] {
        std::map<std::string, std::pair<int, int>> p;
        char n[8];
        for (int r = 1; r < 9; r++)
            for (int c = 0; c < 8; c++) snprintf(n, sizeof n, "%02d%c", r, 'A' + c), p[n] = {c < 4 ? c : c + 2, r <= 4 ? r : r + 2};
        for (int r = 1; r < 11; r++)
            for (int c = 4; c < 6; c++) snprintf(n, sizeof n, "%02d%c", r, "IJ"[c - 4]), p[n] = {c, r};
        for (int r = 5; r < 7; r++)
            for (int c = 0; c < 10; c++)
                if (c != 4 && c != 5) snprintf(n, sizeof n, "%02d%c", r + 4, 'A' + (c < 4 ? c : c - 2)), p[n] = {c, r};
        return p;
    }();
    return pos;
}

std::string level_of(const std::string &room) { return room.substr(0, room.rfind('_')); }

std::pair<int, int> cell_pos(const std::string &room) {
    size_t u = room.rfind('_');
    std::string lv = room.substr(0, u), rc = room.substr(u + 1);
    if (lv == TOWER) {
        auto it = tower_positions().find(rc.substr(0, 3));
        if (it != tower_positions().end()) return it->second;
    }
    return {rc[2] - 'A', atoi(rc.substr(0, 2).c_str())};
}

std::pair<double, double> cell_origin(const std::string &room) {
    auto p = cell_pos(room);
    return {p.first * 15.0, (p.second - 1) * 12.0};
}

std::string cell_name(const std::string &level, int col, int row) {
    if (level == TOWER)
        for (const auto &kv : tower_positions())
            if (kv.second == std::make_pair(col, row)) return level + "_" + kv.first;
    char n[32];
    snprintf(n, sizeof n, "%02d", row);  // (Python's f'{row:02d}' - a negative row never names a cell anyway)
    return level + "_" + n + std::string(1, (char)('A' + col));
}

}  // namespace rooms

namespace {

using las::at;

const char *const LINK_LEVEL = "Lv07EagleTower";  // dungeon_gen.LINK_LEVELS
const uint16_t ROOF = 0x1B6, WARP_PAD = 0xF3;
const int GLOBAL_FLAG_BASE = 1600, CRYSTAL_FLAG = 9;
const uint8_t COPY_KEY_TAG = 0x6C;

const std::map<std::string, std::string> &entrances() {  // dungeon_gen.ENTRANCES
    static const std::map<std::string, std::string> e = {
        {"Lv01TailCave", "Lv01TailCave_08D"},           {"Lv02BottleGrotto", "Lv02BottleGrotto_08C"},
        {"Lv03KeyCavern", "Lv03KeyCavern_08B"},         {"Lv04AnglersTunnel", "Lv04AnglersTunnel_08D"},
        {"Lv05CatfishsMaw", "Lv05CatfishsMaw_08H"},     {"Lv06FaceShrine", "Lv06FaceShrine_08D"},
        {"Lv08TurtleRock", "Lv08TurtleRock_08D"},       {"Lv10ClothesDungeon", "Lv10ClothesDungeon_08C"},
        {"Lv07EagleTower", "Lv07EagleTower_08B"}};
    return e;
}

struct CatRoom {
    std::vector<std::string> unit;
    bool crystal = false;
};

const std::map<std::string, CatRoom> &catalog() {
    static const std::map<std::string, CatRoom> cat = [] {
        std::map<std::string, CatRoom> c;
        for (size_t i = 0; i < ROOM_CATALOG_SIZE; i++) {
            CatRoom r;
            r.crystal = ROOM_CATALOG[i].crystal;
            std::string u = ROOM_CATALOG[i].unit;
            for (size_t a = 0; a < u.size();) {
                size_t b = u.find(' ', a);
                if (b == std::string::npos) b = u.size();
                r.unit.push_back(u.substr(a, b - a));
                a = b + 1;
            }
            c[ROOM_CATALOG[i].name] = r;
        }
        return c;
    }();
    return cat;
}

bool in_cat(const std::string &room) { return catalog().count(room) != 0; }

std::string leb_path(const std::string &room) {
    return "region_common/level/" + rooms::level_of(room) + "/" + room + ".leb";
}
std::string lvb_path(const std::string &level) { return "region_common/level/" + level + "/" + level + ".lvb"; }

uint32_t crc32_of(const std::string &s) {  // (zlib.crc32)
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

float f32_at(const std::string &d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("rooms: read past the end");
    float f;
    memcpy(&f, &d[at], 4);
    return f;
}
void put_f32_at(std::string &d, size_t at, double v) {
    float f = (float)v;
    memcpy(&d[at], &f, 4);
}
uint32_t u32_at(const std::string &d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("rooms: read past the end");
    uint32_t v;
    memcpy(&v, &d[at], 4);
    return v;
}
void put_u32_at(std::string &d, size_t at, uint32_t v) { memcpy(&d[at], &v, 4); }

// re.finditer(rb'MI_[A-Za-z0-9_.]+', data): the texture names a model references
std::set<std::string> mi_names(const std::string &d) {
    std::set<std::string> out;
    auto cls = [](char c) { return isalnum((unsigned char)c) || c == '_' || c == '.'; };
    for (size_t i = 0; i + 3 < d.size();) {
        if (d.compare(i, 3, "MI_") == 0 && i + 3 < d.size() && cls(d[i + 3])) {
            size_t j = i + 3;
            while (j < d.size() && cls(d[j])) j++;
            out.insert(d.substr(i, j - i));
            i = j;
        } else {
            i++;
        }
    }
    return out;
}

// ---- the overlay romfs and the seed's own folder
struct Ctx {
    SeedBuild &b;
    std::map<std::string, std::string> snap;  // the seed's level files when write_all starts
    explicit Ctx(SeedBuild &bb) : b(bb) {
        const std::string pre = "region_common/level/";
        for (auto it = b.files().lower_bound(pre); it != b.files().end() && !it->first.compare(0, pre.size(), pre); ++it)
            snap[it->first] = it->second;
    }
    bool overlay_read(const std::string &rel, std::string &out) const {
        auto it = snap.find(rel);
        if (it != snap.end()) { out = it->second; return true; }
        return b.read_game(rel, out);
    }
    std::string overlay(const std::string &rel) const {
        std::string out;
        if (!overlay_read(rel, out)) throw std::runtime_error("rooms: no " + rel);
        return out;
    }
    // os.listdir of an overlay level folder: the game's files and the seed's copies (sorted)
    std::vector<std::string> overlay_list(const std::string &dir) const {
        std::set<std::string> names;
        for (const std::string &n : b.list_game(dir)) names.insert(n);
        const std::string pre = dir + "/";
        for (auto it = snap.lower_bound(pre); it != snap.end() && !it->first.compare(0, pre.size(), pre); ++it)
            names.insert(it->first.substr(pre.size(), it->first.find('/', pre.size()) - pre.size()));
        return std::vector<std::string>(names.begin(), names.end());
    }
    bool out_has(const std::string &rel) const { return b.files().count(rel) != 0; }
    std::string latest(const std::string &rel) const {  // (the seed's file if it wrote one, else the game's)
        std::string out;
        if (!b.read_bytes(rel, out)) throw std::runtime_error("rooms: no " + rel);
        return out;
    }

    // caches (the Python's lru_caches / module dicts: one seed per run)
    std::vector<std::pair<std::string, std::set<std::string>>> roofs;  // (map/<file>, model names)
    bool roofs_read = false;
    std::map<std::string, std::map<std::string, std::array<double, 4>>> boxes;
    std::map<int, std::pair<double, double>> shifts;
    bool shifts_read = false;
    std::map<std::string, int> anims;
    bool anims_read = false;
    std::map<std::string, std::map<std::string, uint32_t>> cells_of_level;
    // (the overlay's level files, parsed: write_all never sees its own writes there; each model's texture names)
    std::map<std::string, std::shared_ptr<struct Lvb>> lvbs;
    std::map<std::string, std::set<std::string>> model_textures;
};

// ======== dungeon_gen: the dungeon map's floor pages
const std::map<int, std::pair<double, double>> &map_shifts(Ctx &c) {
    if (c.shifts_read) return c.shifts;
    c.shifts_read = true;
    const std::string pre_anim = "anim/Dg_DgnMap_Dgn_";  // (only these of its ~75 MB)
    Sarc arc = Sarc::parse(c.overlay("region_common/ui/Game.arc"),
                           [&](const std::string &n) { return !n.compare(0, pre_anim.size(), pre_anim); });
    for (const auto &f : arc.files) {
        const std::string &n = f.first, pre = "anim/Dg_DgnMap_Dgn_";
        if (n.compare(0, pre.size(), pre) || n.size() <= pre.size() + 6 || n.compare(n.size() - 6, 6, ".bflan")) continue;
        std::string digits = n.substr(pre.size(), n.size() - 6 - pre.size());
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
        const std::string &b = f.second;
        size_t p = b.find("N_MapPosition_00");
        size_t t = p == std::string::npos ? std::string::npos : b.find("FLPA", p);
        if (t == std::string::npos) continue;
        double xy[2] = {0, 0};
        for (int i = 0; i < (uint8_t)b.at(t + 4); i++) {
            size_t io = t + u32_at(b, t + 8 + i * 4);
            int target = (uint8_t)b.at(io + 1);
            uint16_t nkeys = (uint16_t)((uint8_t)b.at(io + 4) | (uint8_t)b.at(io + 5) << 8);
            uint32_t koff = u32_at(b, io + 8);
            if (target < 2 && nkeys) xy[target] = f32_at(b, io + koff + 4);
        }
        c.shifts[atoi(digits.c_str())] = {xy[0], xy[1]};
    }
    return c.shifts;
}

const std::map<std::string, int> &map_anims(Ctx &c) {
    if (c.anims_read) return c.anims;
    c.anims_read = true;
    GSheet s = GSheet::parse(c.overlay("region_common/datasheets/UiDungeonArea.gsheet"));
    for (const GStruct &v : s.values) c.anims[at(v, "mLeaName").s] = at(v, "mAnimFloorIndex").i;
    return c.anims;
}

std::pair<double, double> map_shift(Ctx &c, const std::string &level, const std::string &cell) {
    const auto &anims = map_anims(c);
    auto ent = entrances().find(level);
    auto e = anims.find(ent == entrances().end() ? std::string() : ent->second);
    int anim = e == anims.end() ? 0 : e->second;
    auto it = anims.find(cell);
    if (it != anims.end()) anim = it->second;
    const auto &sh = map_shifts(c);
    auto s = sh.find(anim);
    return s == sh.end() ? std::make_pair(0.0, 0.0) : s->second;
}

std::string floor_room(const std::string &level, const std::string &cell, const std::string *room) {
    if (room && !room->empty() && level == LINK_LEVEL && rooms::level_of(*room) == level && *room != cell) return *room;
    return cell;
}

// ======== the level files' records
struct Lvb {  // a parsed .lvb (fixedhash.py)
    FHash fh;
    std::vector<std::string> area_names;
};

Lvb read_lvb(const std::string &bytes) {
    Lvb l;
    l.fh = FHash::parse(bytes);
    for (const FHEntry &e : l.fh.get("area")->sub->entries) l.area_names.push_back(e.name);
    return l;
}

Lvb &overlay_lvb(Ctx &c, const std::string &level) {
    auto it = c.lvbs.find(level);
    if (it == c.lvbs.end()) it = c.lvbs.emplace(level, std::make_shared<Lvb>(read_lvb(c.overlay(lvb_path(level))))).first;
    return *it->second;
}

// _source_records: the room's own area record and the record of the camera zone holding it (a zone: nullptr)
std::pair<std::string, std::shared_ptr<std::string>> source_records(Ctx &c, const std::string &room) {
    Lvb &l = overlay_lvb(c, rooms::level_of(room));
    auto it = std::find(l.area_names.begin(), l.area_names.end(), room);
    if (it == l.area_names.end()) throw std::runtime_error("rooms: no area " + room);
    size_t idx = (size_t)(it - l.area_names.begin());
    std::string area = l.fh.get("area")->sub->entries[idx].data;
    std::shared_ptr<std::string> zone;
    for (const FHEntry &z : l.fh.get("zone")->sub->entries) {
        uint8_t n = (uint8_t)z.data.at(0x9c + 6);
        for (int k = 0; k < n; k++)
            if ((uint8_t)z.data.at(0x9c + 7 + k) == idx) { zone = std::make_shared<std::string>(z.data); break; }
    }
    return {area, zone};
}

// level_cells: {area name: camera zone id} of the level's own level file
const std::map<std::string, uint32_t> &level_cells(Ctx &c, const std::string &level) {
    auto f = c.cells_of_level.find(level);
    if (f != c.cells_of_level.end()) return f->second;
    std::map<std::string, uint32_t> out;
    Lvb &l = overlay_lvb(c, level);
    for (const FHEntry &z : l.fh.get("zone")->sub->entries) {
        uint8_t n = (uint8_t)z.data.at(0x9c + 6);
        for (int k = 0; k < n; k++) {
            uint8_t i = (uint8_t)z.data.at(0x9c + 7 + k);
            if (i < l.area_names.size()) out[l.area_names[i]] = u32_at(z.data, 0);
        }
    }
    return c.cells_of_level[level] = out;
}

// offgrid_rooms: the level's rooms that aren't grid cells (side-view passages ...)
std::vector<std::string> offgrid_rooms(Ctx &c, const std::string &level) {
    std::vector<std::string> out;
    for (const std::string &f : c.overlay_list("region_common/level/" + level))
        if (f.size() > 4 && !f.compare(f.size() - 4, 4, ".leb") && !in_cat(f.substr(0, f.size() - 4)))
            out.push_back(f.substr(0, f.size() - 4));
    std::sort(out.begin(), out.end());
    return out;
}

std::set<std::string> crystal_levels() {
    std::set<std::string> out;
    for (const auto &kv : catalog())
        if (kv.second.crystal) out.insert(rooms::level_of(kv.first));
    return out;
}

// ======== flags (FlagMap / _CrystalFlagOnly)
struct Flags {
    virtual ~Flags() = default;
    virtual uint32_t glob(uint32_t i) = 0;
    virtual uint32_t local(const std::string &level, uint32_t i) = 0;
};

struct FlagMap : Flags {
    std::map<std::pair<std::string, uint32_t>, uint32_t> g;  // (scope or "", flag)
    std::map<std::tuple<std::string, uint32_t, std::string>, uint32_t> l;  // (level, flag, scope or "")
    uint32_t next_g, next_l = 1;
    std::set<uint32_t> reserved, keep;
    std::set<std::string> crystal;
    bool scoped = false;
    std::string scope;
    FlagMap(uint32_t next_global, const std::set<uint32_t> &res, const std::set<uint32_t> &kept,
            const std::set<std::string> &cr)
        : next_g(next_global), reserved(res), keep(kept), crystal(cr) {
        reserved.insert(CRYSTAL_FLAG);
    }
    uint32_t glob(uint32_t i) override {
        if (keep.count(i) && !scoped) return i;
        auto k = std::make_pair(scoped ? scope : std::string("\x01"), i);
        auto it = g.find(k);
        if (it != g.end()) return it->second;
        uint32_t v = g[k] = next_g++;
        if (next_g >= 3800) throw std::runtime_error("out of free event flags");
        return v;
    }
    uint32_t local(const std::string &level, uint32_t i) override {
        if (i == 0 || i == 255) return i;
        if (i == (uint32_t)CRYSTAL_FLAG && crystal.count(level)) return i;
        auto k = std::make_tuple(level, i, scoped ? scope : std::string("\x01"));
        auto it = l.find(k);
        if (it != l.end()) return it->second;
        while (reserved.count(next_l)) next_l++;
        uint32_t v = l[k] = next_l++;
        if (next_l >= 255) throw std::runtime_error("out of level-local flags");
        return v;
    }
};

struct CrystalFlagOnly : Flags {
    FlagMap &f;
    explicit CrystalFlagOnly(FlagMap &fm) : f(fm) {}
    uint32_t glob(uint32_t i) override { return i; }
    uint32_t local(const std::string &level, uint32_t i) override {
        return i == (uint32_t)CRYSTAL_FLAG ? f.local(level, i) : i;
    }
};

// ======== room edits
// rooms.remove_actors: drop actors, renumber every link (section 1 and 3) to the ones left
void remove_actors_if(LebRoom &r, const std::function<bool(const LebActor &)> &pred) {
    std::vector<long> renum(r.actors.size(), -1);
    std::vector<LebActor> keep;
    for (size_t i = 0; i < r.actors.size(); i++)
        if (!pred(r.actors[i])) renum[i] = (long)keep.size(), keep.push_back(r.actors[i]);
    r.actors = std::move(keep);
    for (LebActor &a : r.actors) {
        LebLinks &l = a.links;
        std::vector<LebLinks::ActorLink> s1;
        for (auto &k : l.actors)
            if (k.actor < renum.size() && renum[k.actor] >= 0) { auto n = k; n.actor = (uint32_t)renum[k.actor]; s1.push_back(n); }
        std::vector<uint32_t> s3;
        for (uint32_t j : l.ids)
            if (j < renum.size() && renum[j] >= 0) s3.push_back((uint32_t)renum[j]);
        l.actors = std::move(s1), l.ids = std::move(s3);
        l.x = (uint8_t)l.actors.size(), l.y = (uint8_t)l.ids.size();
    }
}

std::string param_str(const LebParam &p) { return p.kind == LebParam::Str ? p.s : std::string(); }

// rooms.transplant (merge_textures=False; the room keeps its own roof actor - LARECOMP_ROOF's default)
void transplant(Ctx &c, const std::string &src, const std::string &dst) {
    auto so = rooms::cell_origin(src), to = rooms::cell_origin(dst);
    double dx = to.first - so.first, dz = to.second - so.second;
    LebRoom r = LebRoom::parse(c.overlay(leb_path(src)));
    for (LebActor &a : r.actors) a.pos[0] += dx, a.pos[2] += dz;
    for (FhEntry &e : r.fh.entries) {
        if (e.name == "point" && e.sub) {
            auto sub = std::make_shared<FixedHash>(*e.sub);
            for (FhEntry &p : sub->entries) {
                double x = f32_at(p.data, 0), y = f32_at(p.data, 4), z = f32_at(p.data, 8);
                std::string d;
                put_f32(d, x + dx), put_f32(d, y), put_f32(d, z + dz);
                p.data = d + p.data.substr(12);
            }
            e.sub = sub;
        }
    }
    if (FhEntry *g = r.fh.find("grid")) {
        if (g->sub && g->sub->find("info")) {
            auto sub = std::make_shared<FixedHash>(*g->sub);
            FhEntry *info = sub->find("info");
            put_f32_at(info->data, 8, to.first), put_f32_at(info->data, 12, to.second);
            g->sub = sub;
        }
    }
    for (FhEntry &e : r.fh.entries)
        if (e.name == "infomation") {
            e.data.clear();
            put_le(e.data, crc32_of(dst), 4);
        }
    c.b.write_bytes(leb_path(dst), r.repack());
}

// _roof_variants: {map/<roof file>: the roof / fade model names in it}
const std::vector<std::pair<std::string, std::set<std::string>>> &roof_variants(Ctx &c) {
    if (c.roofs_read) return c.roofs;
    c.roofs_read = true;
    for (const std::string &f : c.b.list_game("region_common/map")) {
        if (f.compare(0, 13, "DungeonRoofLv") || f.size() < 6 || f.compare(f.size() - 6, 6, ".bfres")) continue;
        std::string d = c.overlay("region_common/map/" + f);
        std::set<std::string> names;
        // re.finditer(rb'Dungeon(?:Roof|Fade)(?:_ex[UDLR]+)?(?=\x00)', d)
        for (size_t i = 0; i < d.size();) {
            size_t end = std::string::npos;
            if (!d.compare(i, 7, "Dungeon") && (!d.compare(i + 7, 4, "Roof") || !d.compare(i + 7, 4, "Fade"))) {
                size_t k = i + 11;
                if (!d.compare(k, 3, "_ex")) {
                    size_t m = k + 3;
                    while (m < d.size() && strchr("UDLR", d[m]) && d[m]) m++;
                    if (m > k + 3 && m < d.size() && d[m] == 0) end = m;
                }
                if (end == std::string::npos && k < d.size() && d[k] == 0) end = k;
            }
            if (end != std::string::npos) names.insert(d.substr(i, end - i)), i = end;
            else i++;
        }
        c.roofs.push_back({"map/" + f, names});
    }
    return c.roofs;
}

const std::set<std::string> *roof_file(Ctx &c, const std::string &f) {
    for (const auto &kv : roof_variants(c))
        if (kv.first == f) return &kv.second;
    return nullptr;
}

// _model_boxes: {model name: (x0, x1, z0, z1)} of a roof file's models, from each model's shapes' bounding boxes
const std::map<std::string, std::array<double, 4>> &model_boxes(Ctx &c, const std::string &f) {
    auto found = c.boxes.find(f);
    if (found != c.boxes.end()) return found->second;
    std::map<std::string, std::array<double, 4>> out;
    std::string d;
    if (!c.overlay_read("region_common/" + f, d)) d.clear();
    auto q = [&](size_t at, bool &ok) -> uint64_t {
        if (at + 8 > d.size()) { ok = false; return 0; }
        uint64_t v;
        memcpy(&v, &d[at], 8);
        return v;
    };
    for (size_t o = d.find("FMDL"); o != std::string::npos; o = d.find("FMDL", o + 4)) {
        bool ok = true;  // (struct.error / UnicodeDecodeError: on to the next model, what was read kept)
        uint64_t no = q(o + 8, ok);
        if (!ok || no + 2 > d.size()) continue;
        uint16_t len = (uint16_t)((uint8_t)d[no] | (uint8_t)d[no + 1] << 8);
        std::string name = d.substr(no + 2, len);
        bool ascii = true;
        for (unsigned char ch : name) ascii = ascii && ch < 0x80;
        if (!ascii) continue;
        uint64_t shapes = q(o + 0x28, ok);
        if (!ok || o + 0x6C > d.size()) continue;
        uint16_t count = (uint16_t)((uint8_t)d[o + 0x6A] | (uint8_t)d[o + 0x6B] << 8);
        for (uint16_t i = 0; i < count; i++) {
            uint64_t s = shapes + (uint64_t)i * 0x60;
            if (s + 4 > d.size() || d.compare(s, 4, "FSHP")) break;
            uint64_t bb = q(s + 0x38, ok);
            if (!ok || bb + 24 > d.size()) break;
            double cx = f32_at(d, bb), cz = f32_at(d, bb + 8), ex = f32_at(d, bb + 12), ez = f32_at(d, bb + 20);
            std::array<double, 4> b = {cx - ex, cx + ex, cz - ez, cz + ez};
            auto it = out.find(name);
            std::array<double, 4> a = it == out.end() ? b : it->second;
            out[name] = {std::min(a[0], b[0]), std::max(a[1], b[1]), std::min(a[2], b[2]), std::max(a[3], b[3])};
        }
    }
    return c.boxes[f] = out;
}

bool covers(Ctx &c, const std::string &f, const std::string &model, const std::vector<std::pair<int, int>> &others) {
    const auto &boxes = model_boxes(c, f);
    auto it = boxes.find(model);
    if (it == boxes.end()) return !others.empty();
    double x0 = it->second[0], x1 = it->second[1], z0 = it->second[2], z1 = it->second[3];
    for (const auto &o : others) {
        bool xs = o.first < 0 ? x0 < -7.6 : o.first > 0 ? x1 > 7.6 : true;
        bool zs = o.second < 0 ? z0 < -6.1 : o.second > 0 ? z1 > 6.1 : true;
        if (xs && zs) return true;
    }
    return false;
}

// itertools.combinations(s, n), in its order
void combinations(const std::string &s, size_t n, std::vector<std::string> &out) {
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; i++) idx[i] = i;
    if (n > s.size()) return;
    while (true) {
        std::string x;
        for (size_t i : idx) x += s[i];
        out.push_back(x);
        long i = (long)n - 1;
        while (i >= 0 && idx[i] == i + s.size() - n) i--;
        if (i < 0) return;
        idx[i]++;
        for (size_t j = i + 1; j < n; j++) idx[j] = idx[j - 1] + 1;
    }
}

struct Cover { std::string file, roof, fade; };

// _roof_for: the darkness cover for a cell whose sides `missing` have no room
bool roof_for(Ctx &c, const std::string &level, const std::string &missing, const std::pair<std::string, std::string> *own_fade,
              const std::vector<std::string> &corners, Cover &out) {
    const auto &files = roof_variants(c);
    char own[64];
    snprintf(own, sizeof own, "map/DungeonRoofLv%d.bfres", atoi(level.substr(2, 2).c_str()));
    std::vector<std::string> order;
    if (roof_file(c, own)) order.push_back(own);
    for (const auto &kv : files)
        if (kv.first != own) order.push_back(kv.first);
    static const char *const pairs[4][3] = {{"U", "L", "UL"}, {"U", "R", "UR"}, {"D", "L", "DL"}, {"D", "R", "DR"}};
    for (long n = (long)missing.size(); n >= 0; n--) {
        std::vector<std::string> combos;
        combinations(missing, (size_t)n, combos);
        for (const std::string &ext : combos) {
            bool skip = false;
            for (const auto &p : pairs)
                if (ext.find(p[0]) != std::string::npos && ext.find(p[1]) != std::string::npos &&
                    std::find(corners.begin(), corners.end(), p[2]) != corners.end())
                    skip = true;
            if (skip) continue;
            std::string roof = ext.empty() ? "DungeonRoof" : "DungeonRoof_ex" + ext;
            std::string fade = ext.empty() ? "DungeonFade" : "DungeonFade_ex" + ext;
            if (own_fade) {
                const std::set<std::string> *f = roof_file(c, own_fade->first);
                if (f && f->count(roof)) { out = {own_fade->first, roof, own_fade->second}; return true; }
                continue;
            }
            for (const std::string &f : order) {
                const std::set<std::string> &m = *roof_file(c, f);
                if (m.count(roof) && m.count(fade)) { out = {f, roof, fade}; return true; }
            }
            for (const std::string &f : order) {
                const std::set<std::string> &m = *roof_file(c, f);
                if (m.count(roof)) { out = {f, roof, m.count("DungeonFade") ? "DungeonFade" : fade}; return true; }
            }
        }
    }
    return false;
}

bool starts_with(const std::string &s, const std::string &p) { return !s.compare(0, p.size(), p); }

// _fix_room: stamp the cell's camera zone, renumber the flags, drop warp pads, fit the darkness cover
void fix_room(Ctx &c, const std::string &cell, const std::string &src, const long *zone_id, Flags *flags,
              const std::string &missing, bool unit_whole, const std::vector<std::string> &corners,
              const std::vector<std::pair<int, int>> &others) {
    const std::string p = leb_path(cell);
    std::string data = c.out_has(p) ? c.b.files().at(p) : c.overlay(leb_path(src));
    LebRoom r = LebRoom::parse(data);
    if (src != cell) remove_actors_if(r, [](const LebActor &a) { return a.type == WARP_PAD; });
    for (LebActor &a : r.actors) {
        if (a.type != ROOF) continue;
        std::string own[3] = {param_str(a.params[0]), param_str(a.params[1]), param_str(a.params[2])};
        bool generic_roof = own[1] == "DungeonRoof" || starts_with(own[1], "DungeonRoof_ex");
        bool generic_fade = own[2] == "DungeonFade" || starts_with(own[2], "DungeonFade_ex");
        bool keep_roof = unit_whole && !generic_roof && !covers(c, own[0], own[1], others);
        bool keep_fade = unit_whole && !generic_fade && !covers(c, own[0], own[2], others);
        if (keep_roof && keep_fade) continue;
        std::pair<std::string, std::string> of = {own[0], own[2]};
        Cover cv;
        bool got = roof_for(c, rooms::level_of(cell), missing, keep_fade ? &of : nullptr, corners, cv);
        if (!got && keep_fade) got = roof_for(c, rooms::level_of(cell), missing, nullptr, corners, cv);
        if (got) {
            a.params[0] = LebParam::of_str(cv.file), a.params[1] = LebParam::of_str(cv.roof);
            a.params[2] = LebParam::of_str(cv.fade);
        }
    }
    for (LebActor &a : r.actors) {
        if (zone_id) a.room_id = (uint32_t)*zone_id;
        if (flags)
            for (auto &sw : a.switches) {
                if (sw.first == 1) sw.second = (uint16_t)flags->glob(sw.second);
                else if (sw.first == 0) sw.second = (uint16_t)flags->local(rooms::level_of(src), sw.second);
            }
    }
    c.b.write_bytes(p, r.repack());
}

// copy_cells: the cells holding a room placed twice (the cell at home - else the first - holds the room itself)
std::set<std::string> copy_cells(const std::vector<std::pair<std::string, std::string>> &layout) {
    std::vector<std::pair<std::string, std::string>> items = layout;
    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
        bool ma = a.first != a.second, mb = b.first != b.second;
        return ma != mb ? !ma : a.first < b.first;
    });
    std::map<std::string, std::string> first;
    for (const auto &kv : items) first.emplace(kv.second, kv.first);
    std::set<std::string> out;
    for (const auto &kv : layout)
        if (first[kv.second] != kv.first) out.insert(kv.first);
    return out;
}

// _own_keys: every actor of the copied rooms gets a key no other room of the level uses
void own_keys(Ctx &c, const std::string &level, const std::set<std::string> &cells) {
    std::set<uint64_t> used;
    const std::string dir = "region_common/level/" + level;
    auto scan = [&](const std::vector<std::string> &names, bool out) {
        for (const std::string &f : names) {
            if (f.size() < 4 || f.compare(f.size() - 4, 4, ".leb") || cells.count(f.substr(0, f.size() - 4))) continue;
            std::string bytes = out ? c.b.files().at(dir + "/" + f) : c.overlay(dir + "/" + f);
            for (const LebActor &a : LebRoom::parse(bytes).actors) used.insert(a.key);
        }
    };
    scan(c.overlay_list(dir), false);
    std::vector<std::string> written;
    for (const std::string &n : c.b.list_written(dir))
        if (c.out_has(dir + "/" + n)) written.push_back(n);
    scan(written, true);
    for (const std::string &cell : cells) {
        const std::string p = dir + "/" + cell + ".leb";
        LebRoom r = LebRoom::parse(c.b.files().at(p));
        for (LebActor &a : r.actors) {
            uint64_t k = ((uint64_t)COPY_KEY_TAG << 56) | (a.key & 0x00FFFFFFFFFFFFFFull);
            while (used.count(k)) k = ((uint64_t)COPY_KEY_TAG << 56) | ((k + (1ull << 32)) & 0x00FFFFFFFFFFFFFFull);
            used.insert(k);
            a.key = k;
            char hex[17];
            snprintf(hex, sizeof hex, "%016llX", (unsigned long long)k);
            a.name = a.name.substr(0, a.name.size() >= 16 ? a.name.size() - 16 : 0) + hex;
        }
        c.b.write_bytes(p, r.repack());
    }
}

// ======== level_builder.py
struct LbRoom {
    std::string name;
    bool fixed = false;
    bool unit_int = true;
    long unit_i = 0;
    std::string unit_s;
    std::string area_data;
    std::shared_ptr<std::string> zone_data;
    bool has_keep = false;
    uint32_t keep_zone = 0;
    std::string unit_str() const { return unit_int ? std::to_string(unit_i) : unit_s; }
    std::string unit_key() const { return (unit_int ? "i" : "s") + unit_str(); }
};

using Starts = std::vector<std::pair<std::string, std::array<double, 4>>>;  // (an ordered dict)

std::array<double, 4> cell_box(const std::string &name) {
    auto o = rooms::cell_origin(name);
    return {o.first, o.second, o.first + 15.0, o.second + 12.0};
}

const double GRID_Y0 = -3.1, GRID_Y1 = 6.0;
const size_t LEAF = 3;

struct Item { std::string name; double x0, z0, x1, z1, y0, y1; };
struct Node { double aabb[6]; size_t count, first; long left, right; };

long bvh_rec(std::vector<Item> its, std::vector<std::string> &order, std::vector<Node> &nodes) {
    size_t i = nodes.size();
    nodes.push_back(Node());
    double x0 = its[0].x0, z0 = its[0].z0, x1 = its[0].x1, z1 = its[0].z1, y0 = its[0].y0, y1 = its[0].y1;
    for (const Item &t : its)
        x0 = std::min(x0, t.x0), z0 = std::min(z0, t.z0), x1 = std::max(x1, t.x1), z1 = std::max(z1, t.z1),
        y0 = std::min(y0, t.y0), y1 = std::max(y1, t.y1);
    size_t first = order.size();
    if (its.size() <= LEAF) {
        for (const Item &t : its) order.push_back(t.name);
        nodes[i] = {{x0, y0, z0, x1, y1, z1}, its.size(), first, -1, -1};
        return (long)i;
    }
    bool ax = (x1 - x0) >= (z1 - z0);
    std::stable_sort(its.begin(), its.end(), [&](const Item &a, const Item &b) {
        return ax ? (a.x0 + a.x1) / 2 < (b.x0 + b.x1) / 2 : (a.z0 + a.z1) / 2 < (b.z0 + b.z1) / 2;
    });
    size_t h = its.size() / 2;
    long left = bvh_rec(std::vector<Item>(its.begin(), its.begin() + h), order, nodes);
    long right = bvh_rec(std::vector<Item>(its.begin() + h, its.end()), order, nodes);
    nodes[i] = {{x0, y0, z0, x1, y1, z1}, its.size(), first, left, right};
    return (long)i;
}

std::string starts_only(FHash &fh, const Starts *starts) {
    if (starts) {
        FHash &st = *fh.get("tagPlayerStart")->sub;
        FHEntry tmpl = st.entries.at(0);
        std::vector<FHEntry> nw;
        for (const auto &s : *starts) {
            FHEntry e = tmpl;
            e.name = s.first, e.name_off = -1;
            std::string d;
            for (double v : s.second) put_f32(d, v);
            e.data = d + tmpl.data.substr(16);
            nw.push_back(e);
        }
        st.entries = nw;
        st.names.clear();
        st.rebuild_index();
    }
    fh.get("staticObject")->sub->entries.clear();  // (static=False)
    return fh.write();
}

std::string build_level(const std::string &base, const std::vector<LbRoom> &rms, const Starts *starts, bool keep_level) {
    FHash fh = FHash::parse(base);
    if (keep_level) return starts_only(fh, starts);
    FHash &area_e = *fh.get("area")->sub, &node_e = *fh.get("node")->sub, &zone_e = *fh.get("zone")->sub;
    const FHEntry tmpl_area = area_e.entries.at(0);
    const FHEntry *tz = nullptr;
    for (const FHEntry &e : zone_e.entries)
        if ((uint8_t)e.data.at(0x9c + 4) == 3) { tz = &e; break; }
    const FHEntry tmpl_zone = tz ? *tz : zone_e.entries.at(0);

    std::map<std::string, const LbRoom *> by_name;
    for (const LbRoom &r : rms) by_name[r.name] = &r;
    auto box = [&](const LbRoom &r) -> std::array<double, 4> {
        if (r.fixed) {
            const std::string &a = r.area_data;
            return {f32_at(a, 0), f32_at(a, 8), f32_at(a, 12), f32_at(a, 20)};
        }
        return cell_box(r.name);
    };
    std::vector<Item> items;
    for (const LbRoom &r : rms) {
        auto b = box(r);
        double y0 = GRID_Y0, y1 = GRID_Y1;
        if (r.fixed) y0 = std::min((double)f32_at(r.area_data, 4), GRID_Y0), y1 = std::max((double)f32_at(r.area_data, 16), GRID_Y1);
        items.push_back({r.name, b[0], b[1], b[2], b[3], y0, y1});
    }
    std::vector<std::string> order;
    std::vector<Node> nodes;
    bvh_rec(items, order, nodes);
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < order.size(); i++) idx[order[i]] = i;

    // areas
    std::vector<FHEntry> areas;
    for (const std::string &n : order) {
        const LbRoom &r = *by_name[n];
        std::string d = r.area_data.empty() ? tmpl_area.data : r.area_data;
        if (!r.fixed) {
            double ox = f32_at(d, 0), oz = f32_at(d, 8);
            auto cb = cell_box(n);
            double dx = cb[0] - ox, dz = cb[1] - oz;
            for (size_t o : {(size_t)0, (size_t)0x18}) {
                double v[6];
                for (int k = 0; k < 6; k++) v[k] = f32_at(d, o + 4 * k);
                double nv[6] = {v[0] + dx, v[1], v[2] + dz, v[3] + dx, v[4], v[5] + dz};
                for (int k = 0; k < 6; k++) put_f32_at(d, o + 4 * k, nv[k]);
            }
            put_u32_at(d, 0x34, crc32_of(n));
        }
        FHEntry e = tmpl_area;
        e.name = n, e.data = d, e.name_off = -1;
        areas.push_back(e);
    }
    const std::string lv = order.empty() ? std::string() : rooms::level_of(order[0]);
    std::string names;
    for (FHEntry &e : areas) {
        size_t path_off = names.size();
        std::string path = "level/" + (e.name.find('_') != std::string::npos ? rooms::level_of(e.name) : lv) + "/" + e.name + ".leb";
        names += path, names.push_back(0);
        e.name_off = (long)names.size();
        names += e.name, names.push_back(0);
        put_u32_at(e.data, 0x30, (uint32_t)path_off);
    }
    area_e.entries = areas;
    area_e.names = names;
    area_e.rebuild_index();

    // the tree
    const FHEntry tmpl_node = node_e.entries.at(0);
    std::vector<FHEntry> nn;
    for (const Node &nd : nodes) {
        std::string d = tmpl_node.data;
        for (int k = 0; k < 6; k++) put_f32_at(d, 4 * k, nd.aabb[k]);
        put_u32_at(d, 0x18, (uint32_t)((nd.count << 16) | nd.first));
        put_u32_at(d, 0x1C, nd.left < 0 ? 0xFFFFFFFFu : (uint32_t)((nd.right << 16) | nd.left));
        FHEntry e = tmpl_node;
        e.data = d;
        nn.push_back(e);
    }
    node_e.entries = nn;

    // camera zones, one per unit, sorted by id
    std::vector<std::pair<const LbRoom *, std::vector<std::string>>> units;  // (first room, names) in first-seen order
    std::map<std::string, size_t> unit_at;
    for (const LbRoom &r : rms) {
        auto it = unit_at.find(r.unit_key());
        if (it == unit_at.end()) unit_at[r.unit_key()] = units.size(), units.push_back({&r, {r.name}});
        else units[it->second].second.push_back(r.name);
    }
    auto unit_zid = [&](const std::pair<const LbRoom *, std::vector<std::string>> &u) -> long {
        const LbRoom &r0 = *by_name[u.second[0]];
        if (r0.has_keep) return r0.keep_zone;
        return u.first->unit_int ? u.first->unit_i : (1L << 30);
    };
    std::stable_sort(units.begin(), units.end(), [&](const auto &a, const auto &b) {
        long za = unit_zid(a), zb = unit_zid(b);
        if (za != zb) return za < zb;
        return a.first->unit_str() < b.first->unit_str();
    });
    std::vector<FHEntry> zones;
    std::map<std::string, size_t> zone_of;
    for (size_t zi = 0; zi < units.size(); zi++) {
        const auto &u = units[zi];
        double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
        for (const std::string &n : u.second) {
            auto b = box(*by_name[n]);
            x0 = std::min(x0, b[0]), z0 = std::min(z0, b[1]), x1 = std::max(x1, b[2]), z1 = std::max(z1, b[3]);
            zone_of[n] = zi;
        }
        const LbRoom &r0 = *by_name[u.second[0]];
        std::string d = r0.zone_data && !r0.zone_data->empty() ? *r0.zone_data : tmpl_zone.data;
        long zid = r0.has_keep ? (long)r0.keep_zone : (u.first->unit_int && u.first->unit_i >= 300 ? u.first->unit_i : 300 + (long)zi);
        put_u32_at(d, 0, (uint32_t)zid);
        if (!r0.fixed) {
            double cy = f32_at(d, 8), hy = f32_at(d, 20);
            double v[6] = {(x0 + x1) / 2, cy, (z0 + z1) / 2, (x1 - x0) / 2, hy, (z1 - z0) / 2};
            for (int k = 0; k < 6; k++) put_f32_at(d, 4 + 4 * k, v[k]);
        }
        std::vector<size_t> ids;
        for (const std::string &n : u.second) ids.push_back(idx[n]);
        std::sort(ids.begin(), ids.end());
        if (ids.size() > 12) ids.resize(12);
        d[0x9c + 6] = (char)ids.size();
        for (size_t k = 0; k < 12; k++) d[0x9c + 7 + k] = (char)(k < ids.size() ? ids[k] : 0xFF);
        FHEntry e = tmpl_zone;
        e.data = d;
        zones.push_back(e);
    }
    zone_e.entries = zones;
    for (FHEntry &e : area_e.entries) {
        std::string &d = e.data;
        auto z = zone_of.find(e.name);
        size_t zi = z == zone_of.end() ? 0 : z->second;
        d[0x3C] = (char)(zi & 0xFF), d[0x3D] = (char)((zi >> 8) & 0xFF);
        if (!by_name[e.name]->fixed) {
            std::string rc = e.name.substr(e.name.rfind('_') + 1, 3);
            d[0x3E] = (char)(atoi(rc.substr(0, 2).c_str()) - 1);
            d[0x3F] = (char)(rc[2] - 'A');
            for (size_t off : {(size_t)0x04, (size_t)0x1C})
                if (f32_at(d, off) > -0.5) put_f32_at(d, off, -0.5);
        }
    }
    return starts_only(fh, starts);
}

// ======== dungeon_gen.write
using Layout = std::vector<std::pair<std::string, std::string>>;  // (cell, room) in the plan's order

const std::string *layout_get(const Layout &L, const std::string &cell) {
    for (const auto &kv : L)
        if (kv.first == cell) return &kv.second;
    return nullptr;
}

// _moved_starts (LINK_LEVELS): a moved room of the level takes its player starts along
Starts moved_starts(const std::string &level, const Layout &layout, const Starts &starts) {
    Starts out;
    if (level != LINK_LEVEL) return out;
    for (const auto &kv : layout) {
        const std::string &cell = kv.first, &src = kv.second;
        if (src == cell || rooms::level_of(src) != level) continue;
        auto so = rooms::cell_origin(src), to = rooms::cell_origin(cell);
        for (const auto &s : starts) {
            double x = s.second[0], y = s.second[1], z = s.second[2], rot = s.second[3];
            bool have = false;
            for (const auto &o : out) have = have || o.first == s.first;
            if (so.first <= x && x < so.first + 15 && so.second <= z && z < so.second + 12 && !have)
                out.push_back({s.first, {x + to.first - so.first, y, z + to.second - so.second, rot}});
        }
    }
    return out;
}

GSheet read_sheet_at(Ctx &c, const std::string &rel) { return GSheet::parse(c.latest(rel)); }

// _dungeon_map_rooms: UiDungeonArea rows for the layout's cells; UiDungeonStair pairs with a missing room dropped
void dungeon_map_rooms(Ctx &c, const Layout &layout, const std::string &entrance) {
    const std::string level = rooms::level_of(entrance);
    std::set<std::string> names;
    for (const auto &kv : layout) names.insert(kv.first);
    for (const std::string &n : offgrid_rooms(c, level)) names.insert(n);
    const std::string rel = "region_common/datasheets/UiDungeonArea.gsheet";
    GSheet sheet = read_sheet_at(c, rel);
    const GStruct *tp = nullptr;
    for (const GStruct &x : sheet.values)
        if (at(x, "mLeaName").s == entrance) { tp = &x; break; }
    if (!tp) throw std::runtime_error("rooms: no UiDungeonArea row for " + entrance);
    const GStruct tmpl = *tp;
    std::vector<GStruct> keep;
    for (const GStruct &x : sheet.values) {
        const std::string &n = at(x, "mLeaName").s;
        if (!starts_with(n, level + "_") || names.count(n)) keep.push_back(x);
    }
    sheet.values = keep;
    std::map<std::string, GStruct> rows;
    for (const GStruct &x : sheet.values) rows[at(x, "mLeaName").s] = x;
    if (level == LINK_LEVEL) {
        for (GStruct &x : sheet.values) {
            const std::string n = at(x, "mLeaName").s;
            std::string src = floor_room(level, n, layout_get(layout, n));
            auto it = rows.find(src);
            if (src != n && it != rows.end()) {
                at(x, "mFloor") = at(it->second, "mFloor");
                at(x, "mAnimFloorIndex") = at(it->second, "mAnimFloorIndex");
            }
        }
    }
    for (const std::string &cell : names) {
        if (rows.count(cell)) continue;
        std::string fr = floor_room(level, cell, layout_get(layout, cell));
        auto it = rows.find(fr);
        GStruct e = it != rows.end() ? it->second : tmpl;
        at(e, "mLeaName") = GValue::of_str(cell);
        at(e, "mIsBoss") = GValue::of_bool(false);
        sheet.values.push_back(e);
    }
    c.b.write_bytes(rel, sheet.write());
    const std::string rel2 = "region_common/datasheets/UiDungeonStair.gsheet";
    GSheet st = read_sheet_at(c, rel2);
    std::vector<GStruct> kept;
    for (const GStruct &x : st.values) {
        bool ok = true;
        for (const char *f : {"mFirstLeaName", "mSceondLeaName"}) {
            const std::string &r = at(x, f).s;
            ok = ok && (!starts_with(r, level + "_") || names.count(r));
        }
        if (ok) kept.push_back(x);
    }
    st.values = kept;
    c.b.write_bytes(rel2, st.write());
}

// _dungeon_map_images: each cell's dungeon-map image (ui/DgnTex.arc) from its source room's
void dungeon_map_images(Ctx &c, const Layout &layout) {
    const std::string rel = "region_common/ui/DgnTex.arc", name = "timg/__Combined.bntx";
    Sarc arc = Sarc::parse(c.latest(rel));
    const std::string *bp = arc.get(name);
    if (!bp) throw std::runtime_error("rooms: no DgnTex bank");
    std::string data = *bp;
    Bntx bank = Bntx::parse(data);
    std::map<std::string, const BntxTexture *> texs;
    for (const BntxTexture &t : bank.textures) texs[t.name] = &t;
    auto same_shape = [&](const std::string &a, const std::string &b) {
        const BntxTexture &x = *texs.at(a), &y = *texs.at(b);
        return x.format == y.format && x.width == y.width && x.height == y.height && x.num_mips == y.num_mips &&
               x.image_size == y.image_size;
    };
    std::vector<std::pair<std::string, std::string>> set_from, nw;
    Layout sorted_l = layout;
    std::sort(sorted_l.begin(), sorted_l.end());
    for (const auto &kv : sorted_l) {
        const std::string &cell = kv.first;
        std::string srcroom = kv.second;
        if (!texs.count(srcroom) || !texs.count(srcroom + "_Mask")) {
            if (texs.count(cell)) continue;
            const std::string pre = rooms::level_of(cell) + "_";
            bool found = false;
            for (const auto &t : texs)  // (sorted)
                if (starts_with(t.first, pre) && texs.count(t.first + "_Mask")) { srcroom = t.first, found = true; break; }
            if (!found) throw std::runtime_error("rooms: no map image for " + cell);  // (next() of nothing)
        }
        if (texs.count(cell) && texs.count(cell + "_Mask") && same_shape(cell, srcroom) &&
            same_shape(cell + "_Mask", srcroom + "_Mask")) {
            if (srcroom != cell) set_from.push_back({cell, srcroom}), set_from.push_back({cell + "_Mask", srcroom + "_Mask"});
        } else if (srcroom != cell) {
            nw.push_back({srcroom, cell}), nw.push_back({srcroom + "_Mask", cell + "_Mask"});
        }
    }
    data = bntx_patch_in_place(data, set_from);
    if (!nw.empty()) data = bntx_copy_renamed(data, nw);
    arc.put(name, data);
    c.b.write_bytes(rel, arc.write());
}

// _level_textures: the level's map bank = exactly the textures its rooms' models (and the roof models) reference
void level_textures(Ctx &c, const std::string &level, const Layout &layout) {
    const std::string mapdir = "region_common/map/";
    std::set<std::string> models;
    for (const auto &kv : layout) models.insert(kv.second);
    for (const std::string &n : offgrid_rooms(c, level)) models.insert(n);
    for (const std::string &f : c.b.list_game("region_common/map"))
        if (starts_with(f, "DungeonRoof") && f.size() > 6 && !f.compare(f.size() - 6, 6, ".bfres")) models.insert(f.substr(0, f.size() - 6));
    std::unordered_set<std::string> only;
    for (const std::string &m : models) {
        auto it = c.model_textures.find(m);
        if (it == c.model_textures.end()) {
            std::string d;
            std::set<std::string> names;
            if (c.overlay_read(mapdir + m + ".bfres", d)) names = mi_names(d);
            it = c.model_textures.emplace(m, names).first;
        }
        only.insert(it->second.begin(), it->second.end());
    }
    std::set<std::string> others;
    for (const auto &kv : layout)
        if (rooms::level_of(kv.second) != level) others.insert(rooms::level_of(kv.second));
    std::vector<std::string> levels = {level};
    levels.insert(levels.end(), others.begin(), others.end());
    // (the overlay's map folder is the game's: the sources are game files, so the runtime can build the bank when
    // the game reads it - SeedBuild::write_deferred)
    DeferredFile f;
    for (const std::string &lv : levels) f.sources.push_back(mapdir + lv + ".bntx");
    f.only.assign(only.begin(), only.end());
    std::sort(f.only.begin(), f.only.end());
    c.b.write_deferred(mapdir + level + ".bntx", f);
}

void write_level(Ctx &c, const std::string &level, const Layout &layout, const std::string &entrance, FlagMap &shared,
                 const std::vector<std::string> &jump_rows) {
    const auto home = source_records(c, entrance);
    std::set<std::string> unmoved;
    for (const auto &kv : layout)
        if (kv.first == kv.second) unmoved.insert(kv.first);
    std::set<uint32_t> reserved, kept_global, kept_zones;
    for (const std::string &cell : unmoved)
        for (const LebActor &a : LebRoom::parse(c.overlay(leb_path(cell))).actors) {
            for (const auto &sw : a.switches) {
                if (sw.first == 0) reserved.insert(sw.second);
                if (sw.first == 1) kept_global.insert(sw.second);
            }
            kept_zones.insert(a.room_id);
        }
    for (const std::string &n : offgrid_rooms(c, level))
        for (const LebActor &a : LebRoom::parse(c.overlay(leb_path(n))).actors) kept_zones.insert(a.room_id);
    long first_zone = 300;
    for (uint32_t z : kept_zones)
        if (z < 0x10000) first_zone = std::max(first_zone, (long)z + 1);
    std::vector<std::string> cells_sorted;
    for (const auto &kv : layout) cells_sorted.push_back(kv.first);
    std::sort(cells_sorted.begin(), cells_sorted.end());
    std::map<std::string, long> zone_ids;
    for (size_t i = 0; i < cells_sorted.size(); i++) zone_ids[cells_sorted[i]] = first_zone + (long)i;
    if (level == LINK_LEVEL) {
        zone_ids.clear();
        const auto &lc = level_cells(c, level);
        for (const auto &kv : layout) zone_ids[kv.first] = (long)lc.at(kv.first);
    }
    FlagMap flags(shared.next_g, reserved, kept_global, crystal_levels());
    std::unique_ptr<CrystalFlagOnly> kept_flags;
    if (!flags.crystal.count(level)) {
        bool crystal_in = false;
        for (const auto &kv : layout) {
            auto it = catalog().find(kv.second);
            crystal_in = crystal_in || (it != catalog().end() && it->second.crystal);
        }
        if (crystal_in) kept_flags.reset(new CrystalFlagOnly(flags));
    }
    const std::set<std::string> copies = copy_cells(layout);
    std::set<std::pair<int, int>> here;
    for (const auto &kv : layout) here.insert(rooms::cell_pos(kv.first));
    std::vector<LbRoom> rms;
    Layout sorted_l = layout;
    std::sort(sorted_l.begin(), sorted_l.end());
    for (const auto &kv : sorted_l) {
        const std::string &cell = kv.first, &src = kv.second;
        if (src != cell) transplant(c, src, cell);
        auto cp = rooms::cell_pos(cell);
        int c0 = cp.first, r0 = cp.second;
        std::string missing;
        const struct { char l; int dc, dr; } sides[4] = {{'U', 0, -1}, {'D', 0, 1}, {'L', -1, 0}, {'R', 1, 0}};
        for (const auto &s : sides)
            if (!here.count({c0 + s.dc, r0 + s.dr})) missing.push_back(s.l);
        bool keep = unmoved.count(cell) != 0;
        auto ci = catalog().find(src);
        std::vector<std::string> unit = ci != catalog().end() ? ci->second.unit : std::vector<std::string>{src};
        auto sp = rooms::cell_pos(src);
        int sc = sp.first, sr = sp.second;
        bool whole = unit.size() > 1;
        for (const std::string &u : unit) {
            if (!whole) break;
            auto up = rooms::cell_pos(u);
            const std::string *at_cell = layout_get(layout, rooms::cell_name(level, c0 + up.first - sc, r0 + up.second - sr));
            whole = at_cell && *at_cell == u;
        }
        std::vector<std::string> corners;
        const struct { const char *k; int dc, dr; } diag[4] = {{"UL", -1, -1}, {"UR", 1, -1}, {"DL", -1, 1}, {"DR", 1, 1}};
        for (const auto &d : diag)
            if (here.count({c0 + d.dc, r0 + d.dr})) corners.push_back(d.k);
        flags.keep = rooms::level_of(src) == level && !copies.count(cell) ? kept_global : std::set<uint32_t>();
        flags.scoped = copies.count(cell) != 0, flags.scope = cell;
        std::set<std::pair<int, int>> sibs;
        if (whole)
            for (const std::string &u : unit) {
                auto up = rooms::cell_pos(u);
                sibs.insert({up.first - sc, up.second - sr});
            }
        const std::string src_lv = rooms::level_of(src);
        std::set<std::pair<int, int>> src_cells;
        for (const auto &k : catalog())
            if (rooms::level_of(k.first) == src_lv) src_cells.insert(rooms::cell_pos(k.first));
        for (size_t i = 0; i < DEAD_ROOMS_SIZE; i++)
            if (rooms::level_of(DEAD_ROOMS[i]) == src_lv) src_cells.insert(rooms::cell_pos(DEAD_ROOMS[i]));
        std::vector<std::pair<int, int>> others;
        for (int dc = -1; dc <= 1; dc++)
            for (int dr = -1; dr <= 1; dr++)
                if ((dc || dr) && here.count({c0 + dc, r0 + dr}) && !sibs.count({dc, dr}) && !src_cells.count({sc + dc, sr + dr}))
                    others.push_back({dc, dr});
        long zid = zone_ids.at(cell);
        Flags *fl = keep ? (Flags *)kept_flags.get() : (Flags *)&flags;
        fix_room(c, cell, src, keep ? nullptr : &zid, fl, missing, whole, corners, others);
        auto rec = source_records(c, src);
        LbRoom r;
        r.name = cell, r.unit_int = true, r.unit_i = zone_ids.at(cell), r.area_data = rec.first, r.zone_data = home.second;
        if (keep && rec.second) {
            uint32_t z = u32_at(*rec.second, 0);
            r.unit_int = false, r.unit_s = "z" + std::to_string(z), r.zone_data = rec.second, r.has_keep = true, r.keep_zone = z;
        }
        rms.push_back(r);
    }
    flags.scoped = false;
    if (!copies.empty()) own_keys(c, level, copies);
    for (const std::string &n : offgrid_rooms(c, level)) {
        auto rec = source_records(c, n);
        LbRoom r;
        r.name = n, r.fixed = true, r.area_data = rec.first, r.unit_int = false, r.unit_s = n;
        if (rec.second) {
            uint32_t z = u32_at(*rec.second, 0);
            r.unit_s = "z" + std::to_string(z), r.zone_data = rec.second, r.has_keep = true, r.keep_zone = z;
        }
        rms.push_back(r);
    }
    const std::string base = c.overlay(lvb_path(level));
    FHash fh = FHash::parse(base);
    Starts starts;
    for (const FHEntry &e : fh.get("tagPlayerStart")->sub->entries)
        starts.push_back({e.name, {f32_at(e.data, 0), f32_at(e.data, 4), f32_at(e.data, 8), f32_at(e.data, 12)}});
    for (const auto &m : moved_starts(level, layout, starts))  // (starts.update)
        for (auto &s : starts)
            if (s.first == m.first) s.second = m.second;
    for (const auto &kv : layout) {  // (setdefault: one start at each cell's centre)
        bool have = false;
        for (const auto &s : starts) have = have || s.first == kv.first;
        if (have) continue;
        auto o = rooms::cell_origin(kv.first);
        starts.push_back({kv.first, {o.first + 7.5, 0.0, o.second + 6.0, 0.0}});
    }
    c.b.write_bytes(lvb_path(level), build_level(base, rms, &starts, level == LINK_LEVEL));
    shared.next_g = flags.next_g;
    dungeon_map_rooms(c, layout, entrance);
    for (const std::string &row : jump_rows) c.b.table_append("door_jumps.tsv", row);
    dungeon_map_images(c, layout);
    level_textures(c, level, layout);
}

// str() of a JSON value as the Python held it
std::string py_str(yyjson_val *v) {
    if (yyjson_is_str(v)) return yyjson_get_str(v);
    if (yyjson_is_bool(v)) return yyjson_get_bool(v) ? "True" : "False";
    if (yyjson_is_null(v)) return "None";
    if (yyjson_is_real(v)) return py_float_repr(yyjson_get_real(v));
    if (yyjson_is_uint(v)) return std::to_string(yyjson_get_uint(v));
    if (yyjson_is_sint(v)) return std::to_string(yyjson_get_sint(v));
    throw std::runtime_error("rooms: a door jump value the build can't print");
}

std::string fmt_g(double v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// dungeon_gen.write_all (from las_headless.shuffle_dungeon_rooms), then the pots / bushes tables it moved
void write_all(SeedBuild &b) {
    yyjson_val *plan = b.decisions().raw("room-plan");
    if (!plan) throw std::runtime_error("dungeon room shuffle: the seed's decisions have no room plan");
    Ctx c(b);
    std::vector<std::pair<std::string, Layout>> layouts;
    size_t i, n, j, m;
    yyjson_val *lv;
    yyjson_arr_foreach(yyjson_obj_get(plan, "layouts"), i, n, lv) {
        Layout L;
        yyjson_val *pair;
        yyjson_arr_foreach(yyjson_arr_get(lv, 1), j, m, pair)
            L.push_back({yyjson_get_str(yyjson_arr_get(pair, 0)), yyjson_get_str(yyjson_arr_get(pair, 1))});
        layouts.push_back({yyjson_get_str(yyjson_arr_get(lv, 0)), L});
    }
    yyjson_val *jumps = yyjson_obj_get(plan, "jumps");
    bool have_jumps = jumps && yyjson_is_arr(jumps);
    std::vector<std::pair<std::string, std::string>> rows;  // (level, its row)
    if (have_jumps) {
        yyjson_val *row;
        yyjson_arr_foreach(jumps, i, n, row) {
            std::string line;
            size_t k, kn;
            yyjson_val *v;
            yyjson_arr_foreach(row, k, kn, v) line += (k ? "\t" : "") + py_str(v);
            rows.push_back({py_str(yyjson_arr_get(row, 0)), line + "\n"});
        }
    }
    if (!have_jumps) throw std::runtime_error("dungeon room shuffle: plans without door jumps aren't built");  // (_jumps)
    b.table_write("door_jumps.tsv", "");
    b.table_write("flags_ext", "chaos dungeon map letters: event flags 3800+\n");
    int start = GLOBAL_FLAG_BASE;
    const std::string sheet_path = "region_common/datasheets/GlobalFlags.gsheet";
    if (c.out_has(sheet_path))
        for (const GStruct &v : GSheet::parse(b.files().at(sheet_path)).values) {
            int u = at(v, "index").i;
            if (u < 3800) start = std::max(start, u + 1);
        }
    FlagMap shared((uint32_t)start, {}, {}, {});
    for (const auto &l : layouts) {
        std::vector<std::string> mine;
        for (const auto &r : rows)
            if (r.first == l.first) mine.push_back(r.second);
        write_level(c, l.first, l.second, entrances().at(l.first), shared, mine);
    }
    std::string map_tsv, rooms_tsv;
    for (const auto &l : layouts) {
        Layout s = l.second;
        std::sort(s.begin(), s.end());
        for (const auto &kv : s) {
            auto sh = map_shift(c, l.first, floor_room(l.first, kv.first, &kv.second));
            map_tsv += l.first + "\t" + kv.first.substr(kv.first.rfind('_') + 1) + "\t" + fmt_g(sh.first) + "\t" + fmt_g(sh.second) + "\n";
        }
    }
    b.table_write("dungeon_map.tsv", map_tsv);
    for (const auto &l : layouts) {
        const std::set<std::string> copies = copy_cells(l.second);
        Layout s = l.second;
        std::sort(s.begin(), s.end());
        for (const auto &kv : s)
            rooms_tsv += l.first + "\t" + kv.first.substr(kv.first.rfind('_') + 1) + "\t" + kv.second +
                         (copies.count(kv.first) ? "\tcopy" : "") + "\n";
    }
    b.table_write("dungeon_rooms.tsv", rooms_tsv);
    if ((int)shared.next_g > start) {
        GSheet sheet = GSheet::parse(c.latest(sheet_path));
        for (uint32_t k = (uint32_t)start; k < shared.next_g; k++)
            sheet.values.push_back({{"symbol", las::gs("LARDungeon" + std::to_string(k))}, {"index", las::gi((int32_t)k)}});
        b.write_bytes(sheet_path, sheet.write());
    }
    if (shared.next_g > 2048) b.table_append("flags_ext", "dungeon generator: event flags up to " + std::to_string(shared.next_g) + "\n");
    // las_headless: gold pots / bushes are found by position - moved rooms moved their item actors too
    yyjson_val *call;
    yyjson_arr_foreach(yyjson_obj_get(b.decisions().raw("ours-world"), "pots"), i, n, call) {
        const char *p = yyjson_get_str(yyjson_obj_get(call, "prefix"));
        if (p && !strcmp(p, "LARPot")) b.table_write("pots.tsv", las::pots_tsv(b, "LARPot", 0.0));
    }
    yyjson_arr_foreach(yyjson_obj_get(b.decisions().raw("ours-world"), "pots"), i, n, call) {
        const char *p = yyjson_get_str(yyjson_obj_get(call, "prefix"));
        if (p && !strcmp(p, "LARBush")) b.table_write("bushes.tsv", las::pots_tsv(b, "LARBush", 0.4));
    }
}

}  // namespace

std::vector<BuildStep> steps_rooms() {
    // (its writes are named by the function that made each: the rooms first, then each level's files, the flags)
    return {{"rooms.transplant>dungeon_gen._fix_room|rooms.transplant|dungeon_gen.write|dungeon_gen._own_keys|"
             "dungeon_gen._dungeon_map_rooms|dungeon_gen._dungeon_map_images|dungeon_gen._level_textures|"
             "dungeon_gen.write_all",
             write_all}};
}

}  // namespace rando
