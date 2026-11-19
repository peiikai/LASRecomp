// Decide mode (decide.h): the random streams, the decided fields and the decisions document.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <stdexcept>

#include "decide.h"
#include "las_data.h"
#include "../../../third_party/yyjson/yyjson.h"

namespace rando {

// ---- Python's JSON and repr
std::string py_float_repr(double v) {
    if (isnan(v)) return "nan";
    if (isinf(v)) return v < 0 ? "-inf" : "inf";
    if (v == 0) return signbit(v) ? "-0.0" : "0.0";
    char buf[64];
    int p = 1;
    for (; p <= 17; p++) {  // (the shortest digits that read back as v: repr's 'r' mode)
        snprintf(buf, sizeof buf, "%.*e", p - 1, v);
        if (strtod(buf, nullptr) == v) break;
    }
    std::string s = buf, digits;
    bool neg = s[0] == '-';
    if (neg) s = s.substr(1);
    size_t e = s.find('e');
    int exp10 = atoi(s.c_str() + e + 1);
    for (size_t i = 0; i < e; i++)
        if (s[i] != '.') digits += s[i];
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    int decpt = exp10 + 1;  // (0.<digits> x 10^decpt)
    std::string out;
    if (decpt > -4 && decpt <= 16) {  // fixed
        if (decpt <= 0) out = "0." + std::string((size_t)-decpt, '0') + digits;
        else if ((size_t)decpt >= digits.size()) out = digits + std::string((size_t)decpt - digits.size(), '0') + ".0";
        else out = digits.substr(0, (size_t)decpt) + "." + digits.substr((size_t)decpt);
    } else {  // exponent, at least two digits
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        char eb[16];
        snprintf(eb, sizeof eb, "e%c%02d", decpt - 1 < 0 ? '-' : '+', abs(decpt - 1));
        out += eb;
    }
    return neg ? "-" + out : out;
}

static void json_str(std::string &o, const char *s, size_t n) {
    o += '"';
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t len = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 6 && i + 1 < n) cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F), len = 2;
        else if ((c >> 4) == 14 && i + 2 < n) cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F), len = 3;
        else if (i + 3 < n) cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F), len = 4;
        else cp = c;
        i += len;
        char b[16];
        switch (cp) {
        case '"': o += "\\\""; continue;
        case '\\': o += "\\\\"; continue;
        case '\n': o += "\\n"; continue;
        case '\r': o += "\\r"; continue;
        case '\t': o += "\\t"; continue;
        case '\b': o += "\\b"; continue;
        case '\f': o += "\\f"; continue;
        default: break;
        }
        if (cp >= 0x20 && cp < 0x7F) { o += (char)cp; continue; }
        if (cp >= 0x10000) {  // (ensure_ascii: a surrogate pair)
            uint32_t v = cp - 0x10000;
            snprintf(b, sizeof b, "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
        } else {
            snprintf(b, sizeof b, "\\u%04x", cp);
        }
        o += b;
    }
    o += '"';
}

static void json_val(std::string &o, yyjson_val *v, int indent, bool sort, int depth) {
    auto nl = [&](int d) {
        if (indent < 0) return;
        o += '\n';
        o.append((size_t)(indent * d), ' ');
    };
    const char *item_sep = indent < 0 ? ", " : ",";
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL: o += "null"; break;
    case YYJSON_TYPE_BOOL: o += yyjson_get_bool(v) ? "true" : "false"; break;
    case YYJSON_TYPE_NUM:
        if (yyjson_is_real(v)) {
            double d = yyjson_get_real(v);
            o += isnan(d) ? "NaN" : isinf(d) ? (d < 0 ? "-Infinity" : "Infinity") : py_float_repr(d);
        } else if (yyjson_is_sint(v)) {
            o += std::to_string(yyjson_get_sint(v));
        } else {
            o += std::to_string(yyjson_get_uint(v));
        }
        break;
    case YYJSON_TYPE_STR: json_str(o, yyjson_get_str(v), yyjson_get_len(v)); break;
    case YYJSON_TYPE_ARR: {
        if (!yyjson_arr_size(v)) { o += "[]"; break; }
        o += '[';
        yyjson_val *x;
        yyjson_arr_iter it = yyjson_arr_iter_with(v);
        bool first = true;
        while ((x = yyjson_arr_iter_next(&it))) {
            if (!first) o += item_sep;
            first = false;
            nl(depth + 1);
            json_val(o, x, indent, sort, depth + 1);
        }
        nl(depth);
        o += ']';
        break;
    }
    case YYJSON_TYPE_OBJ: {
        if (!yyjson_obj_size(v)) { o += "{}"; break; }
        std::vector<std::pair<std::string, yyjson_val *>> kv;
        yyjson_val *k;
        yyjson_obj_iter it = yyjson_obj_iter_with(v);
        while ((k = yyjson_obj_iter_next(&it))) kv.push_back({std::string(yyjson_get_str(k), yyjson_get_len(k)), yyjson_obj_iter_get_val(k)});
        if (sort) std::stable_sort(kv.begin(), kv.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        o += '{';
        bool first = true;
        for (auto &e : kv) {
            if (!first) o += item_sep;
            first = false;
            nl(depth + 1);
            json_str(o, e.first.data(), e.first.size());
            o += ": ";
            json_val(o, e.second, indent, sort, depth + 1);
        }
        nl(depth);
        o += '}';
        break;
    }
    default: throw std::runtime_error("py_json: unknown value");
    }
}

std::string py_json(const char *json, size_t len, int indent, bool sort_keys) {
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (!doc) throw std::runtime_error("py_json: bad JSON");
    std::string o;
    try {
        json_val(o, yyjson_doc_get_root(doc), indent, sort_keys, 0);
    } catch (...) {
        yyjson_doc_free(doc);
        throw;
    }
    yyjson_doc_free(doc);
    return o;
}

std::string py_str_repr(const std::string &s) {
    bool sq = s.find('\'') != std::string::npos, dq = s.find('"') != std::string::npos;
    char q = sq && !dq ? '"' : '\'';
    std::string o(1, q);
    for (unsigned char c : s) {
        if (c == (unsigned char)q || c == '\\') o += '\\', o += (char)c;
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20 || c == 0x7F) {
            char b[8];
            snprintf(b, sizeof b, "\\x%02x", c);
            o += b;
        } else o += (char)c;  // (UTF-8 passes through: printable non-ASCII)
    }
    return o + q;
}

// ---- the decider
Decider::Decider(Decisions &d, const std::vector<uint32_t> &rs) : d_(d) {
    if (rs.size() != 625) throw std::runtime_error("decide: the random state isn't 625 numbers");
    PyRandom::State st;
    for (int i = 0; i < 624; i++) st.mt[i] = rs[(size_t)i];
    st.mti = (int)rs[624];
    shared_.setstate(st);
}

std::string Decider::seed() const {
    const char *s = yyjson_get_str(yyjson_obj_get(d_.raw("settings"), "seed"));
    if (!s) throw std::runtime_error("decide: no seed in the settings");
    return s;
}

void Decider::reseed_shared() { shared_.seed(seed()); }

PyRandom Decider::stream(const std::string &tag) const { return PyRandom(seed() + "-" + tag); }

void Decider::set(const std::string &field, const std::string &json) { d_.set_field(field, json); }

void Decider::set_ours_world(const std::string &key, const std::string &json) {
    ours_world_[key] = json;
    std::string o = "{";
    for (const auto &kv : ours_world_) {
        if (o.size() > 1) o += ",";
        json_str(o, kv.first.data(), kv.first.size());
        o += ":" + kv.second;
    }
    d_.set_field("ours-world", o + "}");
}

const std::string *Decider::input(const std::string &name) const {
    auto it = inputs_.find(name);
    return it == inputs_.end() ? nullptr : &it->second;
}

void Decider::draw_trap(const std::string &check, bool dungeon, std::string &path, std::string &name) {
    if (!pools_) {  // ModsProcess.__init__
        using namespace las::data;
        auto &pool = pool_[0];
        for (const auto &m : ITEM_MODELS) pool.push_back({m.first, m.second});
        auto drop = [&](const std::string &k) {
            for (size_t i = 0; i < pool.size(); i++)
                if (pool[i].first == k) { pool.erase(pool.begin() + (long)i); return; }
        };
        yyjson_val *start = d_.raw("starting-items"), *v;
        std::vector<std::string> names;
        yyjson_arr_iter it = yyjson_arr_iter_with(start);
        while ((v = yyjson_arr_iter_next(&it))) names.push_back(yyjson_get_str(v) ? yyjson_get_str(v) : "");
        for (const std::string &item : names) {
            const Decisions::ItemDef *def = d_.item_def(item);
            if (!def) throw std::runtime_error("decide: no item " + item);
            std::string k = def->key == "SwordLv1" ? "SinkingSword" : def->key;
            // (as the Python: it counts the item KEY among the starting items' names - never there, so a starting
            // sword, shield or bracelet always stays a trap look)
            if (k == "SinkingSword" || k == "Shield" || k == "PowerBraceletLv1")
                if (std::count(names.begin(), names.end(), k) < 2) continue;
            drop(k);
        }
        if (!d_.setting("shuffle-instruments"))
            for (const char *inst : {"FullMoonCello", "ConchHorn", "SeaLilysBell", "SurfHarp", "WindMarimba",
                                     "CoralTriangle", "EveningCalmOrgan", "ThunderDrum"})
                drop(inst);
        pool_[1] = pool;
        for (auto kv : std::vector<std::pair<std::string, std::string>>{{"SmallKey", "ItemSmallKey.bfres"},
                                                                         {"NightmareKey", "ItemNightmareKey.bfres"},
                                                                         {"StoneBeak", "ItemStoneBeak.bfres"},
                                                                         {"Compass", "ItemCompass.bfres"},
                                                                         {"DungeonMap", "ItemDungeonMap.bfres"}}) {
            bool found = false;
            for (auto &e : pool_[1])
                if (e.first == kv.first) e.second = kv.second, found = true;
            if (!found) pool_[1].push_back(kv);
        }
        pools_ = true;
    }
    const auto &pool = pool_[dungeon ? 1 : 0];
    const auto &pick = pool[shared_.choice_index(pool.size())];
    name = pick.first, path = pick.second;
    traps_[check].push_back({path, name});
}

std::string Decider::document() const {
    std::string o = "{";
    auto field = [&](const std::string &k, const std::string &json) {
        if (o.size() > 1) o += ",";
        json_str(o, k.data(), k.size());
        o += ":" + json;
    };
    bool traps = false;
    for (const std::string &k : d_.field_names()) {
        if (k == "trap-models") continue;
        yyjson_val *v = d_.raw(k);
        size_t n;
        char *t = yyjson_val_write(v, 0, &n);
        if (!t) throw std::runtime_error("decide: can't write " + k);
        field(k, std::string(t, n));
        free(t);
    }
    std::string tm = "{";
    for (const auto &kv : traps_) {
        if (tm.size() > 1) tm += ",";
        json_str(tm, kv.first.data(), kv.first.size());
        tm += ":[";
        for (size_t i = 0; i < kv.second.size(); i++) {
            if (i) tm += ",";
            tm += "[";
            json_str(tm, kv.second[i].first.data(), kv.second[i].first.size());
            tm += ",";
            json_str(tm, kv.second[i].second.data(), kv.second[i].second.size());
            tm += "]";
        }
        tm += "]";
    }
    field("trap-models", tm + "}");
    (void)traps;
    o += "}";
    return py_json(o.data(), o.size(), 1, true);
}

}  // namespace rando
