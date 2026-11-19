// text_keys (outputs.h): every text the spoiler view and the check tracker can show, for the translation tables.
// The fixed lines and patterns below are the ones build_spoiler_view (outputs_view.cpp) and build_tracker
// (outputs_tracker.cpp) write: a new line there goes here too. Names come from the same tables and functions the
// outputs use, over every check the logic has.
#include <regex>

#include "outputs_lib.h"
#include "prepare_lib.h"

namespace rando {
namespace solve {

std::vector<TextKey> text_keys() {
    using namespace odata;
    std::vector<TextKey> k;
    auto add = [&](const char *g, bool composed, const std::string &t) { k.push_back({g, composed, t}); };

    // ---- the spoiler view: its fixed lines (whole: false = a %s pattern, true = translated as parts)
    static const struct { bool composed; const char *text; } kSpoilerLines[] = {
        {true, "Route"},
        {true, "Only what you need to beat the seed. Each stop can be reached with the items from the stops before it."},
        {true, "Finish"},
        {true, "Spheres"},
        {true, "Each sphere can be done with what you had after the previous spheres (any order within a sphere)."},
        {true, "Only items that open things up are listed; rupees, traps and similar are left out."},
        {true, "Items by area"},
        {true, "Dungeon entrances"},
        {true, "Dungeon rooms"},
        {true, "Where each moved room is now and where it came from."},
        {true, "Entrances"},
        {true, "Doors (each door and where it now leads)"},
        {false, "Settings: %s"},
        {true, "(This seed has no sphere list.)"},
        {false, "Sphere %s"},
        {false, "%s entrance leads into %s"},
        {false, "Unchanged (%s)"},
        {true, "Exits (each way out and where it puts you)"},
        {false, "Seed ID: %s"},
        {false, "Starting items: %s"},
        {true, "Hints"},
        {false, "%s:"},
        {false, "Small items (%s)"},
        {false, "%s: %s"},
        {false, "%s - enter through %s's entrance"},
        {false, "%s's Soul"},
        {false, "%s' Soul"},
        {false, "room %s"},  // ("Double stalfos (room A3)", "Catfish's Maw, room D7": room shuffle)
        {false, "The last of the %s Triforce pieces you need ends the game."},
        {true, "2nd"},  // ("Shield (2nd)": the second copy of a progressive item)
    };
    for (const auto &l : kSpoilerLines) add("spoiler", l.composed, l.text);
    for (const char *s : {"shop-slot3-2nd", "shop-slot5-2nd", "shop-slot5-3rd"})  // (the Town Tool Shop's next items)
        add("spoiler", true, out::tracker_check_name(s));
    for (const auto &e : EN_NAMES) add("spoiler", true, e.second);
    for (const char *p : SV_PLACE)
        if (*p) add("spoiler", true, p);
    for (const StrMap *m : {&SV_AREAS, &SV_DUNGEONS, &SV_DPREFIX})
        for (const auto &e : *m) add("spoiler", true, e.second);
    for (const auto &e : SV_ITEMS) add("spoiler", true, out::spoiler_item_name(e.first));
    for (const auto &d : SV_DPREFIX)
        for (const char *kind : {"small-key", "nightmare-key", "compass", "map", "stone-beak"})
            add("spoiler", true, out::spoiler_item_name(std::string(d.first) + "-" + kind));
    // every check the logic has (real seeds add the numbered digs, bushes, pots and rocks, which the tool reads from
    // seed folders)
    std::vector<std::string> checks;
    for (const auto &e : prep_data().at("logic").dict()) {
        const Value &t = e.value.get("type");
        if (t.is_str() && (t.s() == "item" || t.s() == "follower")) checks.push_back(e.key);
    }
    for (const std::string &c : checks) {
        add("spoiler", true, out::spoiler_location_name(c));
        add("spoiler", true, out::tracker_check_name(c));
    }

    // ---- the check tracker
    for (const StrMap *m : {&CN_NAMES, &CN_KIND_WORD, &CN_HOUSES})
        for (const auto &e : *m) add("tracker", true, e.second);
    for (const char *p : TR_PLACE)
        if (*p) add("tracker", true, p);
    for (const char *w : {"north", "south", "east", "west", "north-west", "north-east", "south-west", "south-east",
                          "centre", "middle", "inside"})
        add("tracker", false, w);
    for (const char *p : {"Trendy Game: prize %s", "Pot (%s)", "Pot in %s", "%s of 3"}) add("tracker", false, p);
    for (const std::string &c : checks) add("tracker", true, out::tracker_check_name(c));
    return k;
}

}  // namespace solve
}  // namespace rando
