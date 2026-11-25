// LAS Recomp's item steps: shopsanity, trendsanity, pets, souls, Triforce, Escape Bottle, Hint Sheet, hints, keysanity / bag texts, refills, silent and quick pickups: the generator's steps ported (seed_build.h); each makes exactly the writes its Python does
// (lasrando check-steps, tools/rando/test_apply.py), and the seed's tables it writes (shopsanity.tsv, souls.tsv,
// triforce.tsv, hints.tsv, quick_pickups.tsv, flags.tsv and their lines of check_flags.tsv / flags_ext).
//
// Each step is one call of las_headless.py into a tools/rando module; its name lists the functions the write log
// names its writes by (the innermost writing one): "a|b" (writes from the first one named a on), "p>a|b" (and writes
// named p just before it); tools/lasrando.cpp check-steps. Shared helpers: ours_items_lib.h, ours_items_text.h.
#include "ours_items_lib.h"
#include "steps.h"

namespace rando {

std::vector<BuildStep> steps_ours_items() {
    using namespace ours;
    return {
        {"shopsanity.apply|shopsanity.texts", shopsanity_apply},
        {"trendsanity.apply", trendsanity_apply},
        {"pets.apply|pets.write_messages", pets_apply},
        {"souls.apply|souls.hold_shutters|souls.boss_doors", souls_apply},
        {"triforce.apply", triforce_apply},
        {"escape_bottle.apply", escape_bottle_apply},
        {"hint_sheet.apply|hint_sheet._item_row", hint_sheet_apply},
        {"hint_sheet._src>seed_hints._owl_flows|seed_hints._split_rooms|seed_hints._phone_flow|hint_sheet._src|seed_hints.apply",
         seed_hints_apply},
        {"keysanity_text.apply|keysanity_text.write_messages", keysanity_text_apply},
        {"refill_gets.apply|bomb_bag_text.add_message", refill_gets_apply},
        {"arrow_bow_text.apply", arrow_bow_text_apply},
        {"bomb_bag_text.apply|bomb_bag_text.add_message", bomb_bag_text_apply},
        {"powder_bag_text.apply|bomb_bag_text.add_message", powder_bag_text_apply},
        {"silent_pickups._flow", silent_pickups_apply},
        // (tables only: no file writes)
        {"souls.write_spots", souls_write_spots},
        {"triforce.write_spots", triforce_write_spots},
        {"quick_pickups.apply", quick_pickups_apply},
        {"seed_tables.write_flags", seed_tables_write_flags},
    };
}

}  // namespace rando
