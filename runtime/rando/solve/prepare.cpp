// The native solver's preparation: tools/rando/las_headless.py main() before the item fill, entrances and room
// shuffle aside (prepare.h).
#include "prepare.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>

#include "prepare_lib.h"
#include "prepare_pyset.h"

namespace rando {
namespace solve {

std::string solve_data_json();  // (solve_data.cpp, solve_gate_data.cpp: tools/rando/export_solve_data.py)
std::string solve_gate_json();

const Value &prep_data() {
    static Value v;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string s = solve_data_json();
        v = Value::parse_json(s.data(), s.size());
    });
    return v;
}

const Value &prep_gate_data() {
    static Value v;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string s = solve_gate_json();
        v = Value::parse_json(s.data(), s.size());
    });
    return v;
}

// SHA-512 (FIPS 180-4) for random.seed(str), as shuffler.cpp's (written from the standard)
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

static uint64_t ror(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

void prepare_sha512(uint8_t out[64], const uint8_t *msg, size_t len) {
    uint64_t h[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                     0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    std::vector<uint8_t> m(msg, msg + len);
    m.push_back(0x80);
    while (m.size() % 128 != 112) m.push_back(0);
    for (int i = 0; i < 8; i++) m.push_back(0);  // (the length's top 64 bits)
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 7; i >= 0; i--) m.push_back((uint8_t)(bits >> (8 * i)));
    for (size_t blk = 0; blk < m.size(); blk += 128) {
        uint64_t w[80];
        for (int t = 0; t < 16; t++) {
            w[t] = 0;
            for (int b = 0; b < 8; b++) w[t] = w[t] << 8 | m[blk + 8 * t + b];
        }
        for (int t = 16; t < 80; t++) {
            uint64_t s0 = ror(w[t - 15], 1) ^ ror(w[t - 15], 8) ^ (w[t - 15] >> 7);
            uint64_t s1 = ror(w[t - 2], 19) ^ ror(w[t - 2], 61) ^ (w[t - 2] >> 6);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int t = 0; t < 80; t++) {
            uint64_t t1 = hh + (ror(e, 14) ^ ror(e, 18) ^ ror(e, 41)) + ((e & f) ^ (~e & g)) + K512[t] + w[t];
            uint64_t t2 = (ror(a, 28) ^ ror(a, 34) ^ ror(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    for (int i = 0; i < 8; i++)
        for (int b = 0; b < 8; b++) out[8 * i + b] = (uint8_t)(h[i] >> (56 - 8 * b));
}

static bool starts_with(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }

// an iterable of str as Python iterates it: a list's items, a str's characters (UTF-8 code points)
static std::vector<std::string> iter_strs(const Value &v) {
    std::vector<std::string> out;
    if (v.is_list()) {
        for (const Value &x : v.list()) out.push_back(py_str(x));
    } else if (v.is_str()) {
        const std::string &s = v.s();
        for (size_t i = 0; i < s.size();) {
            unsigned char c = (unsigned char)s[i];
            size_t n = c < 0x80 ? 1 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3 : 4;
            out.push_back(s.substr(i, n));
            i += n;
        }
    } else {
        throw PyError("TypeError", "'" + std::string(v.is_none() ? "NoneType" : v.is_dict() ? "dict" : "int") +
                                       "' object is not iterable");
    }
    return out;
}

static int64_t qty(const Value &item_defs, const std::string &k) { return item_defs.at(k).at("quantity").i(); }
static void add_qty(Value &item_defs, const std::string &k, int64_t n) {
    item_defs.at(k).set("quantity", Value((long long)(qty(item_defs, k) + n)));
}

// the most plentiful junk rupee (max() keeps the first of equals): what souls, pets and Trendy's dropped prize take
static std::string most_junk(const Value &item_defs, const Value &junk) {
    std::string best;
    int64_t most = 0;
    for (const Value &k : junk.list())
        if (best.empty() || qty(item_defs, k.s()) > most) best = k.s(), most = qty(item_defs, k.s());
    return best;
}

// souls._and: the atom ANDed into every condition of a location (condition-basic: just the atom when it has none)
static void and_atom(Value &logic_defs, const std::string &loc, const std::string &atom) {
    Value v = logic_defs.get(loc);
    if (!v.truthy()) return;
    for (const std::string &k : v.dict().keys()) {
        const Value &c = v.at(k);
        if (starts_with(k, "condition-") && c.truthy()) v.set(k, "(" + py_str(c) + ") & " + atom);
    }
    if (!v.has("condition-basic")) v.set("condition-basic", atom);
}

// \b<token>\b in a condition (tokens start and end with a word character); not_followed: (?![-\[]) after it
static bool word_char(char c) { return isalnum((unsigned char)c) || c == '_'; }
static size_t find_token(const std::string &s, const std::string &tok, size_t from, bool not_followed) {
    for (size_t at = s.find(tok, from); at != std::string::npos; at = s.find(tok, at + 1)) {
        size_t end = at + tok.size();
        if (at > 0 && word_char(s[at - 1])) continue;
        if (end < s.size() && word_char(s[end])) continue;
        if (not_followed && end < s.size() && (s[end] == '-' || s[end] == '[')) continue;
        return at;
    }
    return std::string::npos;
}

static std::string soul_las_name(const std::string &name) {
    std::string out = "soul-";
    for (char c : name) out += c == ' ' ? '-' : (char)tolower((unsigned char)c);
    return out;
}
static std::string soul_item_key(const std::string &name, const std::string &kind) {
    std::string out = kind == "boss" ? "BossSoul" : "MinibossSoul";
    for (char c : name)
        if (c != ' ') out += c;
    return out;
}

struct Soul { std::string kind, name; Value kill; };

static std::vector<Soul> souls_of(const std::vector<std::string> &kinds) {
    const Value &S = prep_data().at("souls");
    std::vector<Soul> out;
    bool boss = std::find(kinds.begin(), kinds.end(), "boss") != kinds.end();
    bool mini = std::find(kinds.begin(), kinds.end(), "mini") != kinds.end();
    if (boss)
        for (const Value &b : S.at("bosses").list()) out.push_back({"boss", b.list()[1].s(), b.list()[3]});
    if (mini)
        for (const Value &m : S.at("minibosses").list()) out.push_back({"mini", m.list()[0].s(), m.list()[2]});
    return out;
}

// souls.add_to_logic (room_logic.EXIT_REQ, its room-graph side, lives with the room shuffle / prep_gate_data)
static size_t souls_add(Value &logic_defs, Value &item_defs, const std::vector<std::string> &kinds) {
    const Value &S = prep_data().at("souls");
    std::vector<Soul> all = souls_of(kinds);
    // Dampe's chambers keep every boss / mini-boss: their kills become soul-free copies
    for (const Soul &s : all) {
        if (!s.kill.truthy() || !logic_defs.has(s.kill.s())) continue;
        const std::string kill = s.kill.s();
        std::vector<std::string> uses;
        for (const auto &e : logic_defs.dict()) {
            Value c = e.value.get("condition-basic");
            if (starts_with(e.key, "dampe-") && c.truthy() && find_token(py_str(c), kill, 0, false) != std::string::npos)
                uses.push_back(e.key);
        }
        if (uses.empty()) continue;
        logic_defs.set(kill + "-chamber", logic_defs.at(kill).deepcopy());
        for (const std::string &k : uses) {
            std::string c = logic_defs.at(k).at("condition-basic").s(), out;
            size_t from = 0;
            for (size_t at; (at = find_token(c, kill, from, true)) != std::string::npos; from = at + kill.size())
                out += c.substr(from, at - from) + kill + "-chamber";
            logic_defs.at(k).set("condition-basic", out + c.substr(from));
        }
    }
    for (const Soul &s : all) {
        Value d = S.at("model").deepcopy();
        d.set("type", "important"), d.set("quantity", 1), d.set("item-key", soul_item_key(s.name, s.kind));
        item_defs.set(soul_las_name(s.name), d);
        if (s.kill.truthy()) and_atom(logic_defs, s.kill.s(), soul_las_name(s.name));
    }
    if (std::find(kinds.begin(), kinds.end(), "boss") != kinds.end())
        for (const char *loc : {"D0-fairy-1", "D0-fairy-2"}) {
            Value v = logic_defs.at(loc);
            v.set("condition-basic", v.at("condition-basic").s() + " & kill-evil-orb");
        }
    if (std::find(kinds.begin(), kinds.end(), "mini") != kinds.end())
        for (const Value &m : S.at("minibosses").list())
            for (const Value &loc : m.list()[3].list()) and_atom(logic_defs, loc.s(), soul_las_name(m.list()[0].s()));
    for (size_t i = 0; i < all.size(); i++) {
        std::string junk = most_junk(item_defs, S.at("junk"));
        if (qty(item_defs, junk) <= 0) throw PyError("AssertionError", "souls: no junk left for " + all[i].name);
        add_qty(item_defs, junk, -1);
    }
    return all.size();
}

Prepared prepare(const Value &request, const GameFiles &game) {
    const Value &D = prep_data();
    Prepared st;
    auto say = [&](const std::string &s) { st.said.push_back(s); };

    // default_settings + the request
    Value settings = Value::new_dict();
    for (const auto &e : D.at("defaults").dict()) settings.set(e.key, e.value.deepcopy());
    settings.set("seed", request.has("seed") ? py_str(request.at("seed")) : std::string("LARecomp"));
    CpStrSet excluded;
    for (const Value &src : D.at("default_excluded").list())
        excluded.update(src.is_list() ? str_list(src) : str_list(D.at("groups").at(src.s())));
    for (const auto &e : request.dict()) {
        if (e.key == "exclude") excluded.update(iter_strs(e.value));
        else if (e.key == "include") excluded.difference_update(iter_strs(e.value));
        else if (e.key == "excluded-locations") {  // (a list given for the set itself: set() of it below)
            excluded = CpStrSet();
            excluded.update(iter_strs(e.value));
        } else settings.set(e.key, e.value.deepcopy());
    }
    excluded = excluded.copy();  // set(settings['excluded-locations'])
    if (!D.at("stealing").contains(settings.at("stealing")))
        throw PyError("SystemExit", "bad stealing setting " + settings.at("stealing").repr());
    const Value &modes = D.at("room_modes");
    Value mode = settings.at("dungeon-room-shuffle");
    if (!mode.is_str() || !modes.has(mode.s()))
        throw PyError("SystemExit", "bad dungeon-room-shuffle setting " + mode.repr());
    settings.set("dungeon-room-shuffle", modes.at(mode.s()));
    // optional location groups (Randomizer tab): included = real checks with logic instead of junk
    const char *groups[][2] = {{"include-fishing", "FISHING_REWARDS"}, {"include-rapids", "RAPIDS_REWARDS"},
                               {"include-dampe", "DAMPE_REWARDS"}, {"include-trade-gifts", "TRADE_GIFT_LOCATIONS"},
                               {"include-blue-rupees", "BLUE_RUPEES"}};
    for (auto &g : groups)
        if (request.get(g[0]).truthy()) excluded.difference_update(str_list(D.at("groups").at(g[1])));
    if (request.get("include-blue-rupees").truthy()) settings.set("blupsanity", true);

    Value logic_defs = D.at("logic").deepcopy();
    Value item_defs = D.at("items").deepcopy();
    // Escape Bottle and Hint Sheet: two Bottles less in the pool, two 50 Rupees in their place
    add_qty(item_defs, "bottle", -2);
    add_qty(item_defs, "rupee-50", 2);

    if (settings.get("hookshot-anything").truthy()) {  // (hookshot_logic.apply)
        int n = 0;
        for (const auto &r : D.at("hookshot_routes").dict()) {
            Value node = logic_defs.get(r.key);
            if (!node.truthy() || !node.has("condition-basic") || node.at("condition-basic").contains(r.value)) continue;
            node.set("condition-basic", "(" + py_str(node.at("condition-basic")) + ") | (" + r.value.s() + ")");
            n++;
        }
        say("hookshot anything: " + std::to_string(n) + " routes");
    }
    Value room_mode = settings.get("dungeon-room-shuffle", "off");
    bool collapsed = !(room_mode == Value("off") || room_mode.is_none() || room_mode == Value(false));
    if (collapsed) {  // (eagle_tower.apply_logic)
        Value node = logic_defs.at("D7-2F");
        node.set("condition-basic", D.at("eagle").at("collapsed_2f"));
        for (const char *lv : {"advanced", "glitched", "hell", "trick"}) node.dict().erase(std::string("condition-") + lv);
        say("eagle's tower collapsed: 1 conditions");
    }

    // keysanity: the freed dungeon items are placed with logic
    Value ks = settings.get("keysanity", "off");
    std::vector<std::string> ks_free;
    if (ks == Value("keys")) ks_free = {"key-"};
    else if (ks == Value("keys+nk")) ks_free = {"key-", "nightmare-key-"};
    else if (ks == Value("all")) ks_free = {"key-", "nightmare-key-", "map", "compass", "stone-beak"};
    for (auto &e : item_defs.dict()) {
        bool free = false;
        for (const std::string &p : ks_free) free = free || starts_with(e.key, p);
        if (free && e.key.size() >= 2 && e.key[e.key.size() - 2] == 'D') e.value.set("type", "important");
    }

    if (settings.get("boss-souls").truthy()) st.soul_kinds.push_back("boss");
    if (settings.get("miniboss-souls").truthy()) st.soul_kinds.push_back("mini");
    if (!st.soul_kinds.empty())
        say("souls: " + std::to_string(souls_add(logic_defs, item_defs, st.soul_kinds)) + " in the pool");

    std::string seed = py_str(settings.at("seed"));
    Value traps_v = settings.get("traps", "none");
    std::string traps = traps_v.is_str() ? traps_v.s() : std::string();
    st.pot_locs = Value::new_dict(), st.bush_locs = Value::new_dict(), st.rock_locs = Value::new_dict();
    st.dig_locs = Value::new_dict(), st.apple_locs = Value::new_dict();
    if (settings.get("pots").truthy()) {
        PyRandom rng;
        rng.seed(seed + "-pots");
        std::vector<std::string> missable = str_list(collapsed ? D.at("eagle").at("missable_collapsed") : D.at("pots").at("missable"));
        st.pot_locs = pots_add(game, missable, logic_defs, item_defs, traps, rng);
        say("pots: " + std::to_string(st.pot_locs.dict().size()) + " locations");
    }
    TileZones zones;
    bool have_zones = false;
    auto field_zones = [&]() -> const TileZones & {
        if (!have_zones) zones = tile_zones(game), have_zones = true;
        return zones;
    };
    if (settings.get("bushes").truthy()) {
        PyRandom rng;
        rng.seed(seed + "-bushes");
        st.bush_locs = bushes_add(game, field_zones(), settings.get("open-mabe").truthy(), logic_defs, item_defs, traps, rng);
        say("bushes: " + std::to_string(st.bush_locs.dict().size()) + " locations");
    }
    if (settings.get("rocks").truthy()) {
        PyRandom rng;
        rng.seed(seed + "-rocks");
        st.rock_locs = rocks_add(game, field_zones(), settings.get("classic-d2").truthy(), settings.get("open-mabe").truthy(),
                                 logic_defs, item_defs, traps, rng);
        say("rocks: " + std::to_string(st.rock_locs.dict().size()) + " locations");
    }
    if (settings.get("digs").truthy()) {
        st.dig_locs = digs_add(game, field_zones(), logic_defs, item_defs);
        say("digs: " + std::to_string(st.dig_locs.dict().size()) + " locations");
    }
    if (settings.get("apples").truthy()) {
        PyRandom rng;
        rng.seed(seed + "-apples");
        st.apple_locs = apples_add(game, field_zones(), logic_defs, item_defs, traps, rng);
        say("apple trees: " + std::to_string(st.apple_locs.dict().size()) + " locations");
    }

    st.sanity_inside = Value::new_dict();
    Value shop = settings.get("shopsanity", "off");
    if (shop == Value("vanilla") || shop == Value("random")) {  // (shopsanity.add_to_logic)
        const Value &S = D.at("shop");
        int64_t spare = 0;
        for (const Value &loc : S.at("dropped").list()) {
            Value d = logic_defs.get(loc.s());
            logic_defs.dict().erase(loc.s());
            if (d.truthy() && d.at("content") == Value("chamber-stone")) add_qty(item_defs, "chamber-stone", -1);
            else if (d.truthy()) spare++;
        }
        Value inside = Value::new_list();
        for (const Value &c : S.at("checks").list()) {
            std::string loc = c.list()[0].s(), filler = py_str(c.list()[3]);
            Value d = logic_defs.get(loc);
            if (d.is_none()) {
                d = Value::new_dict();
                d.set("type", "item"), d.set("subtype", "shop"), d.set("content", c.list()[3]), d.set("region", "mabe");
                d.set("spoiler-region", "mabe-village");
                logic_defs.set(loc, d);
                if (spare) spare--;
                else add_qty(item_defs, filler, 1);
            }
            d.set("condition-basic", "can-shop");
            inside.list().push_back(Value(List{Value(loc), Value("can-shop")}));
        }
        settings.set("fast-stealing", false);
        st.sanity_inside.set(S.at("door").s(), inside);
    }
    if (settings.get("trendsanity").truthy()) {  // (trendsanity.add_to_logic)
        const Value &T = D.at("trendy");
        Value d = logic_defs.get(T.at("dropped").s());
        logic_defs.dict().erase(T.at("dropped").s());
        if (d.truthy()) add_qty(item_defs, most_junk(item_defs, Value(List{"rupee-50", "rupee-20", "rupee-100", "rupee-300"})), -1);
        excluded.add(T.at("final").s());
        Value inside = Value::new_list();
        for (const Value &c : T.at("checks").list()) {
            logic_defs.at(c.list()[0].s()).set("condition-basic", "can-farm-rupees");
            inside.list().push_back(Value(List{c.list()[0], Value("can-farm-rupees")}));
        }
        st.sanity_inside.set(T.at("door").s(), inside);
    }

    // pets.add_to_pool
    if (settings.get("pet-rooster").truthy()) st.pets.push_back("rooster");
    if (!settings.get("open-desert", true).truthy()) st.pets.push_back("marin-companion");
    if (settings.get("pet-ghost").truthy()) st.pets.push_back("ghost");
    for (const std::string &p : st.pets) {
        const List &d = D.at("pets").at("pets").at(p).list();
        Value item = Value::new_dict();
        item.set("type", "important"), item.set("quantity", 1), item.set("item-key", d[0]), item.set("model-path", d[2]);
        item.set("model-name", d[3]), item.set("npc-key", d[1]);
        item_defs.set(p, item);
        add_qty(item_defs, most_junk(item_defs, Value(List{"rupee-50", "rupee-20", "rupee-100", "rupee-300"})), -1);
    }
    auto has_pet = [&](const char *p) { return std::find(st.pets.begin(), st.pets.end(), p) != st.pets.end(); };
    if (has_pet("rooster") && !logic_defs.dict().erase("rooster-statue")) throw PyError("KeyError", "'rooster-statue'");
    if (has_pet("ghost")) {
        if (logic_defs.at("ghost-reward").get("condition-basic") != Value("surf-harp"))
            throw PyError("AssertionError", "pets: ghost-reward isn't surf-harp");
        logic_defs.at("ghost-reward").set("condition-basic", "ghost");
    }
    if (has_pet("marin-companion")) {
        if (logic_defs.at("desert").get("condition-basic") != Value("animal-village & break-bush"))
            throw PyError("AssertionError", "pets: desert isn't animal-village & break-bush");
        logic_defs.at("desert").set("condition-basic", "animal-village & break-bush & marin-companion");
    }
    if (!st.pets.empty()) {
        std::string s;
        for (const std::string &p : st.pets) s += (s.empty() ? "" : ", ") + p;
        say("pets: " + s);
    }

    // triforce.counts / add_to_pool
    Value th = settings.get("triforce-hunt", 0), te = settings.get("triforce-extra", 0);
    int64_t needed = th.truthy() ? th.to_int() : 0;
    if (needed > 0) {
        int64_t pct = te.truthy() ? te.to_int() : 0;
        st.tri_needed = needed, st.tri_placed = needed + (int64_t)ceil((double)(needed * pct) / 100.0);
        const Value &T = D.at("triforce");
        Value item = T.at("model").deepcopy();
        item.set("type", "triforce"), item.set("quantity", 0), item.set("item-key", T.at("item_key"));
        item_defs.set(T.at("las_name").s(), item);
        settings.set("triforce-needed", (long long)st.tri_needed);
        settings.set("triforce-placed", (long long)st.tri_placed);
        say("triforce: " + std::to_string(st.tri_placed) + " pieces, " + std::to_string(st.tri_needed) + " needed");
    }

    List ex;
    for (const std::string &k : excluded.items()) ex.push_back(Value(k));
    settings.set("excluded-locations", Value(std::move(ex)));
    st.settings = settings, st.item_defs = item_defs, st.logic_defs = logic_defs;
    return st;
}

void finish_prefill(Prepared &st) {
    if (st.settings.get("dungeon-room-shuffle", "off") != Value("off"))
        throw PyError("NotImplementedError", "dungeon room shuffle: not part of the preparation");
    if (std::find(st.soul_kinds.begin(), st.soul_kinds.end(), "mini") == st.soul_kinds.end()) return;
    // souls.vanilla_gate on every dungeon but Eagle's Tower (no room graph for the uncollapsed tower): the soul atoms
    // each cell needs come from room_logic's room graph (tools/rando/export_solve_data.py --romfs)
    const Value &G = prep_gate_data();
    const Value &need_all = G.at("need").at(st.settings.get("hookshot-anything").truthy() ? "hook" : "plain");
    Value targets = G.at("targets").copy();
    for (const auto &e : st.pot_locs.dict())
        if (e.value.get("room").truthy()) targets.set(e.key, e.value.at("room"));
    Value &logic_defs = st.logic_defs;
    size_t added = 0;
    for (const Value &lvv : G.at("levels").list()) {
        const std::string lv = lvv.s();
        const Value need = need_all.get(lv, Value::new_dict());
        Value here = targets.copy();
        if (G.at("arena").has(lv)) {  // boss drops (no room listed): the boss sits behind its door
            const List &a = G.at("arena").at(lv).list();
            std::string prefix = "D" + std::to_string(a[0].i()) + "-";
            for (const auto &e : logic_defs.dict())
                if (e.value.get("type") == Value("item") && e.value.get("subtype") == Value("boss") &&
                    starts_with(e.key, prefix) && !here.get(e.key).truthy())
                    here.set(e.key, a[1]);
        }
        for (const auto &e : here.dict()) {
            if (!e.value.truthy() || !starts_with(e.value.s(), lv + "_") || !logic_defs.has(e.key)) continue;
            Value atoms = need.get(e.value.s().substr(e.value.s().rfind('_') + 1));
            if (!atoms.is_list()) continue;
            for (const Value &atom : atoms.list()) {  // (sorted by the export)
                and_atom(logic_defs, e.key, atom.s());
                added++;
            }
        }
    }
    st.said.push_back("mini-boss souls on vanilla dungeons: " + std::to_string(added) + " conditions");
}

GameFiles game_files_dir(const std::string &romfs) {
    GameFiles g;
    g.read = [romfs](const std::string &path, std::string &out) {
        std::ifstream f(romfs + "/" + path, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    };
    g.list = [romfs](const std::string &dir, std::vector<std::string> &names) {
        DIR *d = opendir((romfs + "/" + dir).c_str());
        if (!d) return false;
        names.clear();
        while (struct dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != "..") names.push_back(n);
        }
        closedir(d);
        return true;
    };
    return g;
}

}  // namespace solve
}  // namespace rando
