// Seed build (seed_build.h): the generator's file handling, in memory.
#include "seed_build.h"

#include <string.h>

#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"

namespace rando {

// ---- decisions
Decisions::~Decisions() { yyjson_doc_free(doc_); }

bool Decisions::load(const std::string &json) {
    yyjson_doc_free(doc_);
    doc_ = yyjson_read(json.data(), json.size(), 0);
    root_ = doc_ ? yyjson_doc_get_root(doc_) : nullptr;
    defs_.clear();
    if (!yyjson_is_obj(root_)) return false;
    yyjson_val *defs = yyjson_obj_get(root_, "item-defs"), *k, *v;
    yyjson_obj_iter it = yyjson_obj_iter_with(defs);
    while ((k = yyjson_obj_iter_next(&it))) {
        v = yyjson_obj_iter_get_val(k);
        auto s = [&](const char *f) { const char *x = yyjson_get_str(yyjson_obj_get(v, f)); return std::string(x ? x : ""); };
        defs_[yyjson_get_str(k)] = {s("item-key"), s("model-path"), s("model-name")};
    }
    return true;
}

const std::string *Decisions::item_at(const std::string &check) const {
    static thread_local std::string tmp;
    const char *s = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(root_, "placements"), check.c_str()));
    if (!s) return nullptr;
    tmp = s;
    return &tmp;
}

int Decisions::index_at(const std::string &check) const {
    yyjson_val *v = yyjson_obj_get(yyjson_obj_get(root_, "indexes"), check.c_str());
    return yyjson_is_int(v) ? (int)yyjson_get_sint(v) : -1;
}

const Decisions::ItemDef *Decisions::item_def(const std::string &item) const {
    auto it = defs_.find(item);
    return it == defs_.end() ? nullptr : &it->second;
}

bool Decisions::setting(const std::string &key) const {
    return yyjson_is_true(yyjson_obj_get(yyjson_obj_get(root_, "settings"), key.c_str()));
}

std::string Decisions::setting_str(const std::string &key) const {
    const char *s = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(root_, "settings"), key.c_str()));
    return s ? s : "";
}

bool Decisions::trap_model(const std::string &check, size_t n, std::string &path, std::string &name) const {
    yyjson_val *pick = yyjson_arr_get(yyjson_obj_get(yyjson_obj_get(root_, "trap-models"), check.c_str()), n);
    const char *p = yyjson_get_str(yyjson_arr_get(pick, 0)), *m = yyjson_get_str(yyjson_arr_get(pick, 1));
    if (!p || !m) return false;
    path = p, name = m;
    return true;
}

yyjson_val *Decisions::raw(const std::string &field) const { return yyjson_obj_get(root_, field.c_str()); }

// ---- files
std::string SeedBuild::path_of(const std::string &f) {
    auto ends = [&](const char *e) { size_t n = strlen(e); return f.size() >= n && !f.compare(f.size() - n, n, e); };
    if (ends("leb")) return "region_common/level/" + f.substr(0, f.find('_')) + "/" + f;
    if (ends("lvb")) return "region_common/level/" + f.substr(0, f.find('.')) + "/" + f;
    if (ends("gsheet")) return "region_common/datasheets/" + f;
    if (ends("bfevfl")) return "region_common/event/" + f;
    if (ends("arc")) return "region_common/ui/" + f;
    return f;  // (a path already)
}

bool SeedBuild::read_bytes(const std::string &path, std::string &out) const {
    auto it = files_.find(path);
    if (it != files_.end()) { out = it->second; return true; }
    return game_(path, out);
}

std::string SeedBuild::need(const std::string &path) const {
    std::string b;
    if (!read_bytes(path, b)) throw std::runtime_error("seed build: no file " + path);
    return b;
}

void SeedBuild::write_bytes(const std::string &path, const std::string &bytes) {
    files_[path] = bytes;
    log_.push_back({path, bytes});
}

SeedBuild::ItemInfo SeedBuild::item_info(const std::string &check, bool with_model) {
    const std::string *item = d_.item_at(check);
    if (!item) throw std::runtime_error("seed build: nothing placed at " + check);
    const Decisions::ItemDef *def = d_.item_def(*item);
    if (!def) throw std::runtime_error("seed build: no item " + *item);
    ItemInfo r;
    r.key = def->key;
    r.index = d_.index_at(check);
    if (!with_model) return r;
    if (r.key.size() >= 4 && !r.key.compare(r.key.size() - 4, 4, "Trap")) {
        if (!d_.trap_model(check, trap_draws_[check]++, r.model_path, r.model_name))
            throw std::runtime_error("seed build: no trap model recorded for " + check);
    } else {
        r.model_path = def->model_path, r.model_name = def->model_name;
    }
    return r;
}

LebRoom SeedBuild::read_room(const std::string &n) { return LebRoom::parse(need(path_of(n))); }
void SeedBuild::write_room(const std::string &n, LebRoom &r) { write_bytes(path_of(n), r.repack()); }
EventFlow SeedBuild::read_flow(const std::string &n) { return EventFlow::parse(need(path_of(n))); }
void SeedBuild::write_flow(const std::string &n, const EventFlow &f) { write_bytes(path_of(n), f.write()); }
GSheet SeedBuild::read_sheet(const std::string &n) { return GSheet::parse(need(path_of(n))); }
void SeedBuild::write_sheet(const std::string &n, const GSheet &s) { write_bytes(path_of(n), s.write()); }
LvbLevel SeedBuild::read_level(const std::string &n) { return LvbLevel::parse(need(path_of(n))); }
void SeedBuild::write_level(const std::string &n, LvbLevel &l) { write_bytes(path_of(n), l.repack()); }

}  // namespace rando
