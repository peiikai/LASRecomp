// The generator's shared helpers, ported for the steps (seed_build.h): LAS-Randomizer's RandomizerCore/Tools/
// event_tools.py and the sheet helpers of Tools/oead_tools.py (MIT, Owen-Splat and contributors), leb.py's Room
// helpers, and the generator state the steps read from the seed's decisions (settings, placements, item tables).
//
// Each helper does exactly what its Python does, quirks included, so a step built from them makes the same file:
//   - events are named by name, as the Python passes them; "" stands for None. Looking up a name that isn't there
//     gives None, silently (a link to it is left out, an insert after it does nothing), as findEvent does;
//   - every added event takes the seed's next AutoEvent name (SeedBuild::next_event_name), so names match evfl's;
//   - C++ leaves the order of a call's arguments open: never create two events in one expression (the Python
//     creates them left to right, and the names follow that order).
#pragma once
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "seed_build.h"

namespace rando {
namespace las {

// ---- event flow values (evfl containers: Python bool / int / float / str as Bool / Int / Float / String)
struct V {
    EvflValue v;
    V(bool b) : v(EvflValue::of_bool(b)) {}
    V(int i) : v(EvflValue::of_int(i)) {}
    V(double f) : v(EvflValue::of_float((float)f)) {}
    V(const char *s) : v(EvflValue::of_str(s)) {}
    V(const std::string &s) : v(EvflValue::of_str(s)) {}
    V(const EvflValue &x) : v(x) {}
};
inline EvflValue arg(const std::string &name) { return EvflValue::of_arg(name); }  // evfl.common.Argument

struct KV {  // one key of a Python params dict
    std::string k;
    EvflValue v;
    KV(const char *key, const V &val) : k(key), v(val.v) {}
};
using Params = std::vector<KV>;  // (in the dict's order)
EvflContainer container(const Params &p);

struct ActionDef {  // ('Link', 'Heal', {'amount': 99})
    std::string actor, action;
    Params params;
};
using ActionDefs = std::vector<ActionDef>;
using Cases = std::vector<std::pair<uint32_t, std::string>>;  // a switch's {value: event name}, in the dict's order

// event_tools on one flowchart of the seed's build (b: for the AutoEvent counter and the decisions)
class Flow {
public:
    Flow(SeedBuild &b, EvflFlowchart &fc) : b(b), fc(fc) {}
    SeedBuild &b;
    EvflFlowchart &fc;

    EvflEvent *find(const std::string &name);             // findEvent (nullptr: None)
    EvflEvent &event(const std::string &name);            // (throws when it isn't there: the Python's AttributeError)
    EvflEntryPoint *find_entry(const std::string &name);  // findEntryPoint
    uint16_t actor(const std::string &name, const std::string &sub = "");  // findActor: added when missing
    void add_entry_point(const std::string &name);        // addEntryPoint

    bool insert_after(const std::string &prev, const std::string &next);  // insertEventAfter (prev: event or entry point)
    bool set_case(const std::string &sw, uint32_t value, const std::string &next);  // setSwitchEventCase
    void remove_after(const std::string &name);                                     // removeEventAfter
    void set_song(const std::string &event, const std::string &song);               // setEventSong
    void add_forks(const std::string &fork, const std::vector<std::string> &branches);  // addForkEventForks
    bool set_fork(const std::string &fork, size_t i, const std::string &next);          // setForkEventFork (by position)

    // createActionEvent ("Actor[Sub]" names a sub-actor: the action is added to an actor of the whole name, as there)
    std::string action(const std::string &actor, const std::string &action, const Params &p, const std::string &next = "");
    // createSwitchEvent (a "value1" parameter becomes an argument; cases going to None or a missing event are left out)
    std::string sw(const std::string &actor, const std::string &query, Params p, const Cases &cases);
    std::string subflow(const std::string &chart, const std::string &entry, const Params &p, const std::string &next = "");
    std::string join(const std::string &next = "");  // createJoinEvent
    // createForkEvent: (fork, join); the join is added first, then the fork, which goes after `before`
    std::pair<std::string, std::string> fork(const std::string &before, const std::vector<std::string> &forks,
                                             const std::string &next = "");
    // createActionChain: the first event's name ("" for no events); every event but the first is created going to
    // `after` and then linked from the one before (as the Python)
    std::string action_chain(const std::string &before, const ActionDefs &defs, const std::string &after = "");
    void insert_action_chain(const std::string &before, const std::vector<std::string> &events);
    std::string progressive_switch(const std::string &item1, const std::string &item2, const std::string &flag,
                                   const std::string &before = "", const std::string &after = "");

private:
    std::string add(EvflEvent e);  // flowchart.add_event: named AutoEvent<n>, appended
    uint16_t index_of(const std::string &name) const;  // (EVFL_NONE: None)
};

// ---- data sheets (oead gsheet values: kinds as the Python's literal types)
inline GValue gi(int32_t v) { return GValue::of_int(v); }
inline GValue gf(float v) { return GValue::of_float(v); }
inline GValue gb(bool v) { return GValue::of_bool(v); }
inline GValue gs(const std::string &v) { return GValue::of_str(v); }
inline GValue gst(const GStruct &v) { return GValue::of_struct(v); }
GValue gsa(const std::vector<GStruct> &items);         // a struct array
GValue gstrs(const std::vector<std::string> &items);   // a string array
GValue gfloats(const std::vector<float> &items);       // a float array
GValue gints(const std::vector<int32_t> &items);       // an int array

GValue &at(GStruct &s, const std::string &name);  // s[name] (throws when missing: the Python's KeyError)
const GValue &at(const GStruct &s, const std::string &name);
GStruct &item(GValue &arr, size_t i);              // a struct array's i-th item (throws when out of range)

GStruct cond(int32_t category, const std::string &parameter);  // {'category': c, 'parameter': p}
// oead_tools.createCondition: {'symbol': name, 'conditions': [(category, parameter), ...]} (a Conditions row)
GStruct create_condition(const std::string &name, const std::vector<std::pair<int32_t, std::string>> &checks);
GStruct create_event_trigger(int32_t condition, const std::vector<std::pair<int32_t, std::string>> &additional,
                             const std::string &entry_point);  // oead_tools.createEventTrigger (an Npc trigger)
GStruct create_behavior(int32_t type, const std::vector<std::string> &datas = {});  // createBehavior
GField create_field(const std::string &name, const std::string &type_name, uint8_t type, uint16_t offset,
                    uint16_t flags = 0);  // createField

// ---- rooms (leb.py Room helpers)
LebParam str_param(const std::string &s);  // bytes(s, 'utf-8')
// setChestContent: the chest_index-th chest (type 0xF7); index -1 is written as an empty string; no model: unchanged
void set_chest_content(LebRoom &r, const std::string &content, const LebParam &index, size_t chest_index = 0,
                       double size = 1.0, const std::string *model = nullptr);
void set_chest_content(LebRoom &r, const std::string &content, int index, size_t chest_index = 0, double size = 1.0,
                       const std::string *model = nullptr);  // (a placement's index: -1 for none)
void set_small_key_params(LebRoom &r, const std::string &model_path, const std::string &model_name,
                          const std::string &room, const std::string &item_key, size_t key_index = 0);
void set_rupee_params(LebRoom &r, const std::string &model_path, const std::string &model_name,
                      const std::string &entry_point, const std::string &item_key, int rup_index = 0);
void set_loading_zone_target(LebRoom &r, const std::string &destination, size_t index = 0);
// a room's level: re.match('(.+)_\\d\\d[A-Z]', room).group(1) ("Field_11C" -> "Field"; throws when it doesn't match)
std::string room_level(const std::string &room);
// an item actor's model size and rotation (data.MODEL_SIZES / MODEL_ROTATIONS), as the Python sets them
void apply_model_look(LebActor &a, const std::string &model_name);

// ---- the generator's state, from the decisions (mod_generator's self.settings / placements / item_defs)
bool py_truthy(yyjson_val *v);  // Python's truth of a JSON value (None, false, 0, "", [], {}: false)
yyjson_val *setting_val(const Decisions &d, const std::string &key);  // settings[key] (nullptr: missing)
bool setting_or(const Decisions &d, const std::string &key, bool dflt);  // bool(settings.get(key, dflt))
int64_t setting_int(const Decisions &d, const std::string &key, int64_t dflt = 0);
std::string placed(const Decisions &d, const std::string &check);   // placements[check] ("" when none)
int index_or(const Decisions &d, const std::string &check);          // placements['indexes'].get(check, -1)
bool has_index(const Decisions &d, const std::string &check);
std::string item_key_at(const Decisions &d, const std::string &check);  // item_defs[placements[check]]['item-key']
// item_defs[item][field]: item-key, model-path, model-name and (recorded by record_las_items.py) type, chest-type,
// npc-key; false when the item or the field isn't there
bool item_field(const Decisions &d, const std::string &item, const std::string &field, std::string &out);
std::string item_field(const Decisions &d, const std::string &item, const std::string &field);  // (throws if missing)
std::vector<std::string> starting_items(const Decisions &d);  // placements['starting-items'] (item names)
std::string song(const Decisions &d, const std::string &bgm);  // mod_generator.songs_dict[bgm] (randomize-music)

}  // namespace las
}  // namespace rando
