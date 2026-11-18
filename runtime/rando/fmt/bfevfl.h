// Event flows (region_common/event/*.bfevfl): one flowchart (actors with their actions and queries, events linked by
// index, named entry points) or one timeline (actors, clips, one-shots, triggers, cuts), with parameter containers,
// a pooled string table and a relocation table.
//
// Written for LAS Recomp from the format (no code copied): EventFlow::write lays a file out exactly as evfl 1.2.0
// writes it after reading (the library the Python generator edits event flows with), so a flow edited here is the
// same file. The layout rules it follows:
//   header (0x28), the flowchart and timeline slots and their name dictionaries; a flowchart then writes its header,
//   actor records (0x38), event records (0x28), the entry point dictionary, entry point records (0x20), then each
//   event's data (switch cases before parameters), each actor's (parameters, actions, queries) and each entry
//   point's (its sub flow event list, then 0x18 bytes left empty), every block aligned to 8. A timeline writes its
//   actors' data and its parameters first, then its header and arrays. Strings go to one pool at the end (each
//   once, ordered by their bits read from the last byte's lowest bit up), except container strings, which are stored
//   in place. Dictionaries are Nintendo's radix trees, built by inserting the names in order.
// Things the writer recomputes: each entry point's sub flow events (from its event graph), action and query indices
// (the first of the actor's with that name), the number of switch cases. Bool arrays come out as int arrays (0 / 1)
// and a single true as 0x80000001, as evfl writes them. Event parameters are written only when not empty; actor,
// clip, one-shot, cut and timeline parameters whenever present (has_params).
#pragma once
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace rando {

static const uint16_t EVFL_NONE = 0xFFFF;  // (no event / entry point)

struct EvflActorId {
    std::string name, sub_name;
    bool operator==(const EvflActorId &o) const { return name == o.name && sub_name == o.sub_name; }
};

struct EvflValue {
    enum Type : uint8_t {
        Argument = 0, Int = 2, Bool = 3, Float = 4, String = 5, IntArray = 7, BoolArray = 8, FloatArray = 9,
        StringArray = 10, ActorIdentifier = 12
    };
    Type type = Int;
    int32_t i = 0;
    bool b = false;
    float f = 0;
    std::string s;  // (String, Argument: the argument's name)
    std::vector<int32_t> ints;
    std::vector<bool> bools;
    std::vector<float> floats;
    std::vector<std::string> strs;
    EvflActorId actor;

    static EvflValue of_int(int32_t v) { EvflValue x; x.type = Int, x.i = v; return x; }
    static EvflValue of_bool(bool v) { EvflValue x; x.type = Bool, x.b = v; return x; }
    static EvflValue of_float(float v) { EvflValue x; x.type = Float, x.f = v; return x; }
    static EvflValue of_str(const std::string &v) { EvflValue x; x.type = String, x.s = v; return x; }
    static EvflValue of_arg(const std::string &v) { EvflValue x; x.type = Argument, x.s = v; return x; }
    static EvflValue of_actor(const std::string &name, const std::string &sub = "") {
        EvflValue x;
        x.type = ActorIdentifier, x.actor = {name, sub};
        return x;
    }
};

struct EvflContainer {
    std::vector<std::pair<std::string, EvflValue>> items;  // (in order; names unique)
    EvflValue *get(const std::string &key);                // nullptr: none
    const EvflValue *get(const std::string &key) const;
    void set(const std::string &key, const EvflValue &v);  // (replaces in place, or appends)
    bool empty() const { return items.empty(); }
};

struct EvflActor {
    EvflActorId id;
    std::string argument_name;
    uint16_t argument_entry_point = EVFL_NONE;
    std::vector<std::string> actions, queries;
    bool has_params = false;
    EvflContainer params;
    uint16_t concurrent_clips = 0xFFFF;  // (the game's flowcharts: 1)

    int find_action(const std::string &name) const;  // -1: none
    int find_query(const std::string &name) const;
    uint16_t add_action(const std::string &name);  // (the existing one's index if there is one)
    uint16_t add_query(const std::string &name);
};

struct EvflEvent {
    enum Type : uint8_t { Action = 0, Switch = 1, Fork = 2, Join = 3, SubFlow = 4 };
    std::string name;
    Type type = Action;
    uint16_t next = EVFL_NONE;        // Action, Join, SubFlow
    uint16_t actor = 0, action = 0;   // Action: actor, action; Switch: actor, query
    EvflContainer params;             // Action, Switch, SubFlow
    std::vector<std::pair<uint32_t, uint16_t>> cases;  // Switch: (value, event); values unique
    uint16_t join = 0;                // Fork
    std::vector<uint16_t> forks;      // Fork (at least one)
    std::string flowchart, entry_point;  // SubFlow (entry_point not empty)
};

struct EvflEntryPoint {
    std::string name;
    uint16_t main_event = EVFL_NONE;
};

struct EvflFlowchart {
    std::string name;
    std::vector<EvflActor> actors;
    std::vector<EvflEvent> events;
    std::vector<EvflEntryPoint> entry_points;

    int find_event(const std::string &name) const;  // -1: none
    int find_entry_point(const std::string &name) const;
    int find_actor(const std::string &name, const std::string &sub = "") const;
    uint16_t add_actor(const std::string &name, const std::string &sub = "");  // (the existing one if there is one)
};

struct EvflClip {
    float start_time = -1, duration = -1;
    uint16_t actor = 0, action = 0;
    uint8_t actor_concurrent_clip = 0xFF;
    bool has_params = false;
    EvflContainer params;
};

struct EvflOneshot {
    float time = -1;
    uint16_t actor = 0, action = 0;
    bool has_params = false;
    EvflContainer params;
};

struct EvflTrigger {
    enum Type : uint8_t { Enter = 1, Leave = 2, None = 0xFF };
    uint16_t clip = 0;
    uint8_t type = None;
};

struct EvflCut {
    float start_time = -1;
    uint32_t x4 = 0xFFFFFFFF;
    std::string name;
    bool has_params = false;
    EvflContainer params;
};

struct EvflTimeline {
    std::string name;
    float duration = -1;
    std::vector<EvflActor> actors;
    std::vector<EvflClip> clips;
    std::vector<EvflOneshot> oneshots;
    std::vector<EvflTrigger> triggers;  // (written sorted by when they fire)
    std::vector<std::string> subtimelines;
    std::vector<EvflCut> cuts;
    bool has_params = false;
    EvflContainer params;
};

struct EventFlow {
    std::string name;
    bool has_flowchart = false, has_timeline = false;  // (exactly one of them to write a file)
    EvflFlowchart flowchart;
    EvflTimeline timeline;

    static EventFlow parse(const std::string &data);  // throws on bad data
    std::string write() const;                        // "" with neither or both (as evfl); throws on bad links
};

}  // namespace rando
