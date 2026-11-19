// The generator's shared helpers (las_lib.h): ported from LAS-Randomizer's RandomizerCore/Tools/event_tools.py,
// Tools/oead_tools.py and Tools/leb.py (MIT, Owen-Splat and contributors).
#include "las_lib.h"

#include <ctype.h>

#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "las_data.h"

namespace rando {
namespace las {

// ---- las_data.h lookups (the tables themselves are generated: las_data.cpp)
namespace data {
const char *lookup(const StrTable &t, const std::string &key) {
    for (const auto &kv : t)
        if (key == kv.first) return kv.second;
    return nullptr;
}
bool lookup(const NumTable &t, const std::string &key, double &out) {
    for (const auto &kv : t)
        if (key == kv.first) { out = kv.second; return true; }
    return false;
}
int global_flag(const std::string &name) {
    for (const auto &kv : GLOBAL_FLAGS)
        if (name == kv.first) return kv.second;
    throw std::runtime_error("las: no global flag " + name);
}
}  // namespace data

EvflContainer container(const Params &p) {
    EvflContainer c;
    for (const KV &kv : p) c.set(kv.k, kv.v);  // (a repeated key keeps its first place, as a dict)
    return c;
}

// ---- event_tools
EvflEvent *Flow::find(const std::string &name) {
    if (name.empty()) return nullptr;
    int i = fc.find_event(name);
    return i < 0 ? nullptr : &fc.events[i];
}

EvflEvent &Flow::event(const std::string &name) {
    EvflEvent *e = find(name);
    if (!e) throw std::runtime_error("las: no event " + name + " in " + fc.name);
    return *e;
}

EvflEntryPoint *Flow::find_entry(const std::string &name) {
    if (name.empty()) return nullptr;
    int i = fc.find_entry_point(name);
    return i < 0 ? nullptr : &fc.entry_points[i];
}

uint16_t Flow::index_of(const std::string &name) const {
    if (name.empty()) return EVFL_NONE;
    int i = fc.find_event(name);
    return i < 0 ? EVFL_NONE : (uint16_t)i;
}

uint16_t Flow::actor(const std::string &name, const std::string &sub) { return fc.add_actor(name, sub); }

void Flow::add_entry_point(const std::string &name) {
    EvflEntryPoint ep;
    ep.name = name;
    fc.entry_points.push_back(ep);
}

std::string Flow::add(EvflEvent e) {
    e.name = b.next_event_name();
    fc.events.push_back(std::move(e));
    return fc.events.back().name;
}

bool Flow::insert_after(const std::string &prev, const std::string &next) {
    uint16_t n = index_of(next);
    if (EvflEvent *p = find(prev)) {
        if (p->type == EvflEvent::Switch || p->type == EvflEvent::Fork)  // (no next there: the Python fails)
            throw std::runtime_error("las: event " + prev + " has no next event");
        p->next = n;
        return true;
    }
    if (EvflEntryPoint *ep = find_entry(prev)) {
        ep->main_event = n;
        return true;
    }
    return false;
}

bool Flow::set_case(const std::string &sw, uint32_t value, const std::string &next) {
    EvflEvent *e = find(sw);
    if (!e) return false;
    uint16_t n = index_of(next);
    if (n == EVFL_NONE) throw std::runtime_error("las: switch case to no event in " + sw);
    for (auto &c : e->cases)
        if (c.first == value) { c.second = n; return true; }
    throw std::runtime_error("las: switch " + sw + " has no case " + std::to_string(value));
}

void Flow::remove_after(const std::string &name) {
    if (EvflEvent *e = find(name)) e->next = EVFL_NONE;  // (none: the Python prints and goes on)
}

void Flow::set_song(const std::string &ev, const std::string &song) { event(ev).params.set("label", EvflValue::of_str(song)); }

void Flow::add_forks(const std::string &fork, const std::vector<std::string> &branches) {
    EvflEvent &f = event(fork);
    for (const std::string &br : branches) {
        uint16_t i = index_of(br);
        if (i != EVFL_NONE) f.forks.push_back(i);
    }
}

bool Flow::set_fork(const std::string &fork, size_t i, const std::string &next) {
    EvflEvent *f = find(fork);
    if (!f) return false;
    f->forks.at(i) = index_of(next);
    return true;
}

std::string Flow::action(const std::string &actor_name, const std::string &act, const Params &p, const std::string &next) {
    fc.actors[actor(actor_name)].add_action(act);  // (findActor + addActorAction on the name as given)
    uint16_t nxt = index_of(next);
    std::string name = actor_name, sub;
    if (name.find('[') != std::string::npos) {  // "Actor[Sub]"
        std::string s;
        for (char c : name)
            if (c != ']') s += c;
        size_t a = s.find('['), z = s.find('[', a + 1);
        name = s.substr(0, a), sub = s.substr(a + 1, z == std::string::npos ? std::string::npos : z - a - 1);
    }
    int ai = fc.find_actor(name, sub);
    if (ai < 0) throw std::runtime_error("las: no actor " + name + "[" + sub + "] in " + fc.name);
    int ac = fc.actors[ai].find_action(act);
    if (ac < 0) throw std::runtime_error("las: actor " + name + " has no action " + act);
    EvflEvent e;
    e.type = EvflEvent::Action;
    e.actor = (uint16_t)ai, e.action = (uint16_t)ac;
    e.params = container(p);
    std::string n = add(std::move(e));
    if (nxt != EVFL_NONE) fc.events.back().next = nxt;
    return n;
}

std::string Flow::sw(const std::string &actor_name, const std::string &query, Params p, const Cases &cases) {
    uint16_t ai = actor(actor_name);
    fc.actors[ai].add_query(query);
    for (KV &kv : p)
        if (kv.k == "value1") kv.v = EvflValue::of_arg(kv.v.s);
    EvflEvent e;
    e.type = EvflEvent::Switch;
    e.actor = ai, e.action = (uint16_t)fc.actors[ai].find_query(query);
    e.params = container(p);
    std::string n = add(std::move(e));
    size_t me = fc.events.size() - 1;
    std::vector<std::pair<uint32_t, uint16_t>> cs;
    for (const auto &c : cases) {
        uint16_t i = index_of(c.second);
        if (i == EVFL_NONE) continue;
        bool dup = false;  // (a dict: a repeated value replaces the earlier one in place)
        for (auto &o : cs)
            if (o.first == c.first) o.second = i, dup = true;
        if (!dup) cs.emplace_back(c.first, i);
    }
    fc.events[me].cases = cs;
    return n;
}

std::string Flow::subflow(const std::string &chart, const std::string &entry, const Params &p, const std::string &next) {
    uint16_t nxt = index_of(next);
    EvflEvent e;
    e.type = EvflEvent::SubFlow;
    e.params = container(p);
    e.flowchart = chart, e.entry_point = entry;
    std::string n = add(std::move(e));
    if (nxt != EVFL_NONE) fc.events.back().next = nxt;
    return n;
}

std::string Flow::join(const std::string &next) {
    uint16_t nxt = index_of(next);
    EvflEvent e;
    e.type = EvflEvent::Join;
    std::string n = add(std::move(e));
    if (nxt != EVFL_NONE) fc.events.back().next = nxt;
    return n;
}

std::pair<std::string, std::string> Flow::fork(const std::string &before, const std::vector<std::string> &forks,
                                               const std::string &next) {
    std::string j = join(next);
    EvflEvent e;
    e.type = EvflEvent::Fork;
    e.join = index_of(j);
    std::string n = add(std::move(e));
    size_t me = fc.events.size() - 1;
    if (!before.empty()) insert_after(before, n);
    std::vector<uint16_t> fs;
    for (const std::string &br : forks) {
        uint16_t i = index_of(br);
        if (i != EVFL_NONE) fs.push_back(i);
    }
    fc.events[me].forks = fs;
    return {n, j};
}

std::string Flow::action_chain(const std::string &before, const ActionDefs &defs, const std::string &after) {
    if (defs.empty()) return "";
    std::string first = action(defs[0].actor, defs[0].action, defs[0].params, defs.size() == 1 ? after : "");
    std::string current = first;
    insert_after(before, current);
    for (size_t i = 1; i < defs.size(); i++) {
        std::string prev = current;
        current = action(defs[i].actor, defs[i].action, defs[i].params, after);  // (the Python's `after`, not `next`)
        insert_after(prev, current);
    }
    return first;
}

void Flow::insert_action_chain(const std::string &before, const std::vector<std::string> &events) {
    if (events.empty()) return;
    insert_after(before, events[0]);
    for (size_t i = 1; i < events.size(); i++) insert_after(events[i - 1], events[i]);
}

std::string Flow::progressive_switch(const std::string &item1, const std::string &item2, const std::string &flag,
                                     const std::string &before, const std::string &after) {
    std::string get1 = action("Link", "GenericItemGetSequenceByKey", {{"itemKey", item1}, {"keepCarry", false}, {"messageEntry", ""}}, after);
    std::string add1 = action("Inventory", "AddItemByKey", {{"itemKey", item1}, {"count", 1}, {"index", -1}, {"autoEquip", false}}, get1);
    std::string get2 = action("Link", "GenericItemGetSequenceByKey", {{"itemKey", item2}, {"keepCarry", false}, {"messageEntry", ""}}, after);
    std::string add2 = action("Inventory", "AddItemByKey", {{"itemKey", item2}, {"count", 1}, {"index", -1}, {"autoEquip", false}}, get2);
    std::string set = action("EventFlags", "SetFlag", {{"symbol", flag}, {"value", true}}, add1);
    std::string check = sw("EventFlags", "CheckFlag", {{"symbol", flag}}, {{0, set}, {1, add2}});
    insert_after(before, check);
    return check;
}

// ---- data sheets
GValue gsa(const std::vector<GStruct> &items) {
    GValue v;
    v.kind = GValue::StructArr, v.structs = items;
    return v;
}
GValue gstrs(const std::vector<std::string> &items) {
    GValue v;
    v.kind = GValue::StringArr, v.strs = items;
    return v;
}
GValue gfloats(const std::vector<float> &items) {
    GValue v;
    v.kind = GValue::FloatArr, v.floats = items;
    return v;
}
GValue gints(const std::vector<int32_t> &items) {
    GValue v;
    v.kind = GValue::IntArr, v.ints = items;
    return v;
}

GValue &at(GStruct &s, const std::string &name) {
    GValue *v = gfield(s, name);
    if (!v) throw std::runtime_error("las: no sheet field " + name);
    return *v;
}
const GValue &at(const GStruct &s, const std::string &name) {
    const GValue *v = gfield(s, name);
    if (!v) throw std::runtime_error("las: no sheet field " + name);
    return *v;
}
GStruct &item(GValue &arr, size_t i) {
    if (arr.kind != GValue::StructArr || i >= arr.structs.size()) throw std::runtime_error("las: no such sheet array item");
    return arr.structs[i];
}

GStruct cond(int32_t category, const std::string &parameter) {
    return {{"category", gi(category)}, {"parameter", gs(parameter)}};
}

GStruct create_condition(const std::string &name, const std::vector<std::pair<int32_t, std::string>> &checks) {
    std::vector<GStruct> cs;
    for (const auto &c : checks) cs.push_back(cond(c.first, c.second));
    return {{"symbol", gs(name)}, {"conditions", gsa(cs)}};
}

GStruct create_event_trigger(int32_t condition, const std::vector<std::pair<int32_t, std::string>> &additional,
                             const std::string &entry_point) {
    std::vector<GStruct> cs;
    for (const auto &c : additional) cs.push_back(cond(c.first, c.second));
    return {{"condition", gi(condition)}, {"additionalConditions", gsa(cs)}, {"entryPoint", gs(entry_point)}};
}

GStruct create_behavior(int32_t type, const std::vector<std::string> &datas) {
    return {{"type", gi(type)}, {"parameters", gstrs(datas)}};
}

GField create_field(const std::string &name, const std::string &type_name, uint8_t type, uint16_t offset, uint16_t flags) {
    GField f;
    f.name = name, f.type_name = type_name, f.type = type;
    if (flags) f.flags = flags;
    if (type == GField::String) f.inline_size = 0x10;
    if (type == GField::Int) f.inline_size = 0x4;
    if (type == GField::Struct) {
        f.inline_size = 0x10;
        f.fields.push_back(create_field("category", "ConditionCategory", GField::Int, 16, GField::Enum));
        f.fields.push_back(create_field("parameter", "string", GField::String, 16));
    }
    f.data_size = f.inline_size;
    f.offset = offset;
    return f;
}

// ---- rooms
LebParam str_param(const std::string &s) { return LebParam::of_str(s); }

void set_chest_content(LebRoom &r, const std::string &content, const LebParam &index, size_t chest_index, double size,
                       const std::string *model) {
    std::vector<LebActor *> chests;
    for (LebActor &a : r.actors)
        if (a.type == 0xF7) chests.push_back(&a);
    if (chests.size() <= chest_index) return;
    LebActor &c = *chests[chest_index];
    c.params[1] = str_param(content);
    // `item_index if item_index != -1 else b''` (a whole number read from a room is never -1 there: unsigned)
    c.params[2] = index.kind == LebParam::Float && index.f == -1.0 ? str_param("") : index;
    if (model) {
        c.params[6] = str_param(*model);
        c.params[7] = str_param("TreasureBox");
    }
    c.scale[0] = c.scale[1] = c.scale[2] = size;
}

void set_chest_content(LebRoom &r, const std::string &content, int index, size_t chest_index, double size,
                       const std::string *model) {
    set_chest_content(r, content, index == -1 ? str_param("") : LebParam::of_int((uint32_t)index), chest_index, size, model);
}

void apply_model_look(LebActor &a, const std::string &model_name) {
    double v;
    if (data::lookup(data::MODEL_SIZES, model_name, v)) a.scale[0] = a.scale[1] = a.scale[2] = v;
    if (data::lookup(data::MODEL_ROTATIONS, model_name, v)) a.rot[1] = v;
}

void set_small_key_params(LebRoom &r, const std::string &model_path, const std::string &model_name,
                          const std::string &room, const std::string &item_key, size_t key_index) {
    std::vector<LebActor *> keys;
    for (LebActor &a : r.actors)
        if (a.type == 0xA9) keys.push_back(&a);
    if (keys.size() <= key_index) return;
    LebActor &k = *keys[key_index];
    k.params[1] = str_param(model_path);
    k.params[2] = str_param(model_name);
    k.params[3] = str_param(room);
    k.params[4] = str_param(item_key == "Seashell" ? "true" : "false");
    apply_model_look(k, model_name);
}

void set_rupee_params(LebRoom &r, const std::string &model_path, const std::string &model_name,
                      const std::string &entry_point, const std::string &item_key, int rup_index) {
    for (LebActor &rup : r.actors) {
        if (rup.type != 0xAB) continue;  // (the first one: the ones before were turned into another type)
        rup.type = 0x194;
        rup.params[0] = str_param(model_path);
        rup.params[1] = str_param(model_name);
        rup.params[2] = str_param(entry_point);
        rup.params[3] = str_param(rup_index == 0 ? "Lv10RupeeGet" : "Lv10RupeeGet_" + std::to_string(rup_index + 1));
        rup.params[4] = str_param(item_key == "Seashell" ? "true" : "false");
        apply_model_look(rup, model_name);
        return;
    }
}

std::string room_level(const std::string &room) {
    size_t cut = std::string::npos;  // (greedy: up to the last "_<2 digits><capital>")
    for (size_t i = 1; i + 3 < room.size(); i++)
        if (room[i] == '_' && isdigit((unsigned char)room[i + 1]) && isdigit((unsigned char)room[i + 2]) &&
            room[i + 3] >= 'A' && room[i + 3] <= 'Z')
            cut = i;
    if (cut == std::string::npos) throw std::runtime_error("las: not a room name: " + room);
    return room.substr(0, cut);
}

void set_loading_zone_target(LebRoom &r, const std::string &dest, size_t index) {
    std::vector<LebActor *> zones;
    for (LebActor &a : r.actors)
        if (a.type == 0x190) zones.push_back(&a);
    if (zones.size() <= index) return;
    zones[index]->params[0] = str_param(room_level(dest));
    zones[index]->params[1] = str_param(dest);
}

// ---- the generator's state
bool py_truthy(yyjson_val *v) {
    if (!v || yyjson_is_null(v)) return false;
    if (yyjson_is_bool(v)) return yyjson_get_bool(v);
    if (yyjson_is_num(v)) return yyjson_get_num(v) != 0;
    if (yyjson_is_str(v)) return yyjson_get_len(v) != 0;
    if (yyjson_is_arr(v)) return yyjson_arr_size(v) != 0;
    if (yyjson_is_obj(v)) return yyjson_obj_size(v) != 0;
    return false;
}

yyjson_val *setting_val(const Decisions &d, const std::string &key) { return yyjson_obj_get(d.raw("settings"), key.c_str()); }

bool setting_or(const Decisions &d, const std::string &key, bool dflt) {
    yyjson_val *v = setting_val(d, key);
    return v ? py_truthy(v) : dflt;
}

int64_t setting_int(const Decisions &d, const std::string &key, int64_t dflt) {
    yyjson_val *v = setting_val(d, key);
    if (yyjson_is_int(v)) return yyjson_get_sint(v);
    if (yyjson_is_bool(v)) return yyjson_get_bool(v);
    return dflt;
}

std::string placed(const Decisions &d, const std::string &check) {
    const std::string *s = d.item_at(check);
    return s ? *s : "";
}

int index_or(const Decisions &d, const std::string &check) { return d.index_at(check); }

bool has_index(const Decisions &d, const std::string &check) {
    return yyjson_obj_get(d.raw("indexes"), check.c_str()) != nullptr;
}

bool item_field(const Decisions &d, const std::string &item, const std::string &field, std::string &out) {
    const char *s = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(d.raw("item-defs"), item.c_str()), field.c_str()));
    if (!s) return false;
    out = s;
    return true;
}

std::string item_field(const Decisions &d, const std::string &item, const std::string &field) {
    std::string s;
    if (!item_field(d, item, field, s)) throw std::runtime_error("las: no " + field + " for item " + item);
    return s;
}

std::string item_key_at(const Decisions &d, const std::string &check) {
    std::string item = placed(d, check);
    if (item.empty()) throw std::runtime_error("las: nothing placed at " + check);
    return item_field(d, item, "item-key");
}

std::vector<std::string> starting_items(const Decisions &d) {
    std::vector<std::string> out;
    yyjson_val *a = d.raw("starting-items"), *v;
    yyjson_arr_iter it = yyjson_arr_iter_with(a);
    while ((v = yyjson_arr_iter_next(&it)))
        if (const char *s = yyjson_get_str(v)) out.push_back(s);
    return out;
}

std::string song(const Decisions &d, const std::string &bgm) {
    const char *s = yyjson_get_str(yyjson_obj_get(d.raw("songs"), bgm.c_str()));
    if (!s) throw std::runtime_error("las: no song recorded for " + bgm + " in the seed's decisions");
    return s;
}

}  // namespace las
}  // namespace rando
