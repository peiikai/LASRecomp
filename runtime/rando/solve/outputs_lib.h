// What the outputs' files (outputs.cpp, outputs_tracker.cpp, outputs_view.cpp) share. Internal to outputs*.cpp.
#pragma once

#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "logic.h"
#include "outputs.h"
#include "outputs_data.h"

namespace rando {
namespace solve {
namespace out {

bool starts(const std::string &s, const std::string &p);
bool ends(const std::string &s, const std::string &p);
std::string base_item(const std::string &item);  // re.sub(r'\[\d+\]$', '', item)
std::vector<std::string> split(const std::string &s, char sep);
std::string fmt(const char *f, ...);              // snprintf into a string
std::string py_str(const Value &v);               // str(x) (sets aren't Values: see set_repr)
std::string set_repr(const std::vector<std::string> &items);  // repr of a set of str iterated in that order
std::string py_float_repr(double d);              // repr(float)
std::string py_g(double d);                       // '%g' % d
std::string title_case(const std::string &s);     // str.title()

// a regex of the Python's (ECMAScript is enough for them), answers cached
class Re {
public:
    explicit Re(const char *pattern) : re_(pattern) {}
    bool match(const std::string &s) const;  // re.match on an anchored pattern / re.search: the pattern decides
private:
    std::regex re_;
    mutable std::unordered_map<std::string, bool> memo_;
};

// re.findall(r'[A-Za-z0-9][A-Za-z0-9\-\[\]]*', s) (brackets: false drops '[' and ']' from the tail class)
std::vector<std::string> words(const std::string &s, bool brackets = true);

// the seed's logic as the checks after the fill see it (las_headless: sh.logic_defs, with its check-only aliases)
struct World {
    Value defs;
    std::vector<std::string> keys;
    std::vector<std::string> type;
    Names names;
    std::unique_ptr<Logic> L;
    std::unordered_map<std::string, int> index;  // key -> node
    std::vector<std::vector<int>> readers;        // name id -> nodes whose conditions / region read it (node order)
    Access start;                                 // {k: 1 for settings that are True}
    std::vector<char> junk;                       // excluded + absent locations

    World(const Value &logic_defs, const Value &settings);
    int node(const std::string &key) const;  // -1: none
    bool check(int i, int param, const Access &a) const { return L->check((size_t)i, param, a); }
    bool check_safe(int i, int param, const Access &a) const;  // (an exception: false)
    int id(const std::string &name) { return names.id(name); }
};

struct Reach {
    std::vector<int> locs;  // the real checks reached (node indexes, sorted)
    int goal = 0;
    bool operator==(const Reach &o) const { return goal == o.goal && locs == o.locs; }
    bool operator!=(const Reach &o) const { return !(*this == o); }
    bool has(int i) const;
};

// hint_sheet._reach_fn / route._reach_fn: the final state of a playthrough as one fixpoint. pl: the item id each
// node holds (-1: none), only item / follower nodes read; extra: counts added to the start
class Reacher {
public:
    Reacher(World &w, const Value &settings, bool followers);
    Reach operator()(const std::vector<int> &pl, const std::vector<std::pair<int, int>> &extra = {}) const;
    long runs = 0;

private:
    World &w_;
    bool followers_;
    int tri_ = 0, tri_id_ = -1;
    std::vector<int> goal_ids_;
    std::vector<int> base_of_;  // name id -> its base item's id (itself when none)
    int base_id(int item) const;
};

// the shared state of one seed's outputs
struct Ctx {
    const OutputsInput &in;
    Outputs &res;
    std::unique_ptr<World> w;
    Value spheres;              // [[[loc, item], ...], ...]
    std::vector<int> pl;        // the placements over the nodes: item name id (-1: none)
    Value steps;                // hint_steps.json "steps"
    std::string seed;
    Re hs_junk{odata::HS_JUNK}, sv_junk{odata::SV_JUNK};
    std::map<std::string, std::vector<int64_t>> zones_cache;  // (hint_sheet.door_area: the Field's tile zones)
    bool zones_loaded = false;

    explicit Ctx(const OutputsInput &i, Outputs &r) : in(i), res(r) {}
    void write(const std::string &name, const std::string &data) { res.files.emplace_back(name, data); }
    const std::string *file(const std::string &name) const;  // one of ours, already made
    bool seed_file(const std::string &rel, std::string &data) const { return in.read_seed && in.read_seed(rel, data); }
    bool romfs_file(const std::string &rel, std::string &data) const;  // the game's file
    bool level_file(const std::string &rel, std::string &data) const;  // the seed's copy, else the game's
};

// set semantics on a settings value that the Python holds as a set (recorded as a list)
std::set<std::string> str_set(const Value &v);
// sorted dict keys of a Value dict
std::vector<std::string> sorted_keys(const Value &d);

void build_tracker(Ctx &c, const Value &settings_json);
// the names the spoiler view and the tracker give an item / a location (outputs_view.cpp, outputs_tracker.cpp)
std::string spoiler_item_name(const std::string &item);
std::string spoiler_location_name(const std::string &loc);
std::string tracker_check_name(const std::string &name);
void build_spoiler_view(Ctx &c, const Value &settings_json);
std::string door_name_of(const std::string &dest_level);  // hint_sheet.door_name for a door's dest_level

}  // namespace out
}  // namespace solve
}  // namespace rando
