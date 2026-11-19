// Seed build: the seed's game-file changes, made in memory from the game's own files and the seed's decisions
// (docs/randomizer.md, stage 3).
//
// It mirrors the Python generator's file handling exactly, so the files come out the same: a file is read from its
// last written bytes, or the game's when this seed hasn't written it (each read parses afresh: an edit not written
// back is lost, as there); a write stores the serialised bytes. Steps run in the generator's order, and the event
// name counter (an added event is named AutoEvent<n>, one counter for the whole seed) runs through all of them.
// Nothing here draws a random number: the generator's choices come from the decisions (apply mode), or are made by a
// Decider (decide.h: decide mode).
#pragma once
#include <stdint.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../fmt/bfevfl.h"
#include "../fmt/gsheet.h"
#include "../fmt/leb.h"
#include "../fmt/lvb.h"
#include "../fmt/msbt.h"
#include "../fmt/sarc.h"

struct yyjson_doc;
struct yyjson_val;

namespace rando {

// The seed's decisions (seed.json "decisions", from tools/rando/record.py today; the native solver later).
class Decisions {
public:
    Decisions() = default;
    ~Decisions();
    Decisions(const Decisions &) = delete;
    Decisions &operator=(const Decisions &) = delete;
    bool load(const std::string &json);  // (the "decisions" object's text)

    const std::string *item_at(const std::string &check) const;  // placements: the item there (nullptr: none)
    int index_at(const std::string &check) const;                // its index, -1 if it has none
    struct ItemDef { std::string key, model_path, model_name; };
    const ItemDef *item_def(const std::string &item) const;
    bool setting(const std::string &key) const;  // the settings as the solver left them: true
    std::string setting_str(const std::string &key) const;
    // a trap's look at a check: the n-th draw there (getItemInfo draws once per call)
    bool trap_model(const std::string &check, size_t n, std::string &path, std::string &name) const;
    // a top-level field of the decisions as it is in the JSON (nullptr: none), for the fields a group's steps record
    // themselves (tools/rando/record_<group>.py)
    yyjson_val *raw(const std::string &field) const;

    // decide mode (decide.h): a field set by the build itself (raw() reads it in place of the loaded one)
    void set_field(const std::string &field, const std::string &json);
    void drop_field(const std::string &field);  // (a loaded field the build decides: unread until set)
    std::vector<std::string> field_names() const;  // (sorted)

private:
    std::map<std::string, yyjson_doc *> set_;
    std::map<std::string, bool> dropped_;
    yyjson_doc *doc_ = nullptr;
    yyjson_val *root_ = nullptr;
    std::map<std::string, ItemDef> defs_;
};

class Decider;

class SeedBuild {
public:
    // reads a game file by its romfs path ("region_common/level/Field/Field_16C.leb"); false: no such file
    using GameRead = std::function<bool(const std::string &path, std::string &out)>;

    SeedBuild(GameRead game, const Decisions &d) : game_(std::move(game)), d_(d) {}

    const Decisions &decisions() const { return d_; }
    // decide mode (decide.h): nullptr in apply mode
    void set_decider(Decider *dc) { dc_ = dc; }
    Decider *decider() const { return dc_; }

    // the seed folder's tables (<seed>/<name>: pots.tsv, flags_ext, ...; seed.json "tables"), as the generator writes
    // them: open(name, 'w') / open(name, 'a') / os.remove
    void table_write(const std::string &name, const std::string &text) { tables_[name] = text; }
    void table_append(const std::string &name, const std::string &text) { tables_[name] += text; }
    void table_remove(const std::string &name) { tables_.erase(name); }
    const std::string *table(const std::string &name) const {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }
    const std::map<std::string, std::string> &tables() const { return tables_; }
    // what a step leaves for a later one of this build, by name (the generator's in-memory values, such as
    // mod_generator.enemy_random for enemy_random.write): not part of the seed
    std::map<std::string, std::string> &carry() { return carry_; }

    // the romfs path of a file the generator names by its file name alone (mod_generator.getRelativeDir)
    static std::string path_of(const std::string &file_name);

    bool read_bytes(const std::string &path, std::string &out) const;  // the latest written, else the game's
    // the game's own file, whatever this seed wrote (a step reading the original game, as the Python's rom path)
    bool read_game(const std::string &path, std::string &out) const { return game_(path, out); }
    void write_bytes(const std::string &path, const std::string &bytes);

    // lists a folder of the game's files: the names in it (files and folders); false: no such folder
    using GameList = std::function<bool(const std::string &dir, std::vector<std::string> &names)>;
    void set_game_list(GameList list) { list_ = std::move(list); }
    // the names in a folder (os.listdir), sorted: the game's (none without set_game_list) / the files written there
    std::vector<std::string> list_game(const std::string &dir) const;
    std::vector<std::string> list_written(const std::string &dir) const;

    // by file name, as mod_generator.readFile / writeFile
    LebRoom read_room(const std::string &name);
    void write_room(const std::string &name, LebRoom &room);
    EventFlow read_flow(const std::string &name);
    void write_flow(const std::string &name, const EventFlow &flow);
    GSheet read_sheet(const std::string &name);
    void write_sheet(const std::string &name, const GSheet &sheet);
    LvbLevel read_level(const std::string &name);
    void write_level(const std::string &name, LvbLevel &level);

    std::string next_event_name() { return "AutoEvent" + std::to_string(idgen_++); }

    // mod_generator.getItemInfo: the item at a check, its index, and (with_model) its model - a trap's from the
    // decisions (the generator drew it there, once per call)
    // (dungeon: the pool getItemInfo gets is dungeon_trap_models - small keys, instruments, the Color Dungeon's rupees)
    struct ItemInfo { std::string key; int index = -1; std::string model_path, model_name; };
    ItemInfo item_info(const std::string &check, bool with_model, bool dungeon = false);
    uint32_t idgen() const { return idgen_; }

    // the generator's state at a point of its run (the parity checks start a step there): files already written and
    // the event name counter
    void preload(const std::string &path, const std::string &bytes) { files_[path] = bytes; }
    void set_idgen(uint32_t v) { idgen_ = v; }

    struct Write { std::string path, bytes; };
    const std::vector<Write> &writes() const { return log_; }
    // false: writes() stays empty (a whole seed's build keeps only each file's last bytes: files())
    void keep_writes(bool on) { keep_log_ = on; }
    const std::map<std::string, std::string> &files() const { return files_; }

private:
    std::string need(const std::string &path) const;  // (throws when the file isn't there)

    GameRead game_;
    GameList list_;
    const Decisions &d_;
    Decider *dc_ = nullptr;
    std::map<std::string, std::string> tables_;
    std::map<std::string, std::string> carry_;
    std::map<std::string, std::string> files_;  // path -> bytes written
    std::vector<Write> log_;
    bool keep_log_ = true;
    uint32_t idgen_ = 0;
    std::map<std::string, size_t> trap_draws_;  // (check -> getItemInfo calls with a trap model so far)
};

// A step of the generator, run on the build: by its name in the write log ("mod_generator.makeGeneralLEBChanges").
struct BuildStep {
    const char *name;
    void (*run)(SeedBuild &b);
};
const std::vector<BuildStep> &build_steps();  // (the ported ones, in the generator's order)
bool build_step_known(const char *name);       // (the step has its place in that order)
bool build_step_runs(const char *name, const Decisions &d);  // (las_headless.py runs it for this seed's settings)

}  // namespace rando
