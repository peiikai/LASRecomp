// LAS Recomp's message helpers (ours_items_text.h): tools/rando/loc_text.py and keysanity_text._tokens, ported.
#include "ours_items_text.h"

#include <string.h>

#include <stdexcept>

namespace rando {
namespace ours {

const std::string RED("\x0e\x00\x00\x03\x00\x04\x00\xd9\x45\x6d\xff", 11);
const std::string WHITE("\x0e\x00\x00\x03\x00\x04\x00\xed\xed\xed\xff", 11);
const std::string PAGE("\x0e\x00\x00\x04\x00\x00\x00", 7);

static uint16_t u16(const std::string &s, size_t i) {
    if (i + 2 > s.size()) throw std::runtime_error("message: tag runs past the end");  // (struct.error)
    return (uint16_t)((uint8_t)s[i] | (uint8_t)s[i + 1] << 8);
}

std::vector<Tok> tokens(const std::string &raw) {
    std::vector<Tok> out;
    size_t i = 0;
    while (i < raw.size() && raw[i] != 0) {
        if (raw[i] == 0x0E) {
            Tok t;
            t.tag = true;
            t.group = u16(raw, i + 1), t.type = u16(raw, i + 3);
            size_t size = u16(raw, i + 5);
            t.raw = raw.substr(i, 7 + size);  // (a slice: shorter at the end, as Python's)
            i += 7 + size;
            out.push_back(t);
        } else {
            size_t j = i;
            while (j < raw.size() && raw[j] != 0 && raw[j] != 0x0E) j++;
            Tok t;
            t.raw = raw.substr(i, j - i);
            out.push_back(t);
            i = j;
        }
    }
    return out;
}

std::string last_wait(const std::string &raw) {
    std::vector<Tok> t = tokens(raw);
    for (size_t i = t.size(); i-- > 0;)
        if (is_wait(t[i])) return t[i].raw;
    return "";
}

size_t cp_len(const std::string &s) {
    // (as len(s.decode('utf-8', 'replace')): an ill-formed sequence counts once per maximal subpart, as CPython)
    size_t n = 0;
    for (size_t i = 0; i < s.size(); n++) {
        unsigned char c = (unsigned char)s[i];
        int need = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c == 0xE0) need = 2, lo = 0xA0;
        else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) need = 2;
        else if (c == 0xED) need = 2, hi = 0x9F;
        else if (c == 0xF0) need = 3, lo = 0x90;
        else if (c >= 0xF1 && c <= 0xF3) need = 3;
        else if (c == 0xF4) need = 3, hi = 0x8F;
        size_t j = i + 1;
        for (int k = 0; k < need && j < s.size(); k++, j++) {
            unsigned char x = (unsigned char)s[j];
            if (x < (k ? 0x80 : lo) || x > (k ? 0xBF : hi)) break;
        }
        i = j;
    }
    return n;
}

std::vector<std::string> code_points(const std::string &s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        size_t j = i + 1;
        while (j < s.size() && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string replace_all(std::string s, const std::string &from, const std::string &to) {
    for (size_t i = 0; (i = s.find(from, i)) != std::string::npos; i += to.size()) s.replace(i, from.size(), to);
    return s;
}

// ---- message files
std::vector<std::string> system_msbts(SeedBuild &b) {
    // the game's message folders (region*/*), in sorted order: those with a System.msbt are the languages
    static const char *const dirs[] = {
        "regionCN/CNch", "regionCN/CNch_Validation", "regionEU/EUde", "regionEU/EUen", "regionEU/EUes",
        "regionEU/EUfr", "regionEU/EUit", "regionEU/EUnl", "regionEU/EUru", "regionEU/common", "regionJP/JPja",
        "regionKR/KRkr", "regionTW/TWch", "regionTW/TWch_Validation", "regionUS/USen", "regionUS/USes",
        "regionUS/USes_Validation", "regionUS/USfr", "regionUS/common"};
    std::vector<std::string> out;
    std::string bytes;
    for (const char *d : dirs) {
        std::string rel = std::string(d) + "/message/System.msbt";
        if (b.read_bytes(rel, bytes)) out.push_back(rel);
    }
    return out;
}

bool read_msbt(SeedBuild &b, const std::string &rel, Msbt &out) {
    std::string bytes;
    if (!b.read_bytes(rel, bytes)) return false;
    out = Msbt::parse(bytes);
    return true;
}

Msbt need_msbt(SeedBuild &b, const std::string &rel) {
    Msbt m;
    if (!read_msbt(b, rel, m)) throw std::runtime_error("no message file " + rel);
    return m;
}

void write_msbt(SeedBuild &b, const std::string &rel, const Msbt &m) { b.write_bytes(rel, m.pack()); }

std::map<std::string, std::string> msbt_texts(const Msbt &m) {
    std::map<std::string, std::string> out;
    for (const auto &l : m.labels) out[l.first] = m.strings.at(l.second);
    return out;
}

// ---- loc_text.py
namespace text {
const LangText *table(const TextTable &t, const std::string &key) {
    for (const auto &e : t)
        if (key == e.first) return &e.second;
    return nullptr;
}
const char *lookup(const StrTable &t, const std::string &key) {
    for (const auto &e : t)
        if (key == e.first) return e.second;
    return nullptr;
}
}  // namespace text

std::string lang_of(const std::string &rel) {
    size_t a = rel.find('/'), b = rel.find('/', a + 1);
    std::string folder = replace_all(rel.substr(a + 1, b - a - 1), "_Validation", "");
    const char *l = text::lookup(text::FOLDERS, folder);
    return l ? l : "en";
}

static const std::vector<const char *> *get_exact(const text::LangText *t, const std::string &lang) {
    if (!t) return nullptr;
    for (const auto &e : *t)
        if (lang == e.first) return &e.second;
    return nullptr;
}
static bool truthy(const std::vector<const char *> *v) {
    // (a str entry is one item: '' is false; a list: empty is false)
    return v && !v->empty() && !(v->size() == 1 && !*(*v)[0]);
}

const std::vector<const char *> *tr(const text::LangText *t, const std::string &lang) {
    const std::vector<const char *> *v = get_exact(t, lang);
    if (truthy(v)) return v;
    v = get_exact(t, lang.substr(0, 2));
    return truthy(v) ? v : nullptr;
}

std::string tr1(const text::LangText *t, const std::string &lang, bool *found) {
    const std::vector<const char *> *v = tr(t, lang);
    if (found) *found = v != nullptr;
    return v ? (*v)[0] : "";
}

std::string get_or(const text::LangText *t, const std::string &lang, const std::string &dflt) {
    const std::vector<const char *> *v = get_exact(t, lang);
    return v ? (*v)[0] : dflt;
}

std::string dungeon_label(const std::string &dn) {
    return dn == "D0" ? "ClothesDungeon_map" : std::string("Lv") + dn[1] + "Dungeon_map";
}

std::string clean(const std::string &raw) {
    std::string out;
    for (const Tok &t : tokens(raw))
        if (!is_wait(t) && !t.is(PAGE_TAG[0], PAGE_TAG[1])) out += t.raw;
    return out;
}

static size_t shown(const std::string &raw) {
    size_t n = 0;
    for (const Tok &t : tokens(raw))
        if (!t.tag) n += cp_len(t.raw);
    return n;
}

std::string wrap(const std::vector<Seg> &segs, const std::string &wait, const std::string &lang, int line) {
    int limit = line;
    if (!limit) {
        limit = 36;
        for (const auto &e : text::LINE)
            if (lang == e.first) limit = e.second;
    }
    bool char_wrap = false;
    for (const char *c : text::CHAR_WRAP) char_wrap |= lang == c;
    struct T { std::string b; size_t shown; bool red, space, brk; };
    std::vector<T> toks;
    bool space = false;
    for (const Seg &s : segs) {
        if (s.game) {
            toks.push_back({s.s, shown(s.s), s.red, space, space || toks.empty()});
            space = false;
            continue;
        }
        if (char_wrap) {
            for (const std::string &ch : code_points(s.s)) {
                bool brk = true;
                for (const char *c : text::NO_LINE_START) brk &= ch != c;
                toks.push_back({ch, 1, s.red, false, brk});
            }
            continue;
        }
        // re.split(r'( +)', text): words and runs of spaces
        const std::string &t = s.s;
        for (size_t i = 0; i < t.size();) {
            size_t j = i;
            if (t[i] == ' ') {
                while (j < t.size() && t[j] == ' ') j++;
                space = true;
            } else {
                while (j < t.size() && t[j] != ' ') j++;
                std::string p = t.substr(i, j - i);
                toks.push_back({p, cp_len(p), s.red, space, space || toks.empty()});
                space = false;
            }
            i = j;
        }
    }
    std::vector<std::vector<T>> lines;
    std::vector<T> cur;
    size_t width = 0;
    for (const T &t : toks) {
        size_t add = t.shown + (t.space && !cur.empty() ? 1 : 0);
        if (!cur.empty() && width + add > (size_t)limit && t.brk) {
            lines.push_back(cur);
            cur.clear(), width = 0;
            add = t.shown;
        }
        cur.push_back(t);
        width += add;
    }
    if (!cur.empty()) lines.push_back(cur);
    std::string out;
    bool red = false;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += i % 2 == 0 ? wait + PAGE : std::string("\n");
        for (size_t j = 0; j < lines[i].size(); j++) {
            const T &t = lines[i][j];
            if (j && t.space) out += ' ';
            if (t.red != red) {
                out += t.red ? RED : WHITE;
                red = t.red;
            }
            out += t.b;
        }
    }
    if (red) out += WHITE;
    return out;
}

std::string pages(const std::vector<std::vector<Seg>> &page_segs, const std::string &wait, const std::string &lang) {
    std::string out;
    for (size_t i = 0; i < page_segs.size(); i++) {
        if (i) out += wait + PAGE;
        out += wrap(page_segs[i], wait, lang);
    }
    return out + wait + std::string(1, '\0');
}

static bool word_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

std::vector<Seg> fill(const std::string &tpl, const std::map<std::string, Seg> &parts) {
    std::vector<Seg> out;
    std::string piece;
    for (size_t i = 0; i < tpl.size();) {
        if (tpl[i] == '{') {
            size_t j = i + 1;
            while (j < tpl.size() && word_char(tpl[j])) j++;
            if (j < tpl.size() && tpl[j] == '}' && j > i + 1) {
                if (!piece.empty()) out.push_back(our(piece)), piece.clear();
                auto it = parts.find(tpl.substr(i + 1, j - i - 1));
                if (it == parts.end()) throw std::runtime_error("loc_text.fill: no part " + tpl.substr(i, j - i + 1));
                out.push_back(it->second);
                i = j + 1;
                continue;
            }
        }
        piece += tpl[i++];
    }
    if (!piece.empty()) out.push_back(our(piece));
    return out;
}

Names::Names(SeedBuild &b, const std::string &rel) {
    auto load = [&](const char *name) {
        Msbt m;
        return read_msbt(b, replace_all(rel, "System.msbt", name), m) ? msbt_texts(m) : std::map<std::string, std::string>();
    };
    lang = lang_of(rel);
    ui = load("UI.msbt"), glossary = load("Glossary.msbt"), places = load("Place.msbt");
}

bool Names::item(const std::string &las_item, std::string &out) const {
    const char *label = text::lookup(text::ITEM_LABEL, las_item);
    if (!label) return false;
    std::string l = label;
    const std::map<std::string, std::string> &m = l.compare(0, 2, "g:") ? ui : glossary;
    auto it = m.find(l.compare(0, 2, "g:") ? "ItemName_" + l : "glossary_" + l.substr(2));
    out = it == m.end() ? "" : clean(it->second);
    return !out.empty();
}

bool Names::place(const std::string &label, std::string &out) const {
    auto it = places.find(label);
    out = it == places.end() ? "" : clean(it->second);
    return !out.empty();
}

}  // namespace ours
}  // namespace rando
