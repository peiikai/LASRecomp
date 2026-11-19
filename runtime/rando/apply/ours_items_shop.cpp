// LAS Recomp's item steps: shopsanity, trendsanity, pets, the Escape Bottle (tools/rando/shopsanity.py, trendsanity.py,
// pets.py, escape_bottle.py), ported (ours_items.cpp).
//
// Decisions read (tools/rando/record_ours_items.py): shop-prices (the six goods' prices, shopsanity's draws).
// The pets come from the settings as pets.enabled reads them; the Glossary / System texts from the seed's files.
#include <string.h>

#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "item_get.h"
#include "las_data.h"
#include "ours_items_lib.h"

namespace rando {
namespace ours {
namespace {

using namespace las;

// the region_common file the seed wrote, else the game's (the modules' path())
const std::string EV = "region_common/event/", DS = "region_common/datasheets/";

bool written(SeedBuild &b, const std::string &path) { return b.files().count(path) != 0; }

// a gsheet struct copy with fields replaced (dict(base, k=v, ...))
GStruct with(GStruct s, const std::vector<std::pair<std::string, GValue>> &kv) {
    for (const auto &e : kv) {
        GValue *v = gfield(s, e.first);
        if (v) *v = e.second;
        else s.push_back(e);
    }
    return s;
}

const GStruct &row(const GSheet &sh, const std::string &symbol) {
    for (const GStruct &v : sh.values)
        if (at(v, "symbol").s == symbol) return v;
    throw std::runtime_error("no row " + symbol + " in " + sh.name);  // (next(): StopIteration)
}

// ======== shopsanity.py
struct ShopCheck { const char *loc; int slot, price; };
const ShopCheck SHOP[] = {{"shop-slot1", 0, 20}, {"shop-slot2", 1, 10}, {"shop-slot3-1st", 2, 200},
                          {"shop-slot4", 3, 10}, {"shop-slot5-1st", 4, 999}, {"shop-slot6", 5, 200}};
const int ITEM_ID_BASE = 230;

const data::StrTable UI_NAME = {
    {"SwordLv1", "Sword1"}, {"SwordLv2", "Sword2"}, {"PowerBraceletLv1", "Bracelet1"}, {"PowerBraceletLv2", "Bracelet2"},
    {"RocsFeather", "Feather"}, {"FullMoonCello", "Violin"}, {"ConchHorn", "Horn"}, {"SeaLilysBell", "Bell"},
    {"SurfHarp", "Harp"}, {"WindMarimba", "Marimba"}, {"CoralTriangle", "Triangle"}, {"EveningCalmOrgan", "Organ"},
    {"ThunderDrum", "Drum"}, {"SecretSeashell", "SecretShells"}, {"Seashell", "SecretShells"}, {"MagicPowder", "Powder"},
    {"Song_Mambo", "Mambo"}, {"Song_Soul", "Soul"}, {"Song_WindFish", "WindFish"}, {"ClothesRed", "WearRed"},
    {"ClothesBlue", "WearBlue"}, {"ClothesGreen", "WearGreen"}, {"Necklace", "PinkBra"}};
const data::StrTable EN_NAME = {
    {"HeartPiece", "Piece of Heart"}, {"HeartContainer", "Heart Container"}, {"SmallKey", "Small Key"},
    {"NightmareKey", "Nightmare Key"}, {"Compass", "Compass"}, {"DungeonMap", "Dungeon Map"}, {"StoneBeak", "Stone Beak"},
    {"GoldenLeaf", "Golden Leaf"}, {"Rupee5", "5 Rupees"}, {"Rupee20", "20 Rupees"}, {"Rupee50", "50 Rupees"},
    {"Rupee100", "100 Rupees"}, {"Rupee300", "300 Rupees"}, {"MagicPowder_MaxUp", "Magic Powder Upgrade"},
    {"Bomb_MaxUp", "Bomb Upgrade"}, {"Arrow_MaxUp", "Arrow Upgrade"}, {"PanelDungeonPiece", "Chamber Stone"},
    {"Bottle", "Bottle"}, {"Heart", "Heart"}, {"Arrow", "Arrows"}};

bool py_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0b' || c == '\x0c'; }
std::string strip(const std::string &s) {  // bytes.strip()
    size_t a = 0, z = s.size();
    while (a < z && py_space(s[a])) a++;
    while (z > a && py_space(s[z - 1])) z--;
    return s.substr(a, z - a);
}

// shopsanity.item_name: the localized name of an item key (false: the language has none)
bool shop_item_name(const std::string &key, const Msbt &system, const Msbt &ui, std::string &out) {
    const char *u = data::lookup(UI_NAME, key);
    const std::string *raw = ui.text("ItemName_" + std::string(u ? u : key));
    if (raw && !raw->empty()) {  // (raw.rstrip(b'\0'))
        out = *raw;
        while (!out.empty() && out.back() == '\0') out.pop_back();
        return true;
    }
    raw = system.text("Get" + key);
    if (raw && !raw->empty()) {  // re.search(rb'<colour tag>....(.*?)\x0e', raw, re.S)
        for (size_t i = raw->find(RED.substr(0, 7)); i != std::string::npos; i = raw->find(RED.substr(0, 7), i + 1)) {
            if (i + 11 > raw->size()) break;
            size_t e = raw->find('\x0e', i + 11);
            if (e == std::string::npos) continue;
            std::string g = strip(raw->substr(i + 11, e - i - 11));
            if (g.empty()) return false;
            out = g;
            return true;
        }
    }
    return false;
}

// _set_price: the number right after the first price colour tag, written like it (ASCII or full-width digits)
std::string set_price(const std::string &raw, int price) {
    size_t i = raw.find(RED);
    if (i == std::string::npos) throw std::runtime_error("shopsanity: no price tag");  // (ValueError)
    i += RED.size();
    size_t j = i;
    for (;;) {
        unsigned char c = j < raw.size() ? (unsigned char)raw[j] : 0;
        if (j < raw.size() && ((c >= '0' && c <= '9') || c == ',')) { j++; continue; }
        if (j + 2 < raw.size() && c == 0xEF && (unsigned char)raw[j + 1] == 0xBC &&
            (((unsigned char)raw[j + 2] >= 0x90 && (unsigned char)raw[j + 2] <= 0x99) || (unsigned char)raw[j + 2] == 0x8C)) {
            j += 3;
            continue;
        }
        break;
    }
    if (j == i) throw std::runtime_error("shopsanity: no price after the tag");  // (re.match: None)
    std::string s = std::to_string(price), digits;
    if (raw[i] >= '0' && raw[i] <= '9') digits = s;
    else
        for (char ch : s) digits += std::string("\xef\xbc") + (char)(0x90 + (ch - '0'));  // (U+FF10 + d)
    return raw.substr(0, i) + digits + raw.substr(j);
}

struct Shop { int n; std::string loc; int slot, price; SeedBuild::ItemInfo it; int item_id; };

void shop_texts(SeedBuild &b, const std::vector<Shop> &checks) {
    static const char *const CJK[] = {"JP", "CN", "TW", "KR"};
    for (const std::string &sys_rel : system_msbts(b)) {  // (_lang_dirs: System.msbt and UI.msbt in the game)
        std::string ui_rel = replace_all(sys_rel, "System.msbt", "UI.msbt"), bytes;
        if (!b.read_game(ui_rel, bytes)) continue;
        Msbt ui = Msbt::parse(bytes);
        std::string region = sys_rel.substr(6, 2);
        bool cjk = false;
        for (const char *c : CJK) cjk |= region == c;
        Msbt system = need_msbt(b, sys_rel);
        const std::string *tag = system.text("PriceTagScoop"), *buy = system.text("BuyHeartPeace");
        if (!tag || !buy) throw std::runtime_error("shopsanity: no price texts in " + sys_rel);
        std::string tag_tail = tag->substr(tag->find(RED)), buy_tail = buy->substr(buy->find(RED));
        for (const Shop &c : checks) {
            std::string name;
            if (!shop_item_name(c.it.key, system, ui, name)) {
                const char *en = data::lookup(EN_NAME, c.it.key);
                name = en ? en : c.it.key;
            }
            system.set("LARPriceTag" + std::to_string(c.n), set_price(name + "\n" + tag_tail, c.price));
            system.set("LARBuy" + std::to_string(c.n), set_price(name + (cjk ? " " : ": ") + buy_tail, c.price));
        }
        write_msbt(b, sys_rel, system);
    }
}

}  // namespace

void shopsanity_apply(SeedBuild &b) {
    const Decisions &d = b.decisions();
    std::string mode = d.setting_str("shopsanity");
    if (mode != "vanilla" && mode != "random") return;
    yyjson_val *prices = d.raw("shop-prices");
    std::vector<Shop> checks;
    for (int n = 0; n < 6; n++) {
        int price = SHOP[n].price;
        if (mode == "random") {
            yyjson_val *p = yyjson_arr_get(prices, (size_t)n);
            if (!yyjson_is_int(p)) throw std::runtime_error("shopsanity: no recorded price " + std::to_string(n));
            price = (int)yyjson_get_sint(p);
        }
        checks.push_back({n, SHOP[n].loc, SHOP[n].slot, price, b.item_info(SHOP[n].loc, true), ITEM_ID_BASE + n});
    }
    auto N = [](const char *p, int n) { return p + std::to_string(n); };

    GSheet flags = b.read_sheet("GlobalFlags.gsheet");
    std::vector<std::string> names;
    for (const Shop &c : checks) names.push_back(N("LARShop", c.n));
    for (const Shop &c : checks) names.push_back(N("LARShopSteal", c.n));
    alloc_flags(flags, names);
    b.write_sheet("GlobalFlags.gsheet", flags);

    GSheet items = b.read_sheet("Items.gsheet");
    GStruct bow = row(items, "Bow");
    for (const Shop &c : checks)
        items.values.push_back(with(bow, {{"symbol", gs(N("LARShop", c.n))}, {"itemID", gi(c.item_id)}, {"gettingFlag", gs("")}}));
    b.write_sheet("Items.gsheet", items);

    GSheet conds = b.read_sheet("Conditions.gsheet");
    for (const Shop &c : checks) conds.values.push_back(create_condition(N("LARShopCond", c.n), {{1, N("!LARShop", c.n)}}));
    b.write_sheet("Conditions.gsheet", conds);

    GSheet shop = b.read_sheet("ShopItem.gsheet");
    for (GStruct &s : shop.values) {
        std::vector<GStruct> goods;
        for (const Shop &c : checks)
            if (c.slot == at(s, "mIndex").i)
                goods.push_back({{"mModelPath", gs("actor/" + c.it.model_path)}, {"mModelName", gs(c.it.model_name)},
                                 {"mCount", gi(1)}, {"mLabel", gs(N("System:LARPriceTag", c.n))},
                                 {"mCondition", gs(N("LARShopCond", c.n))}, {"mEntryPoint", gs(N("LARShop", c.n))},
                                 {"mItem", gs(N("LARShop", c.n))}, {"mIndex", gi(-1)}});
        at(s, "mGoods") = gsa(goods);
    }
    b.write_sheet("ShopItem.gsheet", shop);

    {  // buying: an entry point per good
        EventFlow flow = b.read_flow("ToolShopkeeper.bfevfl");
        Flow f(b, flow.flowchart);
        EvflContainer talk = f.event("Event138").params;
        for (const Shop &c : checks) {
            std::string ep = N("LARShop", c.n);
            std::string done = f.action("EventFlags", "SetFlag", {{"symbol", ep}, {"value", true}});
            std::string give = insert_item_get(f, c.it.key, c.it.index, "", done);
            std::string bought = f.action_chain("", {{"Link", "AddRupee", {{"amount", -c.price}}},
                                                     {"ToolShopkeeper", "RemoveGoods", {}},
                                                     {"ToolShopkeeper", "Talk", {{"message", "Npc:BuyToolShop"}}}},
                                                give);
            std::string restore = f.action("ToolShopkeeper", "RestoreGoods", {});
            std::string poor = f.action("ToolShopkeeper", "Talk", {{"message", "Npc:NoneMoneyToolShop"}}, restore);
            std::string money = f.sw("Link", "CheckRupee", {{"amount", c.price}}, {{0, poor}, {1, bought}});
            std::string restore2 = f.action("ToolShopkeeper", "RestoreGoods", {});
            std::string answer = f.sw("Dialog", "GetLastResult", {}, {{0, money}, {1, restore2}});
            std::string offer = f.action("ToolShopkeeper", "Talk", {{"message", N("System:LARBuy", c.n)}});
            std::string turn = f.action("ToolShopkeeper", "GenericTalkSequence", {});
            f.event(turn).params = talk;
            std::string fork = f.fork("", {turn, offer}, answer).first;
            f.add_entry_point(ep);
            std::string show = f.action("Hud", "ShowRupee", {{"visible", true}}, fork);
            f.insert_after(ep, show);
        }
        b.write_flow("ToolShopkeeper.bfevfl", flow);
    }
    {  // stealing: after ExitOutShop's "stolen" scene, give what was stolen
        EventFlow flow = b.read_flow("PlayerStart.bfevfl");
        Flow f(b, flow.flowchart);
        EvflEvent &last = f.event("Event774");
        std::string nxt = last.next != EVFL_NONE ? flow.flowchart.events[last.next].name : "";
        for (size_t i = checks.size(); i-- > 0;) {
            const Shop &c = checks[i];
            std::string clear = f.action("EventFlags", "SetFlag", {{"symbol", N("LARShopSteal", c.n)}, {"value", false}}, nxt);
            std::string give = insert_item_get(f, c.it.key, c.it.index, "", clear);
            nxt = f.sw("EventFlags", "CheckFlag", {{"symbol", N("LARShopSteal", c.n)}}, {{0, nxt}, {1, give}});
        }
        f.insert_after("Event774", nxt);
        b.write_flow("PlayerStart.bfevfl", flow);
    }
    shop_texts(b, checks);
}

// ======== trendsanity.py
namespace {
std::string rename_model(const std::string &data, const std::string &old, const std::string &nu) {
    if (old.size() != nu.size()) throw std::runtime_error("trendsanity: model names of different lengths");
    std::string pat;
    pat += (char)(old.size() & 0xFF), pat += (char)(old.size() >> 8 & 0xFF);
    pat += old;
    pat += '\0';
    size_t i = data.find(pat), count = 0;
    for (size_t j = i; j != std::string::npos; j = data.find(pat, j + pat.size())) count++;
    if (count != 1) throw std::runtime_error("trendsanity: model name " + old + " found " + std::to_string(count) + " times");
    return data.substr(0, i + 2) + nu + data.substr(i + 2 + old.size());
}

std::string symbol_for(const std::string &model_name, int n) {
    static const char *const D = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string code;
    int v = n;
    for (int k = 0; k + 2 < (int)model_name.size(); k++) {
        code = D[v % 36] + code;
        v /= 36;
    }
    if (v != 0 || model_name.size() < 3) throw std::runtime_error("trendsanity: no symbol for " + model_name);
    return "L" + code + model_name.back();
}
}  // namespace

void trendsanity_apply(SeedBuild &b) {
    if (!setting_or(b.decisions(), "trendsanity", false)) return;
    struct Check { const char *loc; int type, index; };
    static const Check CHECKS[] = {{"trendy-prize-1", 1, 0}, {"trendy-prize-2", 1, 1}, {"trendy-prize-3", 1, 2},
                                   {"trendy-prize-4", 2, 0}, {"trendy-prize-5", 2, 1}};
    const int N = 5, FIRST_ROW = 21;
    auto nm = [](const char *p, int n) { return p + std::to_string(n); };

    GSheet flags = b.read_sheet("GlobalFlags.gsheet");
    std::vector<std::string> names;
    for (int n = 0; n < N; n++) names.push_back(nm("LARPrize", n));
    for (int n = 0; n < N; n++) names.push_back(nm("LARPrizeGive", n));
    names.push_back("LARPrizeOff");
    alloc_flags(flags, names);
    b.write_sheet("GlobalFlags.gsheet", flags);

    GSheet sheet = b.read_sheet("CranePrize.gsheet");
    if (sheet.values.size() != (size_t)FIRST_ROW) throw std::runtime_error("trendsanity: CranePrize rows");
    const GStruct doll = sheet.values.at(7);  // (parsed before the edits below)
    auto won = [&](int type, int index) -> std::string {
        for (int n = 0; n < N; n++)
            if (CHECKS[n].type == type && CHECKS[n].index == index) return nm("LARPrize", n);
        return "";
    };
    for (GStruct &v : sheet.values) {
        bool featured = at(at(v, "options").structs.at(0), "isFeatured").b || at(v, "symbol").s == "YoshiDoll";
        for (GStruct &lay : at(v, "layouts").structs) {
            const GStruct &pl = at(lay, "place").structs.at(0);
            std::string w = won(at(pl, "type").i, at(pl, "index").i);
            GValue &conds = at(lay, "conditions");
            for (GStruct &c : conds.structs)
                if (at(c, "parameter").s == "TradeYoshiDollGet") at(c, "parameter") = gs(won(1, 0));
            if (featured) {
                conds.structs.push_back(cond(1, "LARPrizeOff"));
            } else if (!w.empty()) {
                bool has = false;
                for (const GStruct &c : conds.structs) has |= at(c, "parameter").s == w;
                if (!has) conds.structs.push_back(cond(1, w));
            }
        }
    }
    struct Got { int n; SeedBuild::ItemInfo it; };
    std::vector<Got> checks;
    for (int n = 0; n < N; n++) {
        SeedBuild::ItemInfo it = b.item_info(CHECKS[n].loc, true);
        std::string sym = symbol_for(it.model_name, n), src;
        if (!b.read_bytes("region_common/actor/" + it.model_path, src))
            throw std::runtime_error("trendsanity: no model " + it.model_path);
        b.write_bytes("region_common/actor/Prize" + sym + ".bfres", rename_model(src, it.model_name, sym));
        double scale = 1.0;
        data::lookup(data::MODEL_SIZES, it.model_name, scale);
        GStruct nu = with(doll, {{"symbol", gs(sym)}, {"getCount", gi(1)}, {"modelScale", gf((float)scale)},
                                 {"layouts", gsa({{{"itemIndex", gi(-1)}, {"conditions", gsa({cond(1, nm("!LARPrize", n))})},
                                                   {"place", gst({{"type", gi(CHECKS[n].type)}, {"index", gi(CHECKS[n].index)}})}}})}});
        at(at(nu, "options").structs.at(0), "isFeatured") = gb(false);
        at(at(nu, "collision").structs.at(0), "traits") = gs("LARUpright");
        sheet.values.push_back(nu);
        checks.push_back({n, it});
    }
    b.write_sheet("CranePrize.gsheet", sheet);

    GSheet traits = b.read_sheet("CranePrizePhysicsTraits.gsheet");
    traits.values.push_back(with(row(traits, "Sample"),
                                 {{"symbol", gs("LARUpright")}, {"bounciness", gf(0.0f)}, {"angularDamp", gf(1.0f)},
                                  {"axisLock", gst({{"rotationX", gb(true)}, {"rotationY", gb(true)}, {"rotationZ", gb(true)}})}}));
    b.write_sheet("CranePrizePhysicsTraits.gsheet", traits);

    EventFlow flow = b.read_flow("PrizeCommon.bfevfl");
    Flow f(b, flow.flowchart);
    EvflEvent &seq = f.event("Event0");
    if (seq.next == EVFL_NONE || flow.flowchart.events[seq.next].name != "Event1")
        throw std::runtime_error("trendsanity: PrizeCommon changed");
    f.insert_after("Event0", "Event5");
    std::string nxt = "Event0";
    for (size_t i = checks.size(); i-- > 0;) {
        const Got &c = checks[i];
        std::string clear = f.action("EventFlags", "SetFlag", {{"symbol", nm("LARPrizeGive", c.n)}, {"value", false}}, "Event5");
        std::string give = insert_item_get(f, c.it.key, c.it.index, "", clear);
        nxt = f.sw("EventFlags", "CheckFlag", {{"symbol", nm("LARPrizeGive", c.n)}}, {{0, nxt}, {1, give}});
    }
    f.insert_after("Event1", nxt);
    f.insert_after("Event2", "Event1");
    b.write_flow("PrizeCommon.bfevfl", flow);
}

// ======== pets.py
namespace {
struct Pet { const char *name, *sym, *npc, *label, *en_article; };
const Pet PETS[] = {{"rooster", "PetRooster", "NpcFlyingCucco", "glossary_sequence16", "The "},
                    {"marin-companion", "PetMarin", "NpcMarin", "glossary_npc01", ""},
                    {"ghost", "PetGhost", "NpcGhost", "glossary_npc41", "The "}};
const int CARRIER_ID = 79;

// (folder code -> (before, after) male, female)
struct Tpl { const char *code, *pre_m, *post_m, *pre_f, *post_f; };
const Tpl TEMPLATES[] = {
    {"de", "Neuer Gef\xc3\xa4hrte: ", "!", "Neue Gef\xc3\xa4hrtin: ", "!"},
    {"fr", "Nouveau compagnon : ", " !", "Nouvelle compagne : ", " !"},
    {"es", "\xc2\xa1Nuevo compa\xc3\xb1" "ero: ", "!", "\xc2\xa1Nueva compa\xc3\xb1" "era: ", "!"},
    {"it", "Nuovo compagno: ", "!", "Nuova compagna: ", "!"},
    {"nl", "Nieuwe metgezel: ", "!", "Nieuwe metgezel: ", "!"},
    {"ru", "\xd0\x9d\xd0\xbe\xd0\xb2\xd1\x8b\xd0\xb9 \xd1\x81\xd0\xbf\xd1\x83\xd1\x82\xd0\xbd\xd0\xb8\xd0\xba: ", "!",
     "\xd0\x9d\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x8f \xd1\x81\xd0\xbf\xd1\x83\xd1\x82\xd0\xbd\xd0\xb8\xd1\x86\xd0\xb0: ", "!"},
    {"ja", "", "\xe3\x81\x8c \xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xbe\xe3\x81\xab \xe3\x81\xaa\xe3\x81\xa3\xe3\x81\x9f\xef\xbc\x81",
     "", "\xe3\x81\x8c \xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xbe\xe3\x81\xab \xe3\x81\xaa\xe3\x81\xa3\xe3\x81\x9f\xef\xbc\x81"},
    {"kr", "", "{i} \xeb\x8f\x99\xeb\xa3\x8c\xea\xb0\x80 \xeb\x90\x98\xec\x97\x88\xeb\x8b\xa4!", "",
     "{i} \xeb\x8f\x99\xeb\xa3\x8c\xea\xb0\x80 \xeb\x90\x98\xec\x97\x88\xeb\x8b\xa4!"},
};
const char *const CH_CN = "\xe6\x88\x90\xe4\xb8\xba\xe4\xba\x86\xe4\xbc\x99\xe4\xbc\xb4\xef\xbc\x81";  // 成为了伙伴！
const char *const CH_TW = "\xe6\x88\x90\xe7\x82\xba\xe4\xba\x86\xe5\xa4\xa5\xe4\xbc\xb4\xef\xbc\x81";  // 成為了夥伴！

uint32_t decode1(const std::string &ch) {  // (one well-formed UTF-8 character)
    unsigned char c = (unsigned char)ch[0];
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t v = c & (0x3F >> n);
    for (int i = 1; i <= n && i < (int)ch.size(); i++) v = v << 6 | ((unsigned char)ch[i] & 0x3F);
    return v;
}
std::string encode1(uint32_t v) {
    std::string s;
    if (v < 0x80) s += (char)v;
    else if (v < 0x800) s += (char)(0xC0 | v >> 6), s += (char)(0x80 | (v & 0x3F));
    else if (v < 0x10000) s += (char)(0xE0 | v >> 12), s += (char)(0x80 | (v >> 6 & 0x3F)), s += (char)(0x80 | (v & 0x3F));
    else
        s += (char)(0xF0 | v >> 18), s += (char)(0x80 | (v >> 12 & 0x3F)), s += (char)(0x80 | (v >> 6 & 0x3F)),
            s += (char)(0x80 | (v & 0x3F));
    return s;
}
// str.islower() / upper() of one character: Latin and Cyrillic (the glossary names the pets' texts use)
uint32_t upper_of(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

std::string pet_compose(const Pet &pet, const std::string &lang, const std::string &name_raw, const std::string &get_raw) {
    std::vector<Tok> t = tokens(get_raw);
    std::vector<std::string> colors;
    for (const Tok &k : t)
        if (k.is(COLOR[0], COLOR[1])) colors.push_back(k.raw);
    std::string on, off;
    if (colors.size() >= 2) on = colors[0], off = colors[1];
    std::string wait = last_wait(get_raw), name;
    for (const Tok &k : tokens(name_raw)) name += k.raw;
    std::string code = lang.substr(2, 2), pre, post;
    bool female = !strcmp(pet.name, "marin-companion");
    if (code == "ch") {
        if (!lang.compare(0, 2, "CN")) post = CH_CN;
        else if (!lang.compare(0, 2, "TW")) post = CH_TW;
        else throw std::runtime_error("pets: no Chinese text for " + lang);  // (CH[lang[:2]]: KeyError)
    } else {
        const Tpl *tp = nullptr;
        for (const Tpl &x : TEMPLATES)
            if (code == x.code) tp = &x;
        if (!tp) return pet.en_article + on + name + off + " joins you!" + wait + std::string(1, '\0');
        pre = female ? tp->pre_f : tp->pre_m, post = female ? tp->post_f : tp->post_m;
        if (!lang.compare(0, 2, "US")) {  // (post.lstrip())
            size_t k = post.find_first_not_of(" \t\n\r\x0b\x0c");
            post.erase(0, k == std::string::npos ? post.size() : k);
        }
    }
    std::vector<std::string> cps = code_points(name);
    if (!cps.empty()) {
        uint32_t c = decode1(cps[0]), u = upper_of(c);
        if (u != c) name = encode1(u) + name.substr(cps[0].size());
    }
    if (post.find("{i}") != std::string::npos) {  // 이 after a final consonant, else 가
        long last = cps.empty() ? -1 : (long)decode1(cps.back()) - 0xAC00;
        post = replace_all(post, "{i}", 0 <= last && last < 11172 && last % 28 ? "\xec\x9d\xb4" : "\xea\xb0\x80");
    }
    return pre + on + name + off + post + wait + std::string(1, '\0');
}
}  // namespace

void pets_apply(SeedBuild &b) {
    const Decisions &d = b.decisions();
    std::vector<int> pets;  // (indexes into PETS = ORDER)
    if (setting_or(d, "pet-rooster", false)) pets.push_back(0);
    if (!setting_or(d, "open-desert", true)) pets.push_back(1);
    if (setting_or(d, "pet-ghost", false)) pets.push_back(2);
    if (pets.empty()) return;

    GSheet items = b.read_sheet("Items.gsheet");
    GStruct doll = row(items, "YoshiDoll");
    for (int p : pets) {
        doll = with(doll, {{"symbol", gs(PETS[p].sym)}, {"itemID", gi(CARRIER_ID + p)}, {"npcKey", gs(PETS[p].npc)},
                           {"gettingFlag", gs("")}});
        items.values.push_back(doll);
    }
    b.write_sheet("Items.gsheet", items);
    bool ghost = false, marin = false;
    for (int p : pets) ghost |= p == 2, marin |= p == 1;
    if (ghost) {
        GSheet cs = b.read_sheet("Conditions.gsheet");
        GStruct *g = nullptr;
        for (GStruct &v : cs.values)
            if (!g && at(v, "symbol").s == "Graveyard") g = &v;
        if (!g) throw std::runtime_error("pets: no Graveyard condition");
        GStruct &c0 = item(at(*g, "conditions"), 0);
        if (at(c0, "category").i != 3 || at(c0, "parameter").s != "Ghost") throw std::runtime_error("pets: Graveyard changed");
        c0 = cond(1, "unused0429");
        b.write_sheet("Conditions.gsheet", cs);
        EventFlow flow = b.read_flow("Field.bfevfl");
        Flow f(b, flow.flowchart);
        const EvflValue *sym = f.event("Event6").params.get("symbol");
        if (!sym || sym->s != "Ghost4_Clear") throw std::runtime_error("pets: Field.bfevfl changed");
        f.insert_after("Graveyard_talk", "Event6");
        f.remove_after("Event6");
        b.write_flow("Field.bfevfl", flow);
    }
    if (marin) {
        EventFlow flow = b.read_flow("Walrus.bfevfl");
        EvflFlowchart &fc = flow.flowchart;
        Flow f(b, fc);
        const EvflValue *sym = f.event("Event12").params.get("symbol");
        if (!sym || sym->s != "WalrusTalked") throw std::runtime_error("pets: Walrus.bfevfl changed");
        f.insert_after("Event123", "Event58");
        EvflEvent &e36 = f.event("Event36");
        if (fc.actors[e36.actor].actions[e36.action] != "LeaveCompanion" || e36.next != event_index(fc, "Event93"))
            throw std::runtime_error("pets: Walrus Event36 changed");
        f.insert_after("Event71", "Event93");
        const EvflValue *msg = f.event("Event8").params.get("message");
        if (!msg || msg->s != "Scenario:Lv4ClearQuestWalrus2") throw std::runtime_error("pets: Walrus Event8 changed");
        f.insert_after("Event85", "Event63");
        EvflEvent &e18 = f.event("Event18");
        uint16_t e17 = event_index(fc, "Event17");
        size_t i = 0;
        while (i < e18.forks.size() && e18.forks[i] != e17) i++;
        if (i == e18.forks.size()) throw std::runtime_error("pets: Walrus Event18 changed");  // (StopIteration)
        e18.forks[i] = event_index(fc, "Event14");
        if (f.event("Event14").next != event_index(fc, "Event20")) throw std::runtime_error("pets: Walrus Event14 changed");
        f.remove_after("Event14");
        b.write_flow("Walrus.bfevfl", flow);
    }
    {
        EventFlow flow = b.read_flow("Item.bfevfl");
        Flow f(b, flow.flowchart);
        for (int p : pets)
            if (!f.find_entry(PETS[p].sym)) {
                f.add_entry_point(PETS[p].sym);
                f.action_chain(PETS[p].sym, {{"Dialog", "Show", {{"message", std::string("System:") + PETS[p].sym}}}});
            }
        b.write_flow("Item.bfevfl", flow);
    }
    // write_messages: "<pet> joins you!" in every language
    for (const std::string &rel : system_msbts(b)) {
        std::string lang = rel.substr(rel.find('/') + 1);
        lang = lang.substr(0, lang.find('/'));
        Msbt sysm = need_msbt(b, rel);
        Msbt gl = need_msbt(b, replace_all(rel, "System.msbt", "Glossary.msbt"));
        for (int p : pets) {
            const std::string *name = gl.text(PETS[p].label);
            if (!name) continue;
            const std::string *get = sysm.text("GetSmallKey");
            if (!get) throw std::runtime_error("pets: no GetSmallKey in " + rel);
            sysm.set(PETS[p].sym, pet_compose(PETS[p], lang, *name, *get));
        }
        write_msbt(b, rel, sysm);
    }
}

// ======== escape_bottle.py
void escape_bottle_apply(SeedBuild &b) {
    const std::string ENTRY = "EscapeBottle", tp = EV + "Tarin.bfevfl", ip = EV + "Item.bfevfl";
    if (!written(b, tp)) return;
    {
        EventFlow flow = b.read_flow("Tarin.bfevfl");
        Flow f(b, flow.flowchart);
        EvflEvent *e36 = f.find("Event36");
        if (!e36 || e36->next == EVFL_NONE) return;
        std::string nxt = flow.flowchart.events[e36->next].name;
        f.action_chain("Event36", {{"Link", "GenericItemGetSequenceByKey", {{"itemKey", "Bottle"}, {"keepCarry", false}, {"messageEntry", ENTRY}}},
                                   {"Inventory", "AddItemByKey", {{"itemKey", "Bottle"}, {"count", 1}, {"index", 2}, {"autoEquip", false}}}},
                       nxt);
        b.write_flow("Tarin.bfevfl", flow);
    }
    if (!written(b, ip)) {  // (copied from the game first)
        std::string bytes;
        if (!b.read_game(ip, bytes)) throw std::runtime_error("no " + ip);
        b.write_bytes(ip, bytes);
    }
    {
        EventFlow flow = b.read_flow("Item.bfevfl");
        Flow f(b, flow.flowchart);
        if (!f.find_entry(ENTRY)) {
            f.add_entry_point(ENTRY);
            f.action_chain(ENTRY, {{"Dialog", "Show", {{"message", "System:" + ENTRY}}}});
        }
        b.write_flow("Item.bfevfl", flow);
    }
    for (const std::string &rel : system_msbts(b)) {
        Msbt m = need_msbt(b, rel);
        const std::string *gb_ = m.text("GetBottle");
        std::string wait = last_wait(gb_ ? *gb_ : ""), text;
        std::string lang = lang_of(rel);
        const std::vector<const char *> *pg = tr(text::table(text::T, "escape"), lang);
        if (!pg) {
            text = "You got the " + RED + "Escape Bottle" + WHITE + "! Use it to\nwarp out of trouble." + wait + PAGE +
                   "Uncork it in a dungeon to return\nto the entrance!" + wait + PAGE +
                   "Uncork it anywhere else to be\nwhisked home to Mabe Village!" + wait + std::string(1, '\0');
        } else {
            std::string mabe;
            if (!Names(b, rel).place("MabeVillage", mabe)) mabe = "Mabe Village";
            std::map<std::string, Seg> parts = {{"name", our(tr1(text::table(text::T, "escape-name"), lang), true)},
                                                {"mabe", game(mabe, false)}};
            std::vector<std::vector<Seg>> ps;
            for (const char *p : *pg) ps.push_back(fill(p, parts));
            text = pages(ps, wait, lang);
        }
        m.set(ENTRY, text);
        write_msbt(b, rel, m);
    }
}

}  // namespace ours
}  // namespace rando
