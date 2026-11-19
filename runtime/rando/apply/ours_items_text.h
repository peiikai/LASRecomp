// LAS Recomp's message helpers for the ported item steps (ours_items*.cpp): tools/rando/loc_text.py (our sentences in
// every language, fitted to the message box; the game's own names of items and places) and the message tokens of
// tools/rando/keysanity_text.py (_tokens). The tables are generated from loc_text.py
// (ours_items_text_data.cpp, tools/rando/export_ours_items_text.py --check).
//
// Texts are raw MSBT UTF-8 with the game's control tags, as the Python's bytes; lengths of our sentences count Unicode
// code points, as Python's len() of a str.
#pragma once
#include <stdint.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "seed_build.h"

namespace rando {
namespace ours {

// ---- message tokens (keysanity_text._tokens): text runs and control tags, up to the first NUL
struct Tok {
    bool tag = false;
    uint16_t group = 0, type = 0;  // (a tag's)
    std::string raw;
    bool is(uint16_t g, uint16_t t) const { return tag && group == g && type == t; }
};
std::vector<Tok> tokens(const std::string &raw);
static const uint16_t COLOR[2] = {0, 3}, PAGE_TAG[2] = {0, 4}, WAIT[2] = {1, 4};
inline bool is_wait(const Tok &t) { return t.is(WAIT[0], WAIT[1]); }
// next((r for k, g, r in reversed(_tokens(raw)) if g == WAIT), b''): the last wait tag ("" when none)
std::string last_wait(const std::string &raw);

extern const std::string RED, WHITE, PAGE;  // loc_text.RED / WHITE / PAGE (the colour and new-page tags)

// ---- UTF-8 (Python str semantics on valid UTF-8)
size_t cp_len(const std::string &s);                       // len() of the decoded str
std::vector<std::string> code_points(const std::string &s);  // each character's bytes

// ---- message files
// sorted(glob(romfs/region*/*/message/System.msbt)), as paths within the romfs ("regionUS/USen/message/System.msbt")
std::vector<std::string> system_msbts(SeedBuild &b);
bool read_msbt(SeedBuild &b, const std::string &rel, Msbt &out);  // the seed's copy, else the game's (false: none)
Msbt need_msbt(SeedBuild &b, const std::string &rel);              // (throws when there's none)
void write_msbt(SeedBuild &b, const std::string &rel, const Msbt &m);
std::string replace_all(std::string s, const std::string &from, const std::string &to);

// ---- loc_text.py
namespace text {
using StrTable = std::vector<std::pair<const char *, const char *>>;
using LangText = std::vector<std::pair<const char *, std::vector<const char *>>>;  // lang -> its text (a list: pages)
using TextTable = std::vector<std::pair<const char *, LangText>>;                  // key -> each language's
extern const StrTable FOLDERS, ITEM_LABEL, AREA_LABEL;
extern const std::vector<std::pair<const char *, int>> LINE;
extern const std::vector<const char *> CHAR_WRAP, NO_LINE_START;
extern const TextTable T, DETAIL, ITEM_OWN;
const LangText *table(const TextTable &t, const std::string &key);  // t[key] (nullptr: none)
const char *lookup(const StrTable &t, const std::string &key);      // (nullptr: none)
}  // namespace text

std::string lang_of(const std::string &rel);  // 'regionEU/EUde/message/System.msbt' -> 'de'
// tr(table, lang): table[lang], else table[lang[:2]] (an empty one counts as none); nullptr: none
const std::vector<const char *> *tr(const text::LangText *t, const std::string &lang);
std::string tr1(const text::LangText *t, const std::string &lang, bool *found = nullptr);  // (a str entry; "" if none)
// (the dict.get(lang, dflt) of a str table, no fallback)
std::string get_or(const text::LangText *t, const std::string &lang, const std::string &dflt);
std::string dungeon_label(const std::string &dn);  // 'D0' -> ClothesDungeon_map, 'D3' -> Lv3Dungeon_map

struct Seg {  // a (text, red) segment: our text (str: breaks between words) or a game name (bytes: never)
    std::string s;
    bool red = false, game = false;
};
inline Seg our(const std::string &s, bool red = false) { return {s, red, false}; }
inline Seg game(const std::string &s, bool red = false) { return {s, red, true}; }
std::string wrap(const std::vector<Seg> &segs, const std::string &wait, const std::string &lang, int line = 0);
std::string pages(const std::vector<std::vector<Seg>> &page_segs, const std::string &wait, const std::string &lang);
// fill('Du hast die {name} erhalten!', {{"name", seg}}): the template's text with the parts (throws on a missing one)
std::vector<Seg> fill(const std::string &tpl, const std::map<std::string, Seg> &parts);
std::string clean(const std::string &raw);  // loc_text._clean (no page / wait tags, up to the NUL)

class Names {  // loc_text.Names: the game's own names in the language of System.msbt at rel
public:
    Names(SeedBuild &b, const std::string &rel);
    std::string lang;
    std::map<std::string, std::string> ui, glossary, places;  // label -> raw text
    bool item(const std::string &las_item, std::string &out) const;  // (false: None)
    bool place(const std::string &label, std::string &out) const;
};
std::map<std::string, std::string> msbt_texts(const Msbt &m);  // m.texts

}  // namespace ours
}  // namespace rando
