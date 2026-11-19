// The native seed generator (generate.h): tools/rando/las_headless.py main(), its parts joined in its order.
//
// Between the parts, what main() itself does is done here as it does it: the dungeon room shuffle's planning loop and
// its retries (rooms.h; a layout the fill or the playthrough rejects: the next one, as the Python's fresh run of itself
// with LARECOMP_ROOM_ATTEMPT), the fill's errors ('error <text>' for a Triforce hunt the pool can't hold), the indexes it sets
// after the fill (pots.mark_refills, the bottles, pets.mark_indexes), the decisions record.py writes, the entrance
// spoiler (entrance_logic.spoiler) and the settings ModsProcess changes (assemble.preSetup's stealing) or a step adds
// (miniboss_guard's marker flag).
#include "generate.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "../mkdirs.h"
#include "../apply/decide.h"
#include "../apply/steps.h"
#include "entrances.h"
#include "outputs.h"
#include "rooms.h"
#include "shuffler.h"

namespace rando {
namespace solve {

namespace {

const char *const SET_KEYS[] = {"excluded-locations", "absent-locations", "junk-locations"};  // (Python sets)
// the runtime's tables (runtime/rando/seed.c kTables): the seed folder's file -> seed.json "tables" name
const std::pair<const char *, const char *> TABLES[] = {
    {"apple_trees.tsv", "apple_trees"}, {"bushes.tsv", "bushes"},       {"digs.tsv", "digs"},
    {"door_jumps.tsv", "door_jumps"},   {"dungeon_map.tsv", "dungeon_map"}, {"enemies_random.tsv", "enemies_random"},
    {"flags.tsv", "flags"},             {"flags_ext", "flags_ext"},     {"hints.tsv", "hints"},
    {"pots.tsv", "pots"},               {"quick_pickups.tsv", "quick_pickups"}, {"roam.tsv", "roam"},
    {"rocks.tsv", "rocks"},             {"shell_spots.tsv", "shell_spots"}, {"shopsanity.tsv", "shopsanity"},
    {"souls.tsv", "souls"},             {"tracker.tsv", "tracker"},     {"triforce.tsv", "triforce"},
};
// pots.REFILLS / REFILL_INDEX, pets.ORDER / PET_INDEX
const char *const REFILLS[] = {"pot-arrows", "pot-bombs", "pot-powder"};
const int REFILL_INDEX = 80;
const char *const PET_ORDER[] = {"rooster", "marin-companion", "ghost"};
const int PET_INDEX = 90;

std::string str_of(const Value &v) { return v.is_str() ? v.s() : v.repr(); }
bool ends_with(const std::string &s, const char *e) {
    size_t n = strlen(e);
    return s.size() >= n && !s.compare(s.size() - n, n, e);
}

// record._plain on the settings: a Python set (a list here, in CPython's order) sorted
Value plain_settings(const Value &settings) {
    Value s = settings.copy();
    for (const char *k : SET_KEYS) {
        if (!s.has(k) || !s.at(k).is_list()) continue;
        std::vector<std::string> items;
        for (const Value &x : s.at(k).list()) items.push_back(str_of(x));
        std::sort(items.begin(), items.end());
        Value l = Value::new_list();
        for (const std::string &x : items) l.list().push_back(x);
        s.set(k, l);
    }
    return s;
}

// record.decisions' own fields (the groups' fields are the build's: decide mode)
std::string decisions_doc(const Value &placements, const Value &settings, const Value &item_defs, const RoomPlan *rooms) {
    Value d = Value::new_dict();
    d.set("version", 1);
    Value pl = Value::new_dict();
    for (const auto &e : placements.dict())
        if (e.value.is_str()) pl.set(e.key, e.value);
    d.set("placements", pl);
    d.set("indexes", placements.get("indexes", Value::new_dict()));
    d.set("starting-items", placements.get("starting-items", Value::new_list()));
    d.set("dungeon-entrances", placements.get("dungeon-entrances", Value::new_dict()));
    d.set("settings", plain_settings(settings));
    Value defs = Value::new_dict();
    for (const auto &e : item_defs.dict()) {
        Value f = Value::new_dict();
        for (const char *k : {"item-key", "model-path", "model-name", "type", "chest-type", "npc-key"})
            if (e.value.has(k)) f.set(k, e.value.at(k));
        defs.set(e.key, f);
    }
    d.set("item-defs", defs);
    if (rooms) {  // (the dungeon room shuffle's plan: the build lays the rooms out from it)
        // (as lists, in the planner's order: decisions.json is written with sorted keys - record.ROOM_PLAN)
        Value rp = Value::new_dict(), lays = Value::new_list();
        for (const auto &lv : rooms->layouts.dict()) {
            Value cells = Value::new_list();
            for (const auto &c : lv.value.dict()) cells.list().push_back(Value(List{Value(c.key), c.value}));
            lays.list().push_back(Value(List{Value(lv.key), cells}));
        }
        rp.set("layouts", lays), rp.set("jumps", rooms->jumps);
        d.set("room-plan", rp);
    }
    return py_json(d);
}

// entrance_logic.spoiler(doors, entry, exit)
std::string entrance_spoiler(const Value &ent) {
    const List &e = ent.list();
    std::map<std::string, std::string> dest;  // door id -> its dest_level
    for (const Value &d : e[0].list()) dest[d.at("id").s()] = str_of(d.at("dest_level"));
    auto pad = [](const std::string &s, size_t w) { return s.size() < w ? s + std::string(w - s.size(), ' ') : s; };
    const Value &entry = e[1], &exit_ = e[2];
    std::string o = "Entrances (door -> interior it leads into):";
    std::vector<std::string> doors = entry.dict().keys();
    std::sort(doors.begin(), doors.end());
    for (const std::string &d : doors) {
        const std::string t = entry.at(d).s();
        auto it = dest.find(t);
        if (it == dest.end()) throw PyError("KeyError", py_repr(t));
        o += "\n  " + pad(d, 32) + " -> " + pad(t, 32) + " (" + it->second + ")";
    }
    bool moved = false;
    for (const auto &kv : entry.dict()) moved |= !exit_.has(kv.value.s()) || exit_.at(kv.value.s()) != Value(kv.key);
    if (moved) {
        o += "\nExits (interior -> outside of door):";
        std::vector<std::string> k = exit_.dict().keys();
        std::sort(k.begin(), k.end());
        for (const std::string &t : k) o += "\n  " + pad(t, 32) + " -> " + str_of(exit_.at(t));
    }
    return o;
}

// a file's lines as seed.json's tables hold them (split at '\n', no empty last line: runtime/rando/seed.c)
Value table_lines(const std::string &text) {
    Value rows = Value::new_list();
    for (size_t a = 0; a < text.size();) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        rows.list().push_back(text.substr(a, b - a));
        a = b + 1;
    }
    return rows;
}

}  // namespace

namespace {

// one run of las_headless.py; room_first: LARECOMP_ROOM_ATTEMPT. retry: set to the next layout to try when this one is
// rejected (the Python then runs itself again with it, and that run's result is the seed's)
GenerateResult generate_once(const Value &request, const GameFiles &game, const GenerateOutput &out,
                             const std::function<void(double, const std::string &)> &progress,
                             const std::atomic<bool> *cancel, const GenerateOptions &opt, int room_first, int &retry) {
    retry = -1;
    GenerateResult r;
    auto t0 = std::chrono::steady_clock::now();
    auto stage = [&](double f, const std::string &label) {
        if (progress) progress(f, label);
    };
    auto cancelled = [&]() { return cancel && cancel->load(std::memory_order_relaxed); };
    auto fail = [&](GenStatus st, const std::string &why) -> GenerateResult & {
        r.status = st, r.error = why;
        size_t th = why.find("RuntimeError: Triforce hunt: ");  // (a reason the player can act on: 'error <text>')
        if (st == GenStatus::Failed && th != std::string::npos)
            r.player_error = why.substr(th + 14, why.find('\n', th) - th - 14);
        else if (st == GenStatus::Failed && fill_gave_up(why))  // (too few places for the items)
            r.player_error = GIVE_UP_TEXT;
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return r;
    };
    auto say = [&](const std::string &s) { r.log.push_back(s); };

    stage(0.01, "Preparing");
    Prepared st;
    Value ent, post_entrances;
    RoomPlan room_plan;
    bool has_rooms = false;
    int room_attempt = 0;
    // a layout the items can't be placed in / the seed came out unbeatable: las_headless.retry_layout
    auto retry_layout = [&](const std::string &why) {
        if (!has_rooms || room_attempt + 1 >= 40) return false;
        say("room layout " + std::to_string(room_attempt + 1) + ": " + why + " - planning another");
        retry = room_attempt + 1;
        return true;
    };
    try {
        st = prepare(request, game);
        for (const std::string &s : st.said) say(s);
        if (cancelled()) return fail(GenStatus::Cancelled, "cancelled");
        Value state = Value::new_dict();
        state.set("settings", st.settings), state.set("item_defs", st.item_defs), state.set("logic_defs", st.logic_defs);
        ent = shuffle_entrances(state, game.read, say);
        // (decide mode's input: the entrance shuffle's result and the logic as it left it - the 'post_entrances' state)
        Value pe = Value::new_dict();
        pe.set("ent", ent), pe.set("logic_defs", st.logic_defs);
        post_entrances = Value(pe.json());
        // ---- dungeon room shuffle: the layouts first, the item logic follows them (room_logic.apply); a layout whose
        // dungeons can't be filled: the next one (sub-seeds '<seed>~<attempt>')
        if (st.settings.get("dungeon-room-shuffle", "off") != Value("off")) {
            stage(0.03, "Planning dungeon rooms");
            room_attempt = plan_rooms(st, room_first, room_plan, say, cancelled);
            has_rooms = true;
        }
        st.said.clear();
        std::vector<std::string> shuffled;
        if (has_rooms)
            for (const auto &e : room_plan.layouts.dict()) shuffled.push_back(e.key);
        finish_prefill(st, shuffled);
        for (const std::string &s : st.said) say(s);
    } catch (const std::exception &e) {
        if (cancelled()) return fail(GenStatus::Cancelled, "cancelled");  // (plan_rooms stops when asked)
        return fail(GenStatus::Failed, e.what());
    }
    if (cancelled()) return fail(GenStatus::Cancelled, "cancelled");

    // ---- the item fill
    stage(0.04, "Shuffling items");
    Value settings = st.settings, item_defs = st.item_defs;
    ItemShuffler sh(str_of(settings.at("seed")), settings, item_defs, st.logic_defs.deepcopy());
    sh.log = say;
    auto last = std::chrono::steady_clock::now();
    sh.tick = [&]() {
        if (cancelled()) sh.stop();
        auto now = std::chrono::steady_clock::now();
        if (!progress || now - last < std::chrono::milliseconds(500)) return;
        last = now;
        size_t done, total;
        sh.placed(done, total);
        char label[64];
        snprintf(label, sizeof label, "Shuffling items (%zu/%zu placed)", done, total);
        stage(0.04 + 0.62 * (double)done / (double)std::max<size_t>(1, total), label);
    };
    Value placements;
    try {
        placements = sh.run();
    } catch (const std::exception &e) {
        if (cancelled()) return fail(GenStatus::Cancelled, "cancelled");
        std::string why = e.what();
        if (has_rooms && why.find("fill stuck:") != std::string::npos && room_attempt + 1 < 40) {
            say("room layout " + std::to_string(room_attempt) + ": item fill stuck - planning another");
            retry = room_attempt + 1;
        } else if (why.find("Triforce hunt") == std::string::npos) {
            retry_layout("the items can't be placed");
        }
        return fail(GenStatus::Failed, why);
    }
    sh.tick = nullptr;
    say("placements ready");
    if (cancelled()) return fail(GenStatus::Cancelled, "cancelled");

    // ---- the outputs up to the playthrough's verdict
    stage(0.67, "Checking the seed is beatable");
    OutputsInput oin;
    oin.settings = sh.settings;
    oin.placements = placements;
    oin.logic_defs = sh.logic_defs;
    oin.ent = ent;
    oin.read_game = game.read;
    oin.list_game = game.list;
    oin.folder_name = opt.folder_name;
    if (has_rooms) oin.room_layouts = room_plan.layouts;
    OutputsBuilder ob(oin);
    try {
        Outputs &o = ob.before_build();
        for (const std::string &s : o.log) say(s);
        if (o.exit_code == 3) {
            retry_layout("unbeatable");
            return fail(GenStatus::Unbeatable, "unbeatable seed");
        }
    } catch (const std::exception &e) {
        return fail(GenStatus::Failed, e.what());
    }
    auto made = [&](const char *name) -> const std::string * {
        for (const auto &f : ob.result().files)
            if (f.first == name) return &f.second;
        return nullptr;
    };

    // ---- main() between the fill and the game files: the indexes the runtime reads for refills, bottles and pets
    Value indexes = placements.at("indexes");
    if (st.pot_locs.truthy() || st.bush_locs.truthy() || st.dig_locs.truthy() || st.rock_locs.truthy())
        for (const auto &e : placements.dict())  // (pots.mark_refills)
            if (e.key != "indexes" && e.value.is_str())
                for (const char *x : REFILLS)
                    if (e.value.s() == x) indexes.set(e.key, REFILL_INDEX);
    for (const std::string &loc : indexes.dict().keys())  // (the Escape Bottle is the third slot, the Hint Sheet the first)
        if (placements.get(loc) == Value("bottle")) indexes.set(loc, 1);
    if (!st.pets.empty())  // (pets.mark_indexes, on main()'s logic)
        for (const auto &e : placements.dict()) {
            if (!e.value.is_str()) continue;
            const Value *def = st.logic_defs.dict().find(e.key);
            if (!def || !def->is_dict() || def->get("type") != Value("item")) continue;
            for (int i = 0; i < 3; i++)
                if (e.value.s() == PET_ORDER[i]) indexes.set(e.key, PET_INDEX + i);
        }
    // the settings as ModsProcess leaves them (assemble.preSetup: stealing always / never become True / False)
    Value final_settings = sh.settings.copy();
    if (final_settings.get("stealing") == Value("always")) final_settings.set("stealing", true);
    else if (final_settings.get("stealing") == Value("never")) final_settings.set("stealing", false);

    // ---- the seed build, deciding as it goes
    Decisions d;
    if (!d.load(decisions_doc(placements, final_settings, item_defs, has_rooms ? &room_plan : nullptr)))
        return fail(GenStatus::BuildFailed, "decisions");
    std::vector<uint32_t> rs;
    for (int64_t w : sh.random_state()) rs.push_back((uint32_t)w);
    Decider dc(d, rs);
    for (const char *in : {"hint_steps.json", "seed_hints.json", "logic.json"})
        if (const std::string *f = made(in)) dc.set_input(in, *f);
    dc.set_input("post_entrances.json", post_entrances.s());
    SeedBuild b(game.read, d);
    b.set_game_list(game.list);
    b.set_decider(&dc);
    b.keep_writes(false);
    const std::vector<BuildStep> &steps = build_steps();
    size_t k = 0;
    try {
        for (const BuildStep &s : steps) {
            k++;
            if (cancelled()) return fail(GenStatus::Cancelled, "cancelled");
            if (!build_step_runs(s.name, d)) continue;
            char label[64];
            snprintf(label, sizeof label, "Building game files (%zu tasks)", k);
            stage(0.72 + 0.26 * (double)k / (double)steps.size(), label);
            s.run(b);
        }
    } catch (const std::exception &e) {
        return fail(GenStatus::BuildFailed, std::string(steps[k - 1].name) + ": " + e.what());
    }
    // miniboss_guard's marker flag (settings['miniboss-marker-flag'], set while it wrote the seed)
    if (final_settings.get("enemy-minibosses").truthy() && b.files().count("region_common/datasheets/GlobalFlags.gsheet"))
        for (const GStruct &v : b.read_sheet("GlobalFlags.gsheet").values) {
            const GValue *sym = nullptr, *idx = nullptr;
            for (const auto &f : v) {
                if (f.first == "symbol") sym = &f.second;
                if (f.first == "index") idx = &f.second;
            }
            if (sym && idx && sym->s == "OverworldMinibossMarker") final_settings.set("miniboss-marker-flag", (long long)idx->i);
        }
    d.set_field("settings", py_json(plain_settings(final_settings)));
    std::string decisions;
    try {
        decisions = dc.document();
    } catch (const std::exception &e) {
        return fail(GenStatus::BuildFailed, e.what());
    }
    if (cancelled()) return fail(GenStatus::Cancelled, "cancelled");

    // ---- the outputs after the files: settings.json, the tracker, the readable spoiler
    stage(0.99, "Building the check tracker");
    const std::string seed = str_of(settings.at("seed"));
    std::string ent_txt;
    if (ent.truthy()) ent_txt = entrance_spoiler(ent) + "\n";
    try {
        ob.after_build(final_settings, [&](const std::string &rel, std::string &data) {
            if (!rel.compare(0, 6, "romfs/")) {
                auto f = b.files().find(rel.substr(6));
                if (f == b.files().end()) return false;
                data = f->second;
                return true;
            }
            if (ent.truthy() && rel == "entrances_" + seed + ".txt") return data = ent_txt, true;
            const std::string *t = b.table(rel);
            if (!t) return false;
            data = *t;
            return true;
        });
    } catch (const std::exception &e) {
        return fail(GenStatus::Failed, e.what());
    }
    const Outputs &o = ob.result();

    // ---- the seed folder
    std::vector<std::pair<std::string, const std::string *>> files;  // (path, bytes)
    std::string seed_json;
    {
        Value rec = Value::new_dict(), tables = Value::new_dict();
        rec.set("format", "lasrecomp-seed");
        rec.set("version", 1);
        rec.set("seed_id", seed);
        const std::string *sj = made("settings.json");
        rec.set("settings", sj ? Value::parse_json(sj->data(), sj->size()) : Value::new_dict());
        for (const auto &t : TABLES) {
            const std::string *text = b.table(t.first);
            if (!text && !strcmp(t.first, "tracker.tsv")) text = made("tracker.tsv");
            if (text) tables.set(t.second, table_lines(*text));
        }
        rec.set("tables", tables);
        rec.set("decisions", Value::parse_json(decisions.data(), decisions.size()));
        seed_json = py_json(rec, 1) + "\n";
    }
    files.push_back({"seed.json", &seed_json});
    for (const auto &f : b.files())
        if (!f.first.compare(0, 6, "exefs/")) files.push_back({f.first, &f.second});
    for (const char *n : {"spoiler.txt", "spoiler_view.txt", "spoiler.html"})
        if (const std::string *f = made(n)) files.push_back({n, f});
    const std::string ent_name = "entrances_" + seed + ".txt";
    if (ent.truthy()) files.push_back({ent_name, &ent_txt});
    for (const auto &t : b.tables())
        if (ends_with(t.first, ".txt")) files.push_back({t.first, &t.second});
    if (opt.legacy) {
        for (const auto &f : b.files())
            if (f.first.compare(0, 6, "exefs/")) files.push_back({"romfs/" + f.first, &f.second});
        for (const auto &t : b.tables())
            if (!ends_with(t.first, ".txt")) files.push_back({t.first, &t.second});
        files.push_back({"decisions.json", &decisions});
        for (const char *n : {"settings.json", "logic.json", "spheres.json", "hint_steps.json", "route.json",
                              "seed_hints.json", "tracker.json", "tracker.tsv"})
            if (const std::string *f = made(n)) files.push_back({n, f});
    }
    (void)o;
    for (const auto &f : files) {
        if (out.write && !out.write(f.first, *f.second)) return fail(GenStatus::WriteFailed, "can't write " + f.first);
        r.files.push_back(f.first);
    }
    stage(1.0, "Done");
    r.status = GenStatus::Done;
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

}  // namespace

GenerateResult generate(const Value &request, const GameFiles &game, const GenerateOutput &out,
                        const std::function<void(double, const std::string &)> &progress,
                        const std::atomic<bool> *cancel, const GenerateOptions &opt) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> log;
    for (int first = 0;;) {
        int retry = -1;
        GenerateResult r = generate_once(request, game, out, progress, cancel, opt, first, retry);
        log.insert(log.end(), r.log.begin(), r.log.end());
        if (retry < 0) {
            r.log = log;
            r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            return r;
        }
        first = retry;  // (the next layout, in a run of its own: the seed is that run's)
    }
}

GenerateOutput generate_to_dir(const std::string &dir) {
    GenerateOutput o;
    o.write = [dir](const std::string &path, const std::string &bytes) {
        std::string p = dir + "/" + path;
        for (size_t i = 1; i < p.size(); i++)
            if (p[i] == '/') make_dir(p.substr(0, i));
        std::ofstream f(p, std::ios::binary);
        f << bytes;
        return (bool)f;
    };
    return o;
}

int generate_main(const std::string &romfs, const std::string &request, const std::string &out, bool legacy) {
    Value req;
    try {
        req = Value::load_json(request);
    } catch (const std::exception &e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    GenerateOptions opt;
    opt.legacy = legacy;
    std::string o = out;
    while (o.size() > 1 && o.back() == '/') o.pop_back();
    opt.folder_name = o.substr(o.rfind('/') == std::string::npos ? 0 : o.rfind('/') + 1);
    make_dir(o);
    GenerateResult r = generate(req, game_files_dir(romfs), generate_to_dir(o),
                                [](double f, const std::string &label) { printf("stage %.3f %s\n", f, label.c_str()), fflush(stdout); },
                                nullptr, opt);
    for (const std::string &l : r.log) printf("%s\n", l.c_str());
    if (r.status == GenStatus::Done) {
        printf("done (%zu files, %.2f s)\n", r.files.size(), r.seconds);
        return 0;
    }
    if (!r.player_error.empty()) printf("error %s\n", r.player_error.c_str());
    fprintf(stderr, "SHUFFLE ERROR %s\n", r.error.c_str());
    switch (r.status) {
    case GenStatus::BuildFailed: return 2;
    case GenStatus::Unbeatable: return 3;
    case GenStatus::RoomShuffle: return 4;
    default: return 1;
    }
}

}  // namespace solve
}  // namespace rando
