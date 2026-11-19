// LAS Recomp's world steps: pots, bushes, rocks, digs, apple trees, underwater, seashell spots, entrances, mini-boss
// guards, fast egg, chest sweep, environments: the generator's steps ported (seed_build.h); each makes exactly the writes its Python
// does (lasrando check-steps, tools/rando/test_apply.py). Ported from tools/rando/{pots, bushes, rocks, digs,
// underwater, seashell_spots, apple_trees, entrance_logic, miniboss_guard, fast_egg, chest_looks, environments}.py
// (environments.py's preset lists: LAS-Randomizer dev, MIT, Owen-Splat and contributors).
//
// The lists of checks and the random draws come from the decisions' "ours-world" field
// (tools/rando/record_ours_world.py says what each holds); everything else is worked out from the files, as there.
// In decide mode (decide.h) each step first works its key out as the Python does - the -sanity scans on LAS's logic
// (ours_world_scan.cpp), the entrance shuffle's result, the modules' own random streams - and sets it, then goes on
// as in apply mode. Both modes write the modules' tables (pots.tsv, digs.tsv, shell_spots.tsv, ... and their lines
// in check_flags.tsv / flags_ext), as the Python writes them; the tables a dungeon room shuffle rewrites (pots.tsv,
// bushes.tsv after dungeon_gen) stay as they were before it.
//
// tools/rando/record.py logs a write when its file is closed: environments.apply (and dungeon_gen's rooms) write with
// open(p, 'wb').write(...) and no close, so their writes aren't in the log (environments.apply: "not in this seed's
// run"), and a step after dungeon_gen that reads a room it moved (miniboss_guard) starts from the room before it.
//
// Python quirks kept for the same files (each marked "as the Python"):
//   - digs.apply: the digs' item flag takes the HIGHEST free GlobalFlags index below 4096, not the next one;
//   - pots.apply: an item actor is a copy of its pot, links included (the pot's links to other actors stay on it);
//   - underwater.apply: PatchHeartPiece is the original row copied before the edit, appended after it.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "../solve/value.h"
#include "decide.h"
#include "item_get.h"
#include "las_data.h"
#include "ours_world_lib.h"
#include "steps.h"

namespace rando {
namespace las {
namespace {

using namespace data;

// ---- shared
yyjson_val *ow(SeedBuild &b, const char *key) { return yyjson_obj_get(b.decisions().raw("ours-world"), key); }
std::string jstr(yyjson_val *v) { const char *s = yyjson_get_str(v); return s ? s : ""; }
uint64_t jkey(yyjson_val *v) { return strtoull(jstr(v).c_str(), nullptr, 16); }  // (an actor key, as hex)

std::string room_path(const std::string &level, const std::string &room) {
    return "region_common/level/" + level + "/" + room + ".leb";
}
LebRoom read_room_at(SeedBuild &b, const std::string &path) {
    std::string bytes;
    if (!b.read_bytes(path, bytes)) throw std::runtime_error("ours world: no file " + path);
    return LebRoom::parse(bytes);
}
void write_room_at(SeedBuild &b, const std::string &path, LebRoom &r) { b.write_bytes(path, r.repack()); }

int32_t flag_index(const GStruct &row) { return at(row, "index").i; }
GStruct flag_row(const std::string &symbol, int32_t index) { return {{"symbol", gs(symbol)}, {"index", gi(index)}}; }

// seed_tables.next_flag: after every index the sheet has below the Chaos map letters (3800); one past the game's 2048
// is noted in the seed's flags_ext (the runtime's extended flags)
int32_t next_flag(SeedBuild &b, const GSheet &sheet) {
    int32_t m = INT32_MIN;
    for (const GStruct &v : sheet.values)
        if (flag_index(v) < 3800) m = std::max(m, flag_index(v));
    if (m + 1 >= 3800) throw std::runtime_error("out of GlobalFlags indices");
    if (m + 1 >= 2048) b.table_append("flags_ext", "event flag " + std::to_string(m + 1) + "\n");
    return m + 1;
}

using solve::List;
using solve::Value;

Value parse_value(const std::string &json) { return Value::parse_json(json.data(), json.size()); }
std::string hex_lower(uint64_t key) {  // f'{key:x}'
    char buf[24];
    snprintf(buf, sizeof buf, "%llx", (unsigned long long)key);
    return buf;
}

// Python's float // (floor of the exact quotient, as float_floor_div does it)
double py_floordiv(double a, double b) {
    double mod = fmod(a, b), div = (a - mod) / b;
    if (mod != 0.0 && ((b < 0) != (mod < 0))) div -= 1.0;
    if (div == 0.0) return copysign(0.0, a / b);
    double fl = floor(div);
    return div - fl > 0.5 ? fl + 1.0 : fl;
}

bool starts(const std::string &s, const char *p) { return !s.compare(0, strlen(p), p); }
bool ends(const std::string &s, const char *e) {
    size_t n = strlen(e);
    return s.size() >= n && !s.compare(s.size() - n, n, e);
}

// pots.QUICK: picked up like rupees, no item-get animation
bool quick_item(const std::string &item) {
    static const std::set<std::string> q = {"pot-heart", "apple", "green-apple", "pot-arrows", "pot-bombs", "pot-powder"};
    return q.count(item) > 0;
}

// the item at a check given without the get animation (rupees, QUICK) or with LAS's
std::string give(Flow &f, const SeedBuild::ItemInfo &it, bool quick) {
    if (quick)
        return f.action("Inventory", "AddItemByKey", {{"itemKey", it.key}, {"count", 1}, {"index", it.index}, {"autoEquip", false}});
    return insert_item_get(f, it.key, it.index);
}

// seashell_spots.DROP_KEY: an ItemDrop key Seashell<n> (re.match(rb'Seashell(\d+)$'))
bool drop_key(const LebParam &p) {
    if (p.kind != LebParam::Str) return false;
    std::string s(p.s.begin(), p.s.end());
    if (!starts(s, "Seashell")) return false;
    size_t i = 8, n = s.size();
    if (n > i && s[n - 1] == '\n') n--;  // ($ matches before a final newline)
    if (n == i) return false;
    for (; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
void clear_drop(LebActor &a) {  // seashell_spots.clear_drop
    for (int k = 0; k < 2; k++)
        if (drop_key(a.params[k])) a.params[k] = str_param("None");
}

double model_size(const std::string &model, double dflt) {
    double v;
    return lookup(MODEL_SIZES, model, v) ? v : dflt;
}

// Field_<row><col>: (row, col) (bushes._screen_pos)
std::pair<int, int> screen_pos(const std::string &room) {
    if (room.size() != 9 || !starts(room, "Field_")) throw std::runtime_error("not a Field screen: " + room);
    return {atoi(room.substr(6, 2).c_str()), room[8] - 'A'};
}

// ======== pots.apply (pots, and through it bushes.apply / rocks.apply)

struct PotKind { const char *prefix; int32_t base, limit; uint8_t tag; double hide; bool cut; int drop_param; };
const PotKind kPotKinds[] = {
    {"LARPot", 1240, 2048, 0x7A, 0.7, false, 1},   // pots.py FLAG_BASE, HIDE_SCALE
    {"LARBush", 2048, 4096, 0x7B, 0.4, true, 1},   // bushes.py
    {"LARRock", 2048, 3800, 0x7C, 0.4, true, 0},   // rocks.py
};

void pots_call(SeedBuild &b, const PotKind &k, yyjson_val *locs) {
    GSheet sheet = b.read_sheet("GlobalFlags.gsheet");
    EventFlow fl = b.read_flow("SinkingSword.bfevfl");
    Flow f(b, fl.flowchart);
    int32_t next = k.base;
    for (const GStruct &v : sheet.values)
        if (flag_index(v) >= k.base && flag_index(v) < k.limit) next = std::max(next, flag_index(v) + 1);
    struct Loc { std::string name; uint64_t key; };
    std::map<std::pair<std::string, std::string>, std::vector<Loc>> by_room;  // (level, room): sorted, as the Python
    size_t i, n;
    yyjson_val *l;
    yyjson_arr_foreach(locs, i, n, l) {
        by_room[{jstr(yyjson_arr_get(l, 1)), jstr(yyjson_arr_get(l, 2))}].push_back(
            {jstr(yyjson_arr_get(l, 0)), jkey(yyjson_arr_get(l, 3))});
    }
    std::map<std::tuple<std::string, int, bool>, std::string> tails;
    std::map<std::string, std::set<uint64_t>> level_keys;  // level -> every actor key in its rooms (unique per level)
    auto keys_of = [&](const std::string &level) -> std::set<uint64_t> & {
        auto it = level_keys.find(level);
        if (it != level_keys.end()) return it->second;
        std::set<uint64_t> &ks = level_keys[level];
        const std::string dir = "region_common/level/" + level;
        std::set<std::string> names;
        for (const std::string &nm : b.list_game(dir)) if (ends(nm, ".leb") && nm[0] != '.') names.insert(nm);
        for (const std::string &nm : b.list_written(dir)) if (ends(nm, ".leb") && nm[0] != '.') names.insert(nm);
        for (const std::string &nm : names)
            for (const LebActor &a : read_room_at(b, dir + "/" + nm).actors) ks.insert(a.key);
        return ks;
    };
    for (auto &rm : by_room) {
        const std::string &level = rm.first.first, &room_name = rm.first.second;
        const std::string path = room_path(level, room_name);
        LebRoom room = read_room_at(b, path);
        std::map<uint64_t, size_t> by_key;  // (a dict: the last actor with a key)
        for (size_t a = 0; a < room.actors.size(); a++) by_key[room.actors[a].key] = a;
        for (const Loc &loc : rm.second) {
            auto pk = by_key.find(loc.key);
            if (pk == by_key.end()) throw std::runtime_error(loc.name + ": no " + k.prefix + " actor in " + room_name);
            clear_drop(room.actors[pk->second]);
            if (next >= k.limit) throw std::runtime_error(std::string("out of GlobalFlags indices for ") + k.prefix);
            const std::string flag = k.prefix + std::to_string(next);
            sheet.values.push_back(flag_row(flag, next));
            next++;
            SeedBuild::ItemInfo it = b.item_info(loc.name, true);
            bool quick = starts(it.key, "Rup") || quick_item(placed(b.decisions(), loc.name));
            // (one event per location, then a chain shared by every location holding the same item: the flow file's
            // load buffer is limited)
            auto tk = std::make_tuple(it.key, it.index, quick);
            if (!tails.count(tk)) {
                std::string get = give(f, it, quick);
                tails[tk] = f.action("SinkingSword", "Destroy", {}, get);
            }
            f.add_entry_point(loc.name);
            std::string set = f.action("EventFlags", "SetFlag", {{"symbol", flag}, {"value", true}}, tails[tk]);
            f.insert_after(loc.name, set);
            int32_t cut = -1;
            if (k.cut) {
                if (next >= k.limit) throw std::runtime_error(std::string("out of GlobalFlags indices for ") + k.prefix);
                cut = next;
                sheet.values.push_back(flag_row(k.prefix + std::string("Cut") + std::to_string(cut), cut));
                next++;
            }
            LebActor item = room.actors[pk->second];  // (a copy of the pot, its links too: as the Python)
            std::set<uint64_t> &used = keys_of(level);
            const uint64_t low = 0x00FFFFFFFFFFFFFFull, tag = (uint64_t)k.tag << 56;
            item.key = tag | (item.key & low);
            while (used.count(item.key)) item.key = tag | ((item.key + (1ull << 32)) & low);
            used.insert(item.key);
            char nm[64];
            snprintf(nm, sizeof nm, "SinkingSword-%016llX", (unsigned long long)item.key);
            item.name = Bytes(nm, nm + strlen(nm));
            item.type = 0x194;
            const std::string ps[8] = {it.model_path, it.model_name, loc.name, flag, it.key == "Seashell" ? "true" : "false", "", "", ""};
            for (int p = 0; p < 8; p++) item.params[p] = str_param(ps[p]);
            for (auto &s : item.switches) s = {4, 0};
            if (cut >= 0) {
                LebActor &pot = room.actors[pk->second];
                pot.switches[0] = {1, (uint16_t)cut};
                pot.params[k.drop_param] = str_param("None");  // (ItemDrop key: no random drop beside the check)
            }
            double s = model_size(it.model_name, 1.0) * k.hide;
            item.scale[0] = item.scale[1] = item.scale[2] = s;
            double rot;
            if (lookup(MODEL_ROTATIONS, it.model_name, rot)) item.rot[1] = rot;
            room.actors.push_back(item);
        }
        write_room_at(b, path, room);
    }
    b.write_sheet("GlobalFlags.gsheet", sheet);
    b.write_flow("SinkingSword.bfevfl", fl);
}

// decide mode: las_headless calls pots.apply for the pots, the bushes and the rocks that have locations, each with
// the locations its module's scan + add_to_logic made, in that dict's order
void decide_pots(SeedBuild &b, Decider &dc) {
    Value s = parse_value(sanity_json(b)), calls = Value::new_list();
    const std::pair<const char *, const char *> kinds[] = {{"LARPot", "pots"}, {"LARBush", "bushes"}, {"LARRock", "rocks"}};
    for (const auto &kd : kinds) {
        const Value &locs = s.at(kd.second);
        if (!locs.len()) continue;
        Value call = Value::new_dict(), l = Value::new_list();
        for (const auto &e : locs.dict()) {
            const Value &p = e.value;
            l.list().push_back(Value(List{Value(e.key), p.at("level"), p.at("room"),
                                          Value(hex_lower(strtoull(p.at("key").s().c_str(), nullptr, 16)))}));
        }
        call.set("prefix", kd.first);
        call.set("locs", l);
        calls.list().push_back(call);
    }
    if (calls.len()) dc.set_ours_world("pots", calls.json());
}

void pots(SeedBuild &b) {
    if (Decider *dc = b.decider()) decide_pots(b, *dc);
    size_t i, n;
    yyjson_val *call;
    yyjson_arr_foreach(ow(b, "pots"), i, n, call) {
        const std::string prefix = jstr(yyjson_obj_get(call, "prefix"));
        const PotKind *k = nullptr;
        for (const PotKind &x : kPotKinds)
            if (prefix == x.prefix) k = &x;
        if (!k) throw std::runtime_error("pots: unknown kind " + prefix);
        pots_call(b, *k, yyjson_obj_get(call, "locs"));
        // the module's tables after its call: its write_tsv over the seed's rooms, and bushes.apply's / rocks.apply's
        // flags_ext note (bushes.apply opens it with 'w': what was there before is gone, as the Python)
        if (k->cut) {
            if (prefix == "LARBush") b.table_write("flags_ext", "bushsanity: event flags 2048+ (runtime/hle/flags_ext.c)\n");
            else b.table_append("flags_ext", "rocksanity: event flags 2048+ (runtime/hle/flags_ext.c)\n");
        }
        const std::string table = prefix == "LARPot" ? "pots.tsv" : prefix == "LARBush" ? "bushes.tsv" : "rocks.tsv";
        b.table_write(table, pots_tsv(b, k->prefix, k->cut ? k->hide : 0.0));
    }
}

// ======== digs.apply

int dig_tile_id(const std::string &room, int tile) {
    auto rc = screen_pos(room);
    return ((rc.first - 1) * 16 + rc.second) * 80 + tile;
}
int actor_tile(const LebActor &a, const std::pair<int, int> &rc) {  // (row-major tile 0-79 an actor stands on)
    int tz = (int)py_floordiv(a.pos[2] - (rc.first - 1) * 12.0, 1.5), tx = (int)py_floordiv(a.pos[0] - rc.second * 15.0, 1.5);
    return tz * 10 + tx;
}

// digs.tsv: every dig tile of the Field by tile id (a check's item columns, '-' elsewhere), and digs.apply's
// flags_ext note (opened with 'w': what was there before is gone, as the Python)
void dig_tables(SeedBuild &b, yyjson_val *locs, const std::map<std::string, std::vector<std::string>> &cols) {
    std::map<int, std::string> by_tile;
    size_t i, n;
    yyjson_val *l;
    yyjson_arr_foreach(locs, i, n, l)
        by_tile[dig_tile_id(jstr(yyjson_arr_get(l, 1)), (int)yyjson_get_sint(yyjson_arr_get(l, 2)))] = jstr(yyjson_arr_get(l, 0));
    std::vector<DigTile> tiles = dig_scan(b);
    std::stable_sort(tiles.begin(), tiles.end(),
                     [](const DigTile &x, const DigTile &y) { return dig_tile_id(x.room, x.tile) < dig_tile_id(y.room, y.tile); });
    std::string out = "# screen\ttile\ty\titem\tmodel\tname\tentry\tshell\tscale\tturn\tblocker (tools/rando/digs.py)\n";
    for (const DigTile &t : tiles) {
        std::vector<std::string> c(7, "-");
        auto name = by_tile.find(dig_tile_id(t.room, t.tile));
        if (name != by_tile.end() && cols.count(name->second)) c = cols.at(name->second);
        out += t.room.substr(t.room.rfind('_') + 1) + "\t" + std::to_string(t.tile) + "\t" + py_g(t.y);
        for (const std::string &x : c) out += "\t" + x;
        out += "\t" + (t.blocker.empty() ? std::string("-") : t.blocker) + "\n";
    }
    b.table_write("digs.tsv", out);
    b.table_write("flags_ext", "digsanity / bushsanity: event flags 2048+ (runtime/hle/flags_ext.c)\n");
}

// decide mode: the digsanity checks (digs.add_to_logic's locations), by tile id
void decide_digs(SeedBuild &b, Decider &dc) {
    Value s = parse_value(sanity_json(b));
    std::vector<std::pair<int, Value>> rows;
    for (const auto &e : s.at("digs").dict()) {
        const Value &v = e.value;
        rows.push_back({dig_tile_id(v.at("room").s(), (int)v.at("tile").i()),
                        Value(List{Value(e.key), v.at("room"), v.at("tile"), v.at("y"), v.at("blocker"), v.get("shell")})});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
    Value out = Value::new_list();
    for (auto &r : rows) out.list().push_back(r.second);
    dc.set_ours_world("digs", out.json());
}

const char *const DIG_SHELLS = "ours_world.dig_shells";  // (carry: digs.SHELLS for seashell_spots.apply)

void digs(SeedBuild &b) {
    if (Decider *dc = b.decider()) decide_digs(b, *dc);
    yyjson_val *locs = ow(b, "digs");
    const Decisions &d = b.decisions();
    // digs.tsv's item columns per check (label, model path, model name, entry, seashell, scale, turn); a buried
    // Secret Seashell's tile has none
    std::map<std::string, std::vector<std::string>> cols;
    std::string shells;  // digs.SHELLS: location, seashell index, found flag (GOT_FLAG_BASE + tile id)
    GSheet sheet = b.read_sheet("GlobalFlags.gsheet");
    std::set<int32_t> used;
    for (const GStruct &v : sheet.values) used.insert(flag_index(v));
    int32_t free_ = -1;
    for (int32_t i = 2048; i < 4096; i++)
        if (!used.count(i)) free_ = i;  // (the highest free one: as the Python)
    if (free_ < 0) throw std::runtime_error("digs: no free flag");
    sheet.values.push_back(flag_row("LARDigItem", free_));
    b.write_sheet("GlobalFlags.gsheet", sheet);
    EventFlow fl = b.read_flow("SinkingSword.bfevfl");
    Flow f(b, fl.flowchart);
    std::map<std::tuple<std::string, int, bool>, std::string> entries;
    struct Buried { int tile; double y; int index; };
    std::map<std::string, std::vector<Buried>> buried;
    std::map<std::string, std::set<int>> taken;
    size_t i, n;
    yyjson_val *l;
    yyjson_arr_foreach(locs, i, n, l) {  // (by tile id)
        const std::string name = jstr(yyjson_arr_get(l, 0)), room = jstr(yyjson_arr_get(l, 1));
        int tile = (int)yyjson_get_sint(yyjson_arr_get(l, 2));
        double y = yyjson_get_num(yyjson_arr_get(l, 3));
        bool blocker = yyjson_is_str(yyjson_arr_get(l, 4));
        if (!yyjson_is_null(yyjson_arr_get(l, 5))) taken[room].insert(tile);
        const std::string item = placed(d, name);
        if (item == "seashell" && !blocker) {  // the game's own buried shell, with the index LAS gave it
            int index = b.item_info(name, true).index;
            buried[room].push_back({tile, y, index});
            shells += name + "\t" + std::to_string(index) + "\t" + std::to_string(4096 + 256 * 80 + dig_tile_id(room, tile)) + "\n";
            continue;
        }
        SeedBuild::ItemInfo it = b.item_info(name, true);
        bool quick = starts(it.key, "Rup") || quick_item(item) || item == "dig-rupee-1" || item == "dig-rupee-5" ||
                     item == "dig-rupee-20";
        auto key = std::make_tuple(it.key, it.index, quick);
        if (!entries.count(key)) {  // one entry per distinct item, shared by every tile holding it
            const std::string entry = "LARDig" + std::to_string(entries.size());
            entries[key] = entry;
            std::string get = give(f, it, quick);
            f.add_entry_point(entry);
            std::string destroy = f.action("SinkingSword", "Destroy", {}, get);
            f.insert_after(entry, destroy);
        }
        static const std::map<std::string, std::string> NAMES = {
            {"dig-rupee-1", "1 Rupee"}, {"dig-rupee-5", "5 Rupees"}, {"dig-rupee-20", "20 Rupees"},
            {"pot-heart", "Recovery Heart"}, {"pot-arrows", "Arrows"}, {"pot-bombs", "Bombs"},
            {"pot-powder", "Magic Powder"}};  // (digs.NAMES)
        auto label = NAMES.find(item);
        double size = 1.0, turn = 0;
        lookup(MODEL_SIZES, it.model_name, size), lookup(MODEL_ROTATIONS, it.model_name, turn);
        cols[name] = {label != NAMES.end() ? label->second : pretty_item(item), it.model_path, it.model_name,
                      entries[key], it.key == "Seashell" ? "1" : "0", py_g(size), py_g(turn)};
    }
    b.write_flow("SinkingSword.bfevfl", fl);
    b.carry()[DIG_SHELLS] = shells;
    dig_tables(b, locs, cols);
    if (buried.empty() && taken.empty()) return;
    std::string tmpl_bytes;
    if (!b.read_game("region_common/level/Field/Field_11F.leb", tmpl_bytes)) throw std::runtime_error("digs: no Field_11F");
    LebRoom tmpl = LebRoom::parse(tmpl_bytes);
    const LebActor *shell = nullptr;
    for (const LebActor &a : tmpl.actors)
        if (a.type == 0x87 && !shell) shell = &a;
    if (!shell) throw std::runtime_error("digs: no Secret Seashell in Field_11F");
    std::set<std::string> rooms;
    for (auto &x : buried) rooms.insert(x.first);
    for (auto &x : taken) rooms.insert(x.first);
    for (const std::string &room : rooms) {
        const std::string path = room_path("Field", room);
        LebRoom r = read_room_at(b, path);
        auto rc = screen_pos(room);
        std::vector<size_t> gone;
        auto tk = taken.find(room);
        for (size_t a = 0; a < r.actors.size(); a++)
            if (r.actors[a].type == 0x87 && tk != taken.end() && tk->second.count(actor_tile(r.actors[a], rc)))
                gone.push_back(a);
        if (!gone.empty()) remove_actors(r, gone);
        auto bu = buried.find(room);
        if (bu != buried.end())
            for (const Buried &s : bu->second) {
                LebActor a = *shell;
                a.key = (0x7Eull << 56) | (uint64_t)dig_tile_id(room, s.tile);
                char nm[64];
                snprintf(nm, sizeof nm, "ItemSecretSe-%016llX", (unsigned long long)a.key);
                a.name = Bytes(nm, nm + strlen(nm));
                a.pos[0] = rc.second * 15.0 + s.tile % 10 * 1.5 + 0.75;  // (digs.tile_xz)
                a.pos[2] = (rc.first - 1) * 12.0 + s.tile / 10 * 1.5 + 0.75;
                a.pos[1] = s.y;
                a.params[0] = LebParam::of_int((uint32_t)s.index);
                r.actors.push_back(a);
            }
        write_room_at(b, path, r);
    }
}

// ======== underwater.apply

void underwater(SeedBuild &b) {
    struct Spot { const char *name, *level, *room; uint16_t vtype; const char *shell_flag; };
    static const Spot kSpots[] = {
        {"bay-passage-sunken", "MadBattersWellEnter", "MadBattersWellEnter_01B", 0xB0, nullptr},
        {"river-crossing-cave", "AnimalVillageEnter", "AnimalVillageEnter_01B", 0xB0, nullptr},
        {"kanalet-moat-south", "Field", "Field_08J", 0xB0, nullptr},
        {"south-bay-sunken", "Field", "Field_15J", 0xB0, nullptr},
        {"taltal-east-drop", "EagleTowerExit", "EagleTowerExit_01A", 0xB0, nullptr},
        {"kanalet-moat-north", "Field", "Field_04I", 0x87, "KanaletSunkenShellGet"},
        {"taltal-sunken", "Field", "Field_02M", 0x87, "TaltalSunkenShellGet"},
    };
    const double RAISE = 0.75;
    const Decisions &d = b.decisions();
    GSheet sheet = b.read_sheet("GlobalFlags.gsheet");
    std::set<std::string> names;
    for (const GStruct &v : sheet.values) names.insert(at(v, "symbol").s);
    for (const char *fl : {"KanaletSunkenShellGet", "TaltalSunkenShellGet"})
        if (!names.count(fl)) sheet.values.push_back(flag_row(fl, next_flag(b, sheet)));
    b.write_sheet("GlobalFlags.gsheet", sheet);

    EventFlow fl = b.read_flow("SinkingSword.bfevfl");
    Flow f(b, fl.flowchart);
    for (const Spot &s : kSpots) {
        if (!d.item_at(s.name)) continue;
        const char *hf = s.shell_flag ? s.shell_flag : lookup(HEART_FLAGS, s.name);
        if (!hf) throw std::runtime_error(std::string("underwater: no flag for ") + s.name);
        const std::string flag = hf;
        SeedBuild::ItemInfo it = b.item_info(s.name, true);
        std::string get = give(f, it, starts(it.key, "Rup"));
        f.add_entry_point(s.name);
        f.action_chain(s.name, {{"SinkingSword", "Destroy", {}}, {"EventFlags", "SetFlag", {{"symbol", flag}, {"value", true}}}}, get);

        const std::string path = room_path(s.level, s.room);
        LebRoom room = read_room_at(b, path);
        LebActor *act = nullptr;
        for (LebActor &a : room.actors)
            if (a.type == s.vtype && !act) act = &a;
        if (!act) throw std::runtime_error(std::string(s.name) + ": no actor in " + s.room);
        act->type = 0xB0;
        act->pos[1] += RAISE;
        if (it.model_name != "HeartPiece" && it.model_name != "HeartContainer") {
            double sz = it.model_name == "FullMoonCello" ? 2.5 : it.model_name == "Mamu" ? 0.75
                        : it.model_name == "ManboTamegoro" ? 0.9 : model_size(it.model_name, 1.5);  // (SIZES)
            act->scale[0] = act->scale[1] = act->scale[2] = sz;
        }
        act->params[1] = str_param(it.model_path);
        act->params[2] = str_param(it.model_name);
        act->params[3] = str_param(s.name);
        act->params[4] = str_param(flag);
        act->params[5] = str_param(it.key == "Seashell" ? "true" : "false");
        double rot;
        if (lookup(MODEL_ROTATIONS, it.model_name, rot)) act->rot[1] = rot;
        write_room_at(b, path, room);
    }
    b.write_flow("SinkingSword.bfevfl", fl);

    // Npc.gsheet: ItemHeartPiece from its parameters; PatchHeartPiece = the original, for heart pieces held up elsewhere
    GSheet npcs = b.read_sheet("Npc.gsheet");
    auto row_of = [&](const char *sym) -> int {
        for (size_t r = 0; r < npcs.values.size(); r++)
            if (at(npcs.values[r], "symbol").s == sym) return (int)r;
        return -1;
    };
    int hpi = row_of("ItemHeartPiece");
    if (hpi < 0) throw std::runtime_error("underwater: no ItemHeartPiece");
    bool patch = row_of("PatchHeartPiece") < 0;
    GStruct copy = npcs.values[hpi];
    at(copy, "symbol") = gs("PatchHeartPiece");
    {
        GStruct &hp = npcs.values[hpi];
        GStruct &gr = at(hp, "graphics").structs.at(0);
        at(gr, "path") = gs("$1");
        at(gr, "model") = gs("$2");
        at(hp, "eventInfo") = gst({{"eventAsset", gs("SinkingSword.bfevfl")}, {"actorName", gs("SinkingSword")}});
        at(item(at(hp, "eventTriggers"), 0), "entryPoint") = gs("$3");
        at(hp, "layoutConditions").structs.push_back({{"category", gi(1)}, {"parameter", gs("$4")}, {"layoutID", gi(-1)}});
        at(hp, "shellSensor").structs.push_back(cond(9, "$5"));
    }
    if (patch) npcs.values.push_back(copy);
    {
        GStruct &col = at(npcs.values[hpi], "collision").structs.at(0);
        at(col, "parameters") = gfloats({3.0f, 1.3f, 3.0f});
        at(at(col, "offset").structs.at(0), "y") = gf((float)(0.25 - RAISE));
    }
    b.write_sheet("Npc.gsheet", npcs);
    GSheet items = b.read_sheet("Items.gsheet");
    for (GStruct &it : items.values)
        if (at(it, "symbol").s == "HeartPiece") at(it, "npcKey") = gs("PatchHeartPiece");
    b.write_sheet("Items.gsheet", items);
}

// ======== seashell_spots.apply

const int SHELL_TAG = 200;  // seashell_spots.TAG

GStruct lot(const char *type, int32_t cookie, int32_t weight) {
    return {{"mType", gs(type)}, {"mCookie", gi(cookie)}, {"mWeight", gi(weight)}};
}
int drop_row(const GSheet &drops, const std::string &key) {
    for (size_t r = 0; r < drops.values.size(); r++)
        if (at(drops.values[r], "mKey").s == key) return (int)r;
    return -1;
}
// a copy of row `base` as `key` dropping one `type` with cookie `cookie` (unless there is a `key` row already)
void add_drop(GSheet &drops, const std::string &base, const std::string &key, const char *type, int32_t cookie) {
    int bi = drop_row(drops, base);
    if (bi < 0) throw std::runtime_error("ItemDrop: no " + base);
    if (drop_row(drops, key) >= 0) return;
    GStruct e = drops.values[bi];
    at(e, "mKey") = gs(key);
    at(e, "mLotTable") = gsa({lot(type, cookie, 1)});
    drops.values.push_back(e);
}

// decide mode: the Secret Seashell spots no -sanity check took (seashell_spots.locations), by seashell index, and
// where each is (seashell_spots.scan); vanilla: Shellsanity off
void decide_shell_spots(SeedBuild &b, Decider &dc, const std::map<int64_t, ShellSpot> &spots) {
    Value s = parse_value(sanity_json(b));
    std::vector<std::pair<int64_t, std::string>> locs;
    for (const auto &e : s.at("shells").dict()) locs.push_back({e.value.i(), e.key});
    std::string missing;
    for (const auto &l : locs)
        if (!spots.count(l.first)) missing += (missing.empty() ? "" : ", ") + l.second;
    if (!missing.empty()) throw std::runtime_error("Secret Seashell spots not found in the rooms: " + missing);
    std::stable_sort(locs.begin(), locs.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
    Value list = Value::new_list();
    for (const auto &l : locs) {
        const ShellSpot &p = spots.at(l.first);
        list.list().push_back(Value(List{Value(l.second), Value((long long)l.first), Value(p.kind), Value(p.level),
                                         Value(p.room), Value(hex_lower(p.key)), Value(p.param)}));
    }
    Value out = Value::new_dict();
    out.set("vanilla", !setting_or(b.decisions(), "shellsanity", false));
    out.set("spots", list);
    dc.set_ours_world("shell-spots", out.json());
}

void seashell_spots(SeedBuild &b) {
    const std::map<int64_t, ShellSpot> spots = shell_scan(b);
    if (Decider *dc = b.decider()) decide_shell_spots(b, *dc, spots);
    yyjson_val *rec = ow(b, "shell-spots");
    if (!rec) return;
    const Decisions &d = b.decisions();
    bool vanilla = yyjson_get_bool(yyjson_obj_get(rec, "vanilla"));
    // shell_spots.tsv; check_flags.tsv: each spot's found flag (tracker.py)
    std::string rows = "# location\tindex\tkind\tlevel\troom\tx\ty\tz\tmodel\tname\tentry\tflag\tflag index\tshell\tscale"
                       "\tturn  (tools/rando/seashell_spots.py)\n",
                checks;
    auto shell_row = [&](const std::string &name, int index, const std::string &level, const std::string &room,
                         const std::string &flag, int32_t flag_index) {
        rows += name + "\t" + std::to_string(index) + "\tshell\t" + level + "\t" + room + "\t-\t-\t-\t-\t-\t-\t" + flag +
                "\t" + std::to_string(flag_index) + "\t1\t1\t0\n";
    };
    GSheet sheet = b.read_sheet("GlobalFlags.gsheet");
    std::map<std::string, int32_t> names;
    for (const GStruct &v : sheet.values) names[at(v, "symbol").s] = flag_index(v);
    GSheet drops = b.read_sheet("ItemDrop.gsheet");
    EventFlow fl = b.read_flow("SinkingSword.bfevfl");
    Flow f(b, fl.flowchart);
    std::vector<std::pair<std::string, LebRoom>> rooms;  // (path: in the order first read, as the dict)
    auto room_of = [&](const std::string &path) -> LebRoom & {
        for (auto &r : rooms)
            if (r.first == path) return r.second;
        rooms.emplace_back(path, read_room_at(b, path));
        return rooms.back().second;
    };
    size_t i, n;
    yyjson_val *s;
    yyjson_arr_foreach(yyjson_obj_get(rec, "spots"), i, n, s) {  // (by seashell index)
        const std::string name = jstr(yyjson_arr_get(s, 0));
        int index = (int)yyjson_get_sint(yyjson_arr_get(s, 1));
        if (!vanilla && !d.item_at(name)) continue;
        const std::string flag = "LARShellSpot" + std::to_string(index);
        if (!names.count(flag)) {
            names[flag] = next_flag(b, sheet);
            sheet.values.push_back(flag_row(flag, names[flag]));
        }
        checks += name + "\t" + std::to_string(names[flag]) + "\n";
        if (yyjson_arr_size(s) < 7) throw std::runtime_error("seashell spots: " + name + " not found in the rooms");
        if (vanilla) {  // (its own shell, its own index: nothing to edit)
            shell_row(name, index, jstr(yyjson_arr_get(s, 3)), jstr(yyjson_arr_get(s, 4)), flag, names[flag]);
            continue;
        }
        const std::string kind = jstr(yyjson_arr_get(s, 2));
        LebRoom &r = room_of(room_path(jstr(yyjson_arr_get(s, 3)), jstr(yyjson_arr_get(s, 4))));
        uint64_t key = jkey(yyjson_arr_get(s, 5));
        size_t k = (size_t)yyjson_get_sint(yyjson_arr_get(s, 6));
        SeedBuild::ItemInfo it = b.item_info(name, true);
        size_t hi = r.actors.size();
        for (size_t a = 0; a < r.actors.size() && hi == r.actors.size(); a++)
            if (r.actors[a].key == key) hi = a;
        if (hi == r.actors.size()) throw std::runtime_error("seashell spots: no host for " + name);
        LebParam &param = r.actors[hi].params[k];
        if (placed(d, name) == "seashell") {
            // the game's own shell, with the index LAS gave it (a host's own ItemDrop entry: Seashell<n> may be retagged)
            if (param.kind == LebParam::Int) {
                param = LebParam::of_int((uint32_t)it.index);
            } else {
                param = str_param("LARShell" + std::to_string(it.index));
                add_drop(drops, "Seashell0", "LARShell" + std::to_string(it.index), "Seashell", it.index);
            }
            shell_row(name, it.index, jstr(yyjson_arr_get(s, 3)), jstr(yyjson_arr_get(s, 4)), flag, names[flag]);
            continue;
        }
        if (kind == "dig") {  // (no buried shell: the runtime brings the item up when the spot is dug)
            remove_actors(r, {hi});
        } else if (kind == "hole") {
            param = LebParam::of_int((uint32_t)index);
        } else {
            param = str_param("LARShellSpot" + std::to_string(index));
            add_drop(drops, "Seashell" + std::to_string(index), "LARShellSpot" + std::to_string(index), "HeartPiece",
                     SHELL_TAG + index);
        }
        std::string get = give(f, it, starts(it.key, "Rup"));
        f.add_entry_point(name);
        f.action_chain(name, {{"EventFlags", "SetFlag", {{"symbol", flag}, {"value", true}}}, {"SinkingSword", "Destroy", {}}}, get);
        auto where = spots.find(index);  // (where the item shows: the scan's)
        if (where == spots.end()) throw std::runtime_error("seashell spots: " + name + " not found in the rooms");
        double size = 1.0, turn = 0;
        lookup(MODEL_SIZES, it.model_name, size), lookup(MODEL_ROTATIONS, it.model_name, turn);
        rows += name + "\t" + std::to_string(index) + "\t" + kind + "\t" + jstr(yyjson_arr_get(s, 3)) + "\t" +
                jstr(yyjson_arr_get(s, 4)) + "\t" + py_g(where->second.x) + "\t" + py_g(where->second.y) + "\t" +
                py_g(where->second.z) + "\t" + it.model_path + "\t" + it.model_name + "\t" + name + "\t" + flag + "\t" +
                std::to_string(names[flag]) + "\t" + (it.key == "Seashell" ? "1" : "0") + "\t" + py_g(size) + "\t" +
                py_g(turn) + (kind == "hole" ? "\t" + hex_lower(key) : std::string()) + "\n";
    }
    // the digsanity tiles holding a Secret Seashell (digs.SHELLS, when the seed has dig checks): found once owned
    if (setting_or(d, "digs", false)) {
        auto sh = b.carry().find(DIG_SHELLS);
        std::string lines = sh == b.carry().end() ? std::string() : sh->second;
        for (size_t pos = 0; pos < lines.size();) {
            size_t end = lines.find('\n', pos);
            std::string line = lines.substr(pos, end - pos);
            size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
            rows += line.substr(0, t2) + "\tshell\t-\t-\t-\t-\t-\t-\t-\t-\t-\t" + line.substr(t2 + 1) + "\t1\t1\t0\n";
            pos = end + 1;
        }
    }
    for (auto &r : rooms) write_room_at(b, r.first, r.second);
    b.write_sheet("GlobalFlags.gsheet", sheet);
    b.write_sheet("ItemDrop.gsheet", drops);
    b.write_flow("SinkingSword.bfevfl", fl);
    b.table_write("shell_spots.tsv", rows);
    b.table_append("check_flags.tsv", checks);
    b.table_append("flags_ext", "seashell spots: found flags set by the runtime (runtime/hle/shell_spots.c)\n");
}

// ======== apple_trees.apply

// apple_trees.apply's slots: every placed apple-tree-<screen>-<slot>, sorted
std::vector<std::string> apple_locs(const Decisions &d) {
    std::vector<std::string> locs;
    yyjson_obj_iter pit = yyjson_obj_iter_with(d.raw("placements"));
    for (yyjson_val *k; (k = yyjson_obj_iter_next(&pit));)
        if (starts(jstr(k), "apple-tree-")) locs.push_back(jstr(k));
    std::sort(locs.begin(), locs.end());
    return locs;
}

// decide mode: the look a trap hanging in a tree takes - a random real item of the seed
// (random.Random(f'{seed}-apple-looks'), one draw per trap slot in name order)
void decide_apple_looks(SeedBuild &b, Decider &dc) {
    const Decisions &d = b.decisions();
    std::set<std::string> placed_items;
    yyjson_obj_iter pit = yyjson_obj_iter_with(d.raw("placements"));
    for (yyjson_val *k; (k = yyjson_obj_iter_next(&pit));)
        if (yyjson_is_str(yyjson_obj_iter_get_val(k))) placed_items.insert(jstr(yyjson_obj_iter_get_val(k)));
    std::set<std::pair<std::string, std::string>> look_set;
    for (const std::string &it : placed_items) {
        const Decisions::ItemDef *def = d.item_def(it);
        if (def && !def->model_path.empty() && !ends(def->key, "Trap")) look_set.insert({def->model_path, def->model_name});
    }
    std::vector<std::pair<std::string, std::string>> looks(look_set.begin(), look_set.end());
    PyRandom rng = dc.stream("apple-looks");
    Value picks = Value::new_dict();
    for (const std::string &name : apple_locs(d))
        if (ends(item_key_at(d, name), "Trap") && !looks.empty()) {
            const auto &pick = rng.choice(looks);
            picks.set(name, Value(List{Value(pick.first), Value(pick.second)}));
        }
    dc.set_ours_world("apple-looks", picks.json());
}

void apple_trees(SeedBuild &b) {
    if (Decider *dc = b.decider()) decide_apple_looks(b, *dc);
    yyjson_val *looks = ow(b, "apple-looks");
    if (!looks) return;
    const Decisions &d = b.decisions();
    GSheet sheet = b.read_sheet("GlobalFlags.gsheet");
    GSheet drops = b.read_sheet("ItemDrop.gsheet");
    for (GStruct &v : drops.values) {  // (every roll a red apple: a tree always hangs 3)
        const std::string key = at(v, "mKey").s;
        if (key == "AppleLow" || key == "Applehigh") at(v, "mLotTable") = gsa({lot("Apple", 1, 100)});
    }
    EventFlow fl = b.read_flow("SinkingSword.bfevfl");
    Flow f(b, fl.flowchart);
    std::map<std::string, AppleTree> trees;  // (by room: where each slot's item lands, the tree's key)
    for (AppleTree &t : apple_scan(b)) trees[t.room] = t;
    std::string rows = "# location\tindex\tkind\tlevel\troom\tx\ty\tz\tmodel\tname\tentry\tflag\tflag index\tshell\tscale"
                       "\tturn\ttree\tslot  (tools/rando/apple_trees.py)\n",
                checks;
    const std::vector<std::string> locs = apple_locs(d);
    std::map<std::tuple<std::string, int, bool>, std::string> tails;
    for (size_t n = 0; n < locs.size(); n++) {  // apple-tree-<screen>-<slot>
        const std::string &name = locs[n];
        const std::string flag = "LARApple" + name.substr(11, 3) + name.substr(15);
        int32_t index = next_flag(b, sheet);
        sheet.values.push_back(flag_row(flag, index));
        checks += name + "\t" + std::to_string(index) + "\n";
        SeedBuild::ItemInfo it = b.item_info(name, true);
        // (a trap looks like a random real item of the seed: the recorded draw)
        yyjson_val *look = yyjson_obj_get(looks, name.c_str());
        if (ends(it.key, "Trap") && look) it.model_path = jstr(yyjson_arr_get(look, 0)), it.model_name = jstr(yyjson_arr_get(look, 1));
        auto tree = trees.find("Field_" + name.substr(11, 3));
        size_t slot = (size_t)atoi(name.substr(15).c_str()) - 1;
        if (tree == trees.end() || slot >= tree->second.landings.size())
            throw std::runtime_error("apple trees: no tree for " + name);
        const auto &xyz = tree->second.landings[slot];
        double size = 1.0, turn = 0;
        lookup(MODEL_SIZES, it.model_name, size), lookup(MODEL_ROTATIONS, it.model_name, turn);
        rows += name + "\t" + std::to_string(100 + n) + "\tapple\tField\t" + tree->first + "\t" + py_g(xyz[0]) + "\t" +
                py_g(xyz[1]) + "\t" + py_g(xyz[2]) + "\t" + it.model_path + "\t" + it.model_name + "\t" + name + "\t" +
                flag + "\t" + std::to_string(index) + "\t" + (it.key == "Seashell" ? "1" : "0") + "\t" + py_g(size) +
                "\t" + py_g(turn) + "\t" + hex_lower(tree->second.key) + "\t" + std::to_string(slot) + "\n";
        bool quick = starts(it.key, "Rup") || quick_item(placed(d, name));
        auto tk = std::make_tuple(it.key, it.index, quick);
        if (!tails.count(tk)) {
            std::string get = give(f, it, quick);
            tails[tk] = f.action("SinkingSword", "Destroy", {}, get);
        }
        f.add_entry_point(name);
        std::string set = f.action("EventFlags", "SetFlag", {{"symbol", flag}, {"value", true}}, tails[tk]);
        f.insert_after(name, set);
    }
    b.write_sheet("GlobalFlags.gsheet", sheet);
    b.write_sheet("ItemDrop.gsheet", drops);
    b.write_flow("SinkingSword.bfevfl", fl);
    b.table_write("apple_trees.tsv", rows);  // (INDEX_BASE 100 + n: never a seashell index)
    b.table_append("check_flags.tsv", checks);
}

// ======== entrance_logic.apply_rooms (its write_room: las_headless.write_room)

// decide mode: the entrance shuffle's result `ent` ([doors, {door: interior it leads into}, {interior: door it exits
// to}], None without a tier-2 shuffle). Until the native shuffle feeds the build, it comes from the recorded run's
// 'post_entrances' checkpoint (Decider::input). nullptr: no shuffle.
yyjson_doc *post_entrances(SeedBuild &b, Decider &dc, yyjson_val *&ent) {
    ent = nullptr;
    const std::string *pe = dc.input("post_entrances.json");
    if (!pe) {
        if (setting_int(b.decisions(), "entrance-tier", 0) >= 2) throw std::runtime_error("decide: no post_entrances.json");
        return nullptr;
    }
    yyjson_doc *doc = yyjson_read(pe->data(), pe->size(), 0);
    if (!doc) throw std::runtime_error("decide: bad post_entrances.json");
    ent = yyjson_obj_get(yyjson_doc_get_root(doc), "ent");
    if (!ent || !yyjson_is_arr(ent) || !yyjson_arr_size(ent)) ent = nullptr;  // (`if ent:`)
    return doc;
}

// entrance_logic.apply_rooms' base(): re.match(r'(.+_\d\d[A-Z])', r).group(1), else r (greedy: the last such cell)
std::string zone_base(const std::string &r) {
    for (size_t p = r.size() >= 4 ? r.size() - 4 : 0; p >= 1 && p + 3 < r.size(); p--) {
        if (r[p] == '_' && isdigit((unsigned char)r[p + 1]) && isdigit((unsigned char)r[p + 2]) && r[p + 3] >= 'A' && r[p + 3] <= 'Z')
            return r.substr(0, p + 4);
        if (p == 1) break;
    }
    return r;
}

LebParam zone_param(yyjson_val *p) {  // (a loading_zones.json parameter as apply_rooms sets it: str -> bytes)
    if (yyjson_is_str(p)) return str_param(jstr(p));
    if (yyjson_is_real(p)) return LebParam::of_float(yyjson_get_real(p));
    return LebParam::of_int((uint32_t)yyjson_get_sint(p));
}
Value param_value(const LebParam &p) {  // (as the record writes it: bytes decoded)
    if (p.kind == LebParam::Str) return Value(std::string(p.s.begin(), p.s.end()));
    if (p.kind == LebParam::Float) return Value(p.f);
    return Value((long long)p.i);
}

// decide mode: entrance_logic.apply_rooms - door d's field zone gets interior entry[d]'s entry parameters, every zone
// of interior t leading back outside gets the return parameters of interior exit[t]; each room it writes, in order,
// with the zones that changed against the room as it was
void decide_entrance_rooms(SeedBuild &b, Decider &dc) {
    yyjson_val *ent;
    yyjson_doc *doc = post_entrances(b, dc, ent);
    if (!ent) {
        yyjson_doc_free(doc);
        return;
    }
    const std::string zj = loading_zones_json();
    yyjson_doc *zdoc = yyjson_read(zj.data(), zj.size(), 0);
    yyjson_val *zones = yyjson_doc_get_root(zdoc);
    auto zone_params = [&](const std::string &room, int64_t index) -> yyjson_val * {
        size_t i, n;
        yyjson_val *z;
        yyjson_arr_foreach(zones, i, n, z)
            if (jstr(yyjson_arr_get(z, 1)) == room && yyjson_get_sint(yyjson_arr_get(z, 2)) == index) return yyjson_arr_get(z, 3);
        throw std::runtime_error("entrances: no loading zone " + room + " " + std::to_string(index));
    };
    yyjson_val *doors = yyjson_arr_get(ent, 0), *entry = yyjson_arr_get(ent, 1), *exit_ = yyjson_arr_get(ent, 2);
    std::map<std::string, yyjson_val *> by_id, field, ret_params;
    std::map<std::string, std::vector<std::pair<std::string, int64_t>>> returns;
    size_t i, n;
    yyjson_val *d;
    yyjson_arr_foreach(doors, i, n, d) {
        const std::string id = jstr(yyjson_obj_get(d, "id"));
        by_id[id] = d;
        field[id] = zone_params(jstr(yyjson_obj_get(d, "field_room")), yyjson_get_sint(yyjson_obj_get(d, "zone_index")));
        yyjson_val *rz = yyjson_obj_get(d, "return_zone");
        auto &ret = returns[id];
        if (jstr(yyjson_obj_get(d, "type")) == "connector") {  // (several doors of one cave: the zone named per door)
            std::vector<yyjson_val *> rzs = {rz};
            yyjson_val *other = yyjson_obj_get(d, "other_return_zones");
            if (!py_truthy(other)) other = yyjson_is_obj(rz) ? yyjson_obj_get(rz, "other_return_zones") : nullptr;
            size_t k, kn;
            yyjson_val *o;
            if (py_truthy(other)) yyjson_arr_foreach(other, k, kn, o) rzs.push_back(o);
            for (yyjson_val *r : rzs) ret.push_back({jstr(yyjson_obj_get(r, "room")), yyjson_get_sint(yyjson_obj_get(r, "index"))});
            ret_params[id] = zone_params(ret[0].first, ret[0].second);
            continue;
        }
        const std::string to = zone_base(py_truthy(rz) ? jstr(yyjson_obj_get(rz, "target")) : jstr(yyjson_obj_get(d, "field_room")));
        const std::string level = jstr(yyjson_obj_get(d, "dest_level"));
        size_t k, kn;
        yyjson_val *z;
        yyjson_arr_foreach(zones, k, kn, z) {
            yyjson_val *ps = yyjson_arr_get(z, 3);
            if (jstr(yyjson_arr_get(z, 0)) != level || jstr(yyjson_arr_get(ps, 0)) != "Field" ||
                zone_base(jstr(yyjson_arr_get(ps, 1))) != to)
                continue;
            if (ret.empty()) ret_params[id] = ps;
            ret.push_back({jstr(yyjson_arr_get(z, 1)), yyjson_get_sint(yyjson_arr_get(z, 2))});
        }
    }
    std::vector<std::pair<std::string, std::vector<std::pair<int64_t, yyjson_val *>>>> edits;  // (dicts in order)
    auto edit = [&](const std::string &room, int64_t idx, yyjson_val *params) {
        auto e = std::find_if(edits.begin(), edits.end(), [&](const auto &x) { return x.first == room; });
        if (e == edits.end()) edits.push_back({room, {}}), e = edits.end() - 1;
        auto z = std::find_if(e->second.begin(), e->second.end(), [&](const auto &x) { return x.first == idx; });
        if (z == e->second.end()) e->second.push_back({idx, params});
        else z->second = params;
    };
    yyjson_obj_iter it = yyjson_obj_iter_with(entry);
    for (yyjson_val *k; (k = yyjson_obj_iter_next(&it));) {
        yyjson_val *door = by_id.at(jstr(k));
        edit(jstr(yyjson_obj_get(door, "field_room")), yyjson_get_sint(yyjson_obj_get(door, "zone_index")),
             field.at(jstr(yyjson_obj_iter_get_val(k))));
    }
    it = yyjson_obj_iter_with(exit_);
    for (yyjson_val *k; (k = yyjson_obj_iter_next(&it));) {
        const std::string c = jstr(yyjson_obj_iter_get_val(k));
        if (!ret_params.count(c)) continue;
        for (const auto &r : returns[jstr(k)]) edit(r.first, r.second, ret_params[c]);
    }
    Value out = Value::new_list();
    for (const auto &e : edits) {
        const std::string path = room_path(room_level(e.first), e.first);
        LebRoom before = read_room_at(b, path), after = read_room_at(b, path);
        std::vector<LebActor *> zb, za;
        for (LebActor &a : before.actors)
            if (a.type == 0x190) zb.push_back(&a);
        for (LebActor &a : after.actors)
            if (a.type == 0x190) za.push_back(&a);
        for (const auto &ze : e.second) {
            if (ze.first < 0 || (size_t)ze.first >= za.size()) continue;
            size_t pi, pn;
            yyjson_val *p;
            yyjson_arr_foreach(ze.second, pi, pn, p)
                if (pi < 8) za[(size_t)ze.first]->params[pi] = zone_param(p);
        }
        Value changed = Value::new_list();
        for (size_t z = 0; z < za.size(); z++) {
            bool same = true;
            for (int p = 0; p < 8; p++) same = same && za[z]->params[p] == zb[z]->params[p];
            if (same) continue;
            Value ps = Value::new_list();
            for (int p = 0; p < 8; p++) ps.list().push_back(param_value(za[z]->params[p]));
            changed.list().push_back(Value(List{Value((long long)z), ps}));
        }
        out.list().push_back(Value(List{Value(e.first), changed}));
    }
    yyjson_doc_free(zdoc);
    yyjson_doc_free(doc);
    dc.set_ours_world("entrance-rooms", out.json());
}

void entrance_rooms(SeedBuild &b) {
    if (Decider *dc = b.decider()) decide_entrance_rooms(b, *dc);
    size_t i, n, j, m;
    yyjson_val *e, *z;
    yyjson_arr_foreach(ow(b, "entrance-rooms"), i, n, e) {
        const std::string room = jstr(yyjson_arr_get(e, 0));
        const std::string path = room_path(room_level(room), room);
        LebRoom r = read_room_at(b, path);
        std::vector<LebActor *> zones;
        for (LebActor &a : r.actors)
            if (a.type == 0x190) zones.push_back(&a);
        yyjson_arr_foreach(yyjson_arr_get(e, 1), j, m, z) {
            size_t idx = (size_t)yyjson_get_sint(yyjson_arr_get(z, 0));
            if (idx >= zones.size()) continue;
            size_t pi, pn;
            yyjson_val *p;
            yyjson_arr_foreach(yyjson_arr_get(z, 1), pi, pn, p) {
                if (pi >= 8) break;
                LebParam &dst = zones[idx]->params[pi];
                if (yyjson_is_str(p)) dst = str_param(jstr(p));
                else if (yyjson_is_real(p)) dst = LebParam::of_float(yyjson_get_real(p));
                else dst = LebParam::of_int((uint32_t)yyjson_get_sint(p));
            }
        }
        write_room_at(b, path, r);
    }
}

// entrance_logic.raft_shop_to_dock: the paid ride starts at the vanilla dock whatever the shop's exit
void raft_shop(SeedBuild &b) {
    if (Decider *dc = b.decider()) {  // (decide mode: the shop's exit shuffled)
        yyjson_val *ent;
        yyjson_doc *doc = post_entrances(b, *dc, ent);
        if (ent) {
            const char *to = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(ent, 2), "raft-shop"));
            dc->set_ours_world("raft-shop-moved", to && strcmp(to, "raft-shop") ? "true" : "false");
        }
        yyjson_doc_free(doc);
    }
    if (!yyjson_get_bool(ow(b, "raft-shop-moved"))) return;
    const std::string path = "region_common/event/RaftShopMan.bfevfl";
    if (!b.files().count(path)) {  // (a copy of the game's first)
        std::string bytes;
        if (!b.read_game(path, bytes)) throw std::runtime_error("no " + path);
        b.write_bytes(path, bytes);
    }
    std::string bytes;
    b.read_bytes(path, bytes);
    EventFlow fl = EventFlow::parse(bytes);
    Flow f(b, fl.flowchart);
    if (f.event("Event9").next != EVFL_NONE) throw std::runtime_error("RaftShopMan.bfevfl Event9 changed");
    f.action_chain("Event9", {{"GameControl", "RequestLevelJump",
                               {{"level", "Field"}, {"locator", "Field_04P"}, {"offsetX", 0.0}, {"offsetZ", 0.0}}}});
    b.write_bytes(path, fl.write());
}

// ======== miniboss_guard.apply

struct Guard { uint16_t type; const char *flow, *actor, *before, *after, *room; };
const Guard kGuards[] = {
    {0x57, "Lanmola.bfevfl", "Lanmola", "Event34", "Event9", "Field_13O"},
    {0x5B, "DeguArmos.bfevfl", "DeguArmos", "Event47", nullptr, nullptr},
};
struct Entry { uint16_t type; const char *flow, *actor, *entry; };
const Entry kQuiet[] = {{0x61, "BossBlob.bfevfl", "BossBlob", "BossBlobNoDamage"}};
const Entry kDeath[] = {
    {0x53, "Hinox.bfevfl", "Hinox", "HinoxEnd"},
    {0x51, "Rola.bfevfl", "Rola", "RolaEnd"},
    {0x56, "Dodongo.bfevfl", "Dodongo", "DodongoEnd"},
    {0x61, "BossBlob.bfevfl", "BossBlob", "BossBlobEnd"},
    {0x5B, "DeguArmos.bfevfl", "DeguArmos", "DeguArmosEnd"},
    {0x57, "Lanmola.bfevfl", "Lanmola", "LanmolaEnd"},
    {0x52, "Bossblin.bfevfl", "Bossblin", "BossblinEnd"},
    {0x5F, "MightPunch.bfevfl", "MightPunch", "MightPunchEnd"},
    {0x5C, "Jacky.bfevfl", "Jacky", "JackyEnd"},
    {0x60, "StoneHinox.bfevfl", "StoneHinox", "StoneHinoxEnd"},
    {0x58, "HiploopHover.bfevfl", "HiploopHover", "HiploopHoverEnd"},
    {0x21E, "ShadowLink.bfevfl", "ShadowLink", "ShadowLinkEnd"},
    {0x59, "MasterStalfon.bfevfl", "MasterStalfon", "MasterStalfon4End"},
};
size_t mark_switch(uint16_t type) { return type == 0x59 ? 3 : 2; }  // (MARK_SWITCH)

// miniboss_guard._reachable: the events from one, in the order a stack walk visits them
std::vector<size_t> reachable(const EvflFlowchart &fc, uint16_t start) {
    std::vector<size_t> seen;
    std::vector<bool> was(fc.events.size(), false);
    std::vector<uint16_t> stack = {start};
    while (!stack.empty()) {
        uint16_t ev = stack.back();
        stack.pop_back();
        if (ev == EVFL_NONE || ev >= fc.events.size() || was[ev]) continue;
        was[ev] = true;
        seen.push_back(ev);
        const EvflEvent &e = fc.events[ev];
        if (e.type == EvflEvent::Action || e.type == EvflEvent::Join || e.type == EvflEvent::SubFlow) stack.push_back(e.next);
        else if (e.type == EvflEvent::Switch)
            for (auto &c : e.cases) stack.push_back(c.second);
        else if (e.type == EvflEvent::Fork) {
            for (uint16_t x : e.forks) stack.push_back(x);
            stack.push_back(e.join);
        }
    }
    return seen;
}

Params params_of(const EvflContainer &c) {
    Params p;
    for (auto &kv : c.items) p.emplace_back(kv.first.c_str(), V(kv.second));
    return p;
}

// miniboss_guard.death_path: a copy's short end (the death effect, item drop, models hidden); "" for none
std::string death_path(Flow &f, const std::string &actor, const std::string &entry) {
    const EvflFlowchart &fc = f.fc;
    EvflEntryPoint *ep = f.find_entry(entry);
    if (!ep) throw std::runtime_error("miniboss guard: no entry " + entry);
    bool has_effect = false, has_drop = false;
    EvflContainer effect, drop;
    std::map<int32_t, EvflContainer> hide;
    for (size_t ev : reachable(fc, ep->main_event)) {
        const EvflEvent &e = fc.events[ev];
        if (e.type != EvflEvent::Action || fc.actors[e.actor].id.name != actor) continue;
        const std::string &act = fc.actors[e.actor].actions.at(e.action);
        if (act == "ModelVisibility") {
            const EvflValue *vis = e.params.get("visible");
            bool visible = !vis || (vis->type == EvflValue::Bool ? vis->b : vis->i != 0);
            if (!visible) {
                const EvflValue *mi = e.params.get("modelIndex");
                int32_t idx = mi ? mi->i : 0;
                if (!hide.count(idx)) hide[idx] = e.params;
            }
        } else if (act == "BossItemDrop" && !has_drop) {
            drop = e.params, has_drop = true;
        } else if (act == "PlayTailorOtherChannelEx" && !has_effect) {
            effect = e.params, has_effect = true;
        }
    }
    std::string nxt;
    for (auto it = hide.rbegin(); it != hide.rend(); ++it) {  // (built back to front: the highest model index first)
        std::string e = f.action(actor, "ModelVisibility", params_of(it->second), nxt);
        nxt = e;
    }
    if (has_drop) { std::string e = f.action(actor, "BossItemDrop", params_of(drop), nxt); nxt = e; }
    if (has_effect) { std::string e = f.action(actor, "PlayTailorOtherChannelEx", params_of(effect), nxt); nxt = e; }
    return nxt;
}

std::string entry_first(Flow &f, const std::string &entry) {
    EvflEntryPoint *ep = f.find_entry(entry);
    if (!ep || ep->main_event == EVFL_NONE) throw std::runtime_error("miniboss guard: no entry " + entry);
    return f.fc.events.at(ep->main_event).name;
}

void miniboss_guard(SeedBuild &b) {
    const Decisions &d = b.decisions();
    std::set<uint64_t> originals;  // (the original's own key: he keeps his item)
    auto vanilla_keys = [&](const std::string &level, const std::string &room, uint16_t type) {
        std::string bytes;
        if (!b.read_game(room_path(level, room), bytes)) throw std::runtime_error("no " + room);
        for (const LebActor &a : LebRoom::parse(bytes).actors)
            if (a.type == type) originals.insert(a.key);
    };
    for (const Guard &g : kGuards)
        if (g.room) vanilla_keys("Field", g.room, g.type);
    vanilla_keys("Lv08TurtleRock", "Lv08TurtleRock_03H", 0x5F);  // (ORIGINAL_ROOMS)
    const std::set<uint16_t> anywhere = {0x21E, 0x5F};
    std::set<uint16_t> marked;
    for (const Guard &g : kGuards) marked.insert(g.type);
    for (const Entry &q : kQuiet) marked.insert(q.type);
    for (const Entry &q : kDeath) marked.insert(q.type);
    std::set<uint16_t> kinds;
    if (d.setting_str("enemy-mode") == "random" && setting_or(d, "randomize-enemies", false)) kinds = anywhere;
    bool rolled = !kinds.empty();
    struct Room { std::string path; LebRoom room; const std::set<uint16_t> *types; };
    std::vector<Room> rooms;
    for (const std::string &lv : b.list_written("region_common/level")) {
        const std::string dir = "region_common/level/" + lv;
        if (starts(lv, "Panel") || b.files().count(dir)) continue;  // (Dampe's chamber: his own; a file isn't a level)
        const std::set<uint16_t> *types = lv == "Field" ? &marked : &anywhere;
        for (const std::string &fn : b.list_written(dir)) {
            if (!ends(fn, ".leb") || !b.files().count(dir + "/" + fn)) continue;
            LebRoom r = read_room_at(b, dir + "/" + fn);
            bool any = false;
            for (const LebActor &a : r.actors)
                if (types->count(a.type) && !originals.count(a.key)) any = true;
            if (any) rooms.push_back({dir + "/" + fn, std::move(r), types});
        }
    }
    if (rooms.empty() && !rolled) return;
    GSheet sheet = b.read_sheet("GlobalFlags.gsheet");
    int32_t idx = -1;
    for (const GStruct &v : sheet.values)
        if (at(v, "symbol").s == "OverworldMinibossMarker") idx = flag_index(v);
    if (idx < 0) {
        idx = next_flag(b, sheet);
        sheet.values.push_back(flag_row("OverworldMinibossMarker", idx));
        b.write_sheet("GlobalFlags.gsheet", sheet);
    }
    for (Room &r : rooms) {
        for (LebActor &a : r.room.actors)
            if (r.types->count(a.type) && !originals.count(a.key)) {
                a.switches[mark_switch(a.type)] = {1, (uint16_t)idx};
                kinds.insert(a.type);
            }
        write_room_at(b, r.path, r.room);
    }
    auto edit = [&](const char *flow, const std::function<void(Flow &)> &fn) {
        EventFlow fl = b.read_flow(flow);
        Flow f(b, fl.flowchart);
        fn(f);
        b.write_flow(flow, fl);
    };
    // (each table in type order: sorted(kinds & set(...)))
    auto sorted_of = [&](const Entry *t, size_t n) {
        std::vector<const Entry *> v;
        for (size_t i = 0; i < n; i++)
            if (kinds.count(t[i].type)) v.push_back(&t[i]);
        std::sort(v.begin(), v.end(), [](const Entry *a, const Entry *c) { return a->type < c->type; });
        return v;
    };
    for (const Entry *q : sorted_of(kQuiet, sizeof kQuiet / sizeof *kQuiet))
        edit(q->flow, [&](Flow &f) {
            std::string first = entry_first(f, q->entry);
            std::string sw = f.sw(q->actor, "CheckSawTheFirstEvent", {{"switchIndex", 2}}, {{0, first}});
            f.insert_after(q->entry, sw);
        });
    for (const Entry *q : sorted_of(kDeath, sizeof kDeath / sizeof *kDeath))
        edit(q->flow, [&](Flow &f) {
            std::string first = entry_first(f, q->entry);
            std::string shrt = death_path(f, q->actor, q->entry);
            Cases cases = {{0, first}};
            if (!shrt.empty()) cases.push_back({1, shrt});
            std::string sw = f.sw(q->actor, "CheckSawTheFirstEvent", {{"switchIndex", (int)mark_switch(q->type)}}, cases);
            f.insert_after(q->entry, sw);
        });
    std::vector<const Guard *> guards;
    for (const Guard &g : kGuards)
        if (kinds.count(g.type)) guards.push_back(&g);
    std::sort(guards.begin(), guards.end(), [](const Guard *a, const Guard *c) { return a->type < c->type; });
    for (const Guard *g : guards)
        edit(g->flow, [&](Flow &f) {
            std::string first = f.fc.events.at(f.event(g->before).next).name;  // (LAS's item get, after `before`)
            Cases cases = {{0, first}};
            if (g->after) cases.push_back({1, g->after});
            std::string sw = f.sw(g->actor, "CheckSawTheFirstEvent", {{"switchIndex", 2}}, cases);
            f.insert_after(g->before, sw);
        });
}

// ======== fast_egg.apply: the Egg's first hole drops where the maze's last one does (the boss room)

void fast_egg(SeedBuild &b) {
    const std::string first = room_path("Lv09WindFishsEgg", "Lv09WindFishsEgg_01A");
    LebRoom a = read_room_at(b, first), z = read_room_at(b, room_path("Lv09WindFishsEgg", "Lv09WindFishsEgg_05C"));
    auto hole = [](LebRoom &r) -> LebActor * {
        LebActor *h = nullptr;
        int n = 0;
        for (LebActor &x : r.actors)
            if (std::string(x.name.begin(), x.name.end()).compare(0, 12, "AreaHoleLowe") == 0) h = &x, n++;
        return n == 1 ? h : nullptr;
    };
    LebActor *ha = hole(a), *hz = hole(z);
    if (!ha || !hz) return;
    ha->params[1] = hz->params[1];
    ha->params[3] = hz->params[3];
    write_room_at(b, first, a);
}

// ======== chest_looks.sweep: a chest the seed's rooms hold without a model gets the vanilla one

void chest_sweep(SeedBuild &b) {
    if (!b.decisions().setting("chest-looks")) return;
    std::set<std::string> names;
    for (auto &r : CHEST_ROOMS) names.insert(r.second);
    for (auto &r : PANEL_CHEST_ROOMS) names.insert(r.second);
    for (const std::string &lv : b.list_written("region_common/level"))
        for (const std::string &fn : b.list_written("region_common/level/" + lv))
            if (ends(fn, ".leb")) names.insert(fn.substr(0, fn.size() - 4));
    for (const std::string &name : names) {
        const std::string path = room_path(name.substr(0, name.rfind('_')), name);
        std::string raw;
        if (!b.read_bytes(path, raw) || raw.find("ObjTreasureBox") == std::string::npos) continue;
        LebRoom r = LebRoom::parse(raw);
        int n = 0;
        for (LebActor &a : r.actors) {
            const LebParam &m = a.params[6];
            if (a.type == 0xF7 && !(m.kind == LebParam::Str && ends(std::string(m.s.begin(), m.s.end()), ".bfres"))) {
                a.params[6] = str_param("ObjTreasureBox.bfres"), a.params[7] = str_param("TreasureBox");
                n++;
            }
        }
        if (n) write_room_at(b, path, r);
    }
}

// ======== environments.apply: each room's environment preset another of its kind (top-down / side view)

void environments(SeedBuild &b) {
    static const std::vector<std::string> topdown = {
        "AncientRuins", "AncientRuins2", "AncientRuinsDark", "Banana", "Bear", "BedRoom", "Cemetery", "Christine",
        "Crane", "Cucco", "Default", "Desert", "Dog", "DreamShrine", "DrWright", "DungeonSmall", "DungeonSmallTalTal",
        "DungeonSmallUnderground", "DungeonSmallWater", "DungeonSmallWaterDark", "Event1", "Event2", "EventEnding",
        "EventStairs", "Fairy", "Field", "FieldAncientRuins", "FieldBowWow", "FieldKanalet", "FishingBoat",
        "FishingPond", "Ghost", "Goponga", "GreatFairy", "HolyEgg", "ItemGet", "KanaletCastle", "Library", "Lv01Altar",
        "Lv01Base", "Lv02Altar", "Lv02Base", "Lv02Dark", "Lv02DoorOpen", "Lv02Teresa", "Lv03Altar", "Lv03Base",
        "Lv04Altar", "Lv04Base", "Lv05Altar", "Lv05Base", "Lv06Altar", "Lv06Base", "Lv06Dark", "Lv07Altar", "Lv07Base",
        "Lv08Altar", "Lv08Base", "Lv08Base2", "Lv08Dark", "Lv08Treasurebox", "Lv09Base", "Lv09Base2", "Lv09Base3",
        "Lv09BaseDark", "Lv10Base", "Lv11Base", "Madam", "MadBattersWell", "Magic", "MagicDark", "MagicField",
        "MamuCave", "MamuCaveDark", "MarinTarin", "MoriblinCave", "MysteriousForest", "Photo", "Quaduplet", "Rabbit",
        "RapidFlow", "RapidsRide", "Richard", "Schule", "Seashell", "Shop", "TalTalMountain", "TelBox",
        "ToronboShores", "Tracy", "Ulrira", "Underwater", "Zora"};  // (environments.TOPDOWN_ENVIRONMENTS, in order)
    static const std::vector<std::string> side = {
        "Angler", "KanaletCastleEnter", "Lv01Side", "Lv02Side", "Lv02Side2", "Lv03Side", "Lv04Side", "Lv04Side2",
        "Lv04Side3", "Lv05Side", "Lv05Side2", "Lv06Side", "Lv07Side", "Lv08Side", "Lv08Side2", "Lv08Side3",
        "MamboCave"};  // (environments.SIDESCROLLER_ENVIRONMENTS)
    auto in = [](const std::vector<std::string> &l, const std::string &t) { return std::find(l.begin(), l.end(), t) != l.end(); };
    if (Decider *dc = b.decider()) {  // (decide mode: the same draws over the same files, before any is written)
        PyRandom rng = dc->stream("environments");
        Value picks = Value::new_list();
        for (const std::string &lv : b.list_game("region_common/level")) {
            std::string bytes;
            if (!b.read_bytes("region_common/level/" + lv + "/" + lv + ".lvb", bytes)) continue;
            for (const LvbZone &z : LvbLevel::parse(bytes).zones) {
                const std::string t(z.room_type.begin(), z.room_type.end());
                if (in(topdown, t)) picks.list().push_back(Value(rng.choice(topdown)));
                else if (in(side, t)) picks.list().push_back(Value(rng.choice(side)));
            }
        }
        dc->set_ours_world("environments", picks.json());
    }
    yyjson_val *picks = ow(b, "environments");
    if (!picks) return;
    size_t next = 0;
    for (const std::string &lv : b.list_game("region_common/level")) {
        const std::string path = "region_common/level/" + lv + "/" + lv + ".lvb";
        std::string bytes;
        if (!b.read_bytes(path, bytes)) continue;
        LvbLevel level = LvbLevel::parse(bytes);
        bool edited = false;
        for (LvbZone &z : level.zones) {
            const std::string t(z.room_type.begin(), z.room_type.end());
            if (!in(topdown, t) && !in(side, t)) continue;
            const std::string pick = jstr(yyjson_arr_get(picks, next++));
            if (pick.empty()) throw std::runtime_error("environments: fewer draws recorded than rooms");
            z.room_type = Bytes(pick.begin(), pick.end());
            edited = true;
        }
        if (edited) b.write_bytes(path, level.repack());
    }
}

}  // namespace
}  // namespace las

std::vector<BuildStep> steps_ours_world() {
    using namespace las;
    return {
        {"pots.apply", pots},  // (bushes.apply and rocks.apply write through it)
        {"las_headless.write_room", entrance_rooms},
        {"entrance_logic.raft_shop_to_dock", raft_shop},
        {"digs.apply", digs},
        {"underwater.apply", underwater},
        {"seashell_spots.apply", seashell_spots},
        {"apple_trees.apply", apple_trees},
        {"miniboss_guard.apply", miniboss_guard},
        {"chest_looks.sweep", chest_sweep},
        {"fast_egg.apply", fast_egg},
        {"environments.apply", environments},  // (after the decisions are written; its writes aren't logged: see above)
    };
}

}  // namespace rando
