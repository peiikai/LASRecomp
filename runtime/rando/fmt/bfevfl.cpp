// Event flows (bfevfl.h). Written for LAS Recomp from the format; the writer's layout follows the file evfl 1.2.0
// writes (see the header) so files come out byte for byte the same.
#include "bfevfl.h"

#include <string.h>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace rando {

// ---- model helpers
EvflValue *EvflContainer::get(const std::string &key) {
    for (auto &kv : items)
        if (kv.first == key) return &kv.second;
    return nullptr;
}
const EvflValue *EvflContainer::get(const std::string &key) const {
    for (const auto &kv : items)
        if (kv.first == key) return &kv.second;
    return nullptr;
}
void EvflContainer::set(const std::string &key, const EvflValue &v) {
    if (EvflValue *p = get(key)) *p = v;
    else items.emplace_back(key, v);
}

static int index_of(const std::vector<std::string> &l, const std::string &s) {
    for (size_t i = 0; i < l.size(); i++)
        if (l[i] == s) return (int)i;
    return -1;
}
int EvflActor::find_action(const std::string &name) const { return index_of(actions, name); }
int EvflActor::find_query(const std::string &name) const { return index_of(queries, name); }
uint16_t EvflActor::add_action(const std::string &name) {
    int i = find_action(name);
    if (i >= 0) return (uint16_t)i;
    actions.push_back(name);
    return (uint16_t)(actions.size() - 1);
}
uint16_t EvflActor::add_query(const std::string &name) {
    int i = find_query(name);
    if (i >= 0) return (uint16_t)i;
    queries.push_back(name);
    return (uint16_t)(queries.size() - 1);
}

int EvflFlowchart::find_event(const std::string &n) const {
    for (size_t i = 0; i < events.size(); i++)
        if (events[i].name == n) return (int)i;
    return -1;
}
int EvflFlowchart::find_entry_point(const std::string &n) const {
    for (size_t i = 0; i < entry_points.size(); i++)
        if (entry_points[i].name == n) return (int)i;
    return -1;
}
int EvflFlowchart::find_actor(const std::string &n, const std::string &sub) const {
    for (size_t i = 0; i < actors.size(); i++)
        if (actors[i].id.name == n && actors[i].id.sub_name == sub) return (int)i;
    return -1;
}
uint16_t EvflFlowchart::add_actor(const std::string &n, const std::string &sub) {
    int i = find_actor(n, sub);
    if (i >= 0) return (uint16_t)i;
    EvflActor a;
    a.id = {n, sub};
    actors.push_back(a);
    return (uint16_t)(actors.size() - 1);
}

// switch cases as the file holds them: one per value, at the value's first place, going to its last event
static std::vector<std::pair<uint32_t, uint16_t>> unique_cases(const std::vector<std::pair<uint32_t, uint16_t>> &cs) {
    std::vector<std::pair<uint32_t, uint16_t>> out;
    for (const auto &c : cs) {
        auto it = std::find_if(out.begin(), out.end(), [&](const std::pair<uint32_t, uint16_t> &o) {
            return o.first == c.first;
        });
        if (it != out.end()) it->second = c.second;
        else out.push_back(c);
    }
    return out;
}

// ---- reading
namespace {
// strict UTF-8 (strings that aren't are refused, as evfl refuses them)
bool utf8_ok(const std::string &s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        int len;
        uint32_t cp;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) len = 2, cp = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF) len = 3, cp = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4) len = 4, cp = c & 0x07;
        else return false;
        if (i + len > n) return false;
        for (int k = 1; k < len; k++) {
            uint8_t d = (uint8_t)s[i + k];
            if ((d & 0xC0) != 0x80) return false;
            cp = cp << 6 | (d & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

struct Reader {
    const std::string &d;
    uint64_t le(uint64_t at, int n) const {
        if (at > d.size() || n > (int)(d.size() - at)) throw std::out_of_range("bfevfl: read past the end");
        uint64_t v = 0;
        for (int i = 0; i < n; i++) v |= (uint64_t)(uint8_t)d[at + i] << (8 * i);
        return v;
    }
    uint16_t u16(uint64_t at) const { return (uint16_t)le(at, 2); }
    uint32_t u32(uint64_t at) const { return (uint32_t)le(at, 4); }
    uint64_t u64(uint64_t at) const { return le(at, 8); }
    float f32(uint64_t at) const {
        uint32_t u = u32(at);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    static void need(bool ok, const char *what) {
        if (!ok) throw std::runtime_error(std::string("bfevfl: ") + what);
    }
    static std::string checked(std::string s) {
        need(utf8_ok(s), "string isn't UTF-8");
        return s;
    }
    std::string cstr(uint64_t at) const {
        need(at < d.size(), "string past the end");
        size_t e = d.find('\0', at);
        need(e != std::string::npos, "unterminated string");
        return checked(d.substr(at, e - at));
    }
    // a pooled string: a pointer to a 16-bit length and the bytes (0: the empty string)
    std::string ref(uint64_t at) const {
        uint64_t p = u64(at);
        if (!p) return "";
        uint16_t n = u16(p);
        le(p + 2, 0);
        need(p + 2 + n <= d.size(), "string past the end");
        return checked(d.substr(p + 2, n));
    }

    std::vector<std::string> dic(uint64_t at) const {
        std::vector<std::string> out;
        uint32_t n = u32(at + 4);
        for (uint32_t i = 0; i < n; i++) {
            out.push_back(ref(at + 0x18 + 0x10 * (uint64_t)i + 8));
            need(!out.back().empty(), "dictionary entry without a name");
        }
        return out;
    }

    EvflValue item(uint64_t at) const {
        EvflValue v;
        uint8_t type = (uint8_t)le(at, 1);
        uint16_t n = u16(at + 2);
        need(u32(at + 4) == 0 && u64(at + 8) == 0, "bad container item");
        uint64_t p = at + 0x10;
        switch (type) {
        case EvflValue::Int: v.i = (int32_t)u32(p); break;
        case EvflValue::Bool: v.b = u32(p) != 0; break;
        case EvflValue::Float: v.f = f32(p); break;
        case EvflValue::String: case EvflValue::Argument: v.s = ref(p); break;
        case EvflValue::IntArray: for (uint16_t k = 0; k < n; k++) v.ints.push_back((int32_t)u32(p + 4 * k)); break;
        case EvflValue::BoolArray: for (uint16_t k = 0; k < n; k++) v.bools.push_back(u32(p + 4 * k) != 0); break;
        case EvflValue::FloatArray: for (uint16_t k = 0; k < n; k++) v.floats.push_back(f32(p + 4 * k)); break;
        case EvflValue::StringArray: for (uint16_t k = 0; k < n; k++) v.strs.push_back(ref(p + 8 * k)); break;
        case EvflValue::ActorIdentifier: v.actor = {ref(p), ref(p + 8)}; break;
        case 1: need(false, "container inside a container"); break;
        case 6: case 11: need(false, "wide strings aren't handled"); break;
        default: need(false, "unknown container item type");
        }
        v.type = (EvflValue::Type)type;
        return v;
    }
    EvflContainer container(uint64_t at) const {
        EvflContainer c;
        need(le(at, 1) == 1, "not a container");
        need(u32(at + 4) == 0, "bad container");
        uint64_t dp = u64(at + 8);
        need(dp != 0, "container without a dictionary");
        std::vector<std::string> names = dic(dp);
        for (size_t i = 0; i < names.size(); i++) c.set(names[i], item(u64(at + 0x10 + 8 * i)));
        return c;
    }
    bool opt_container(uint64_t at, EvflContainer &c) const {
        uint64_t p = u64(at);
        if (p) c = container(p);
        return p != 0;
    }

    std::vector<std::string> refs(uint64_t at, uint16_t n) const {
        std::vector<std::string> out;
        for (uint16_t i = 0; i < n; i++) out.push_back(ref(at + 8 * i));
        return out;
    }
    EvflActor actor(uint64_t at) const {
        EvflActor a;
        a.id = {ref(at), ref(at + 8)};
        a.argument_name = ref(at + 0x10);
        a.has_params = opt_container(at + 0x28, a.params);
        a.argument_entry_point = u16(at + 0x34), a.concurrent_clips = u16(at + 0x36);
        a.actions = refs(u64(at + 0x18), u16(at + 0x30));
        a.queries = refs(u64(at + 0x20), u16(at + 0x32));
        return a;
    }

    EvflEvent event(uint64_t at) const {
        EvflEvent e;
        e.name = ref(at);
        uint8_t type = (uint8_t)le(at + 8, 1);
        need(type <= EvflEvent::SubFlow, "unknown event type");
        e.type = (EvflEvent::Type)type;
        uint16_t a = u16(at + 0xA), b = u16(at + 0xC), c = u16(at + 0xE);
        switch (e.type) {
        case EvflEvent::Action:
            e.next = a, e.actor = b, e.action = c;
            opt_container(at + 0x10, e.params);
            need(u64(at + 0x18) == 0 && u64(at + 0x20) == 0, "bad action event");
            break;
        case EvflEvent::Switch: {
            e.actor = b, e.action = c;
            opt_container(at + 0x10, e.params);
            uint64_t p = u64(at + 0x18);
            for (uint16_t i = 0; i < a; i++) {
                e.cases.emplace_back(u32(p), u16(p + 4));
                p = (p + 6 + 7) & ~7ull;
            }
            e.cases = unique_cases(e.cases);
            need(u64(at + 0x20) == 0, "bad switch event");
            break;
        }
        case EvflEvent::Fork: {
            e.join = b;
            need(c == 0, "bad fork event");
            uint64_t p = u64(at + 0x10);
            need(a != 0 && p != 0, "fork event without forks");
            for (uint16_t i = 0; i < a; i++) e.forks.push_back(u16(p + 2 * i));
            need(u64(at + 0x18) == 0 && u64(at + 0x20) == 0, "bad fork event");
            break;
        }
        case EvflEvent::Join:
            e.next = a;
            need(b == 0 && c == 0 && u64(at + 0x10) == 0 && u64(at + 0x18) == 0 && u64(at + 0x20) == 0,
                 "bad join event");
            break;
        case EvflEvent::SubFlow:
            e.next = a;
            need(b == 0 && c == 0, "bad sub flow event");
            opt_container(at + 0x10, e.params);
            e.flowchart = ref(at + 0x18), e.entry_point = ref(at + 0x20);
            need(!e.entry_point.empty(), "sub flow event without an entry point");
            break;
        }
        return e;
    }

    EvflEntryPoint entry_point(uint64_t at, const std::string &name) const {
        EvflEntryPoint ep;
        ep.name = name;
        uint64_t list = u64(at);
        uint16_t n = u16(at + 0x18);
        need(u64(at + 8) == 0 && u64(at + 0x10) == 0 && u16(at + 0x1A) == 0 && u16(at + 0x1E) == 0,
             "bad entry point");
        ep.main_event = u16(at + 0x1C);
        if (n) {
            need(list != 0, "entry point without its sub flow list");
            need(list + 2 * (uint64_t)n <= d.size(), "sub flow list past the end");  // (recomputed on write)
        }
        return ep;
    }

    static void link(uint16_t idx, size_t n, bool optional) {
        need((optional && idx == EVFL_NONE) || idx < n, "index out of range");
    }

    EvflFlowchart flowchart(uint64_t at) const {
        EvflFlowchart f;
        need(u32(at + 8) == 0 && u32(at + 0xC) == 0, "bad flowchart header");
        uint16_t nactors = u16(at + 0x10), nevents = u16(at + 0x16), neps = u16(at + 0x18);
        need(u16(at + 0x1A) == 0 && u16(at + 0x1C) == 0 && u16(at + 0x1E) == 0, "bad flowchart header");
        f.name = ref(at + 0x20);
        uint64_t p = u64(at + 0x28);
        if (p)
            for (uint16_t i = 0; i < nactors; i++) f.actors.push_back(actor(p + 0x38 * (uint64_t)i));
        p = u64(at + 0x30);
        if (p)
            for (uint16_t i = 0; i < nevents; i++) f.events.push_back(event(p + 0x28 * (uint64_t)i));
        uint64_t dp = u64(at + 0x38);
        need(dp != 0, "flowchart without an entry point dictionary");
        std::vector<std::string> names = dic(dp);
        need(names.size() == neps, "entry point count");
        p = u64(at + 0x40);
        for (size_t i = 0; i < names.size(); i++) f.entry_points.push_back(entry_point(p + 0x20 * i, names[i]));

        size_t na = f.actors.size(), ne = f.events.size();
        for (const EvflActor &a : f.actors) link(a.argument_entry_point, f.entry_points.size(), true);
        for (const EvflEvent &e : f.events) {
            switch (e.type) {
            case EvflEvent::Action:
                link(e.next, ne, true), link(e.actor, na, false);
                link(e.action, f.actors[e.actor].actions.size(), false);
                break;
            case EvflEvent::Switch:
                link(e.actor, na, false), link(e.action, f.actors[e.actor].queries.size(), false);
                for (const auto &c : e.cases) link(c.second, ne, false);
                break;
            case EvflEvent::Fork:
                link(e.join, ne, false);
                for (uint16_t x : e.forks) link(x, ne, false);
                break;
            default: link(e.next, ne, true); break;
            }
        }
        for (const EvflEntryPoint &ep : f.entry_points) link(ep.main_event, ne, true);
        return f;
    }

    EvflTimeline timeline(uint64_t at) const {
        EvflTimeline t;
        need(u32(at + 8) == 0 && u32(at + 0xC) == 0, "bad timeline header");
        t.duration = f32(at + 0x10);
        uint16_t nactors = u16(at + 0x14), nclips = u16(at + 0x18), nones = u16(at + 0x1A), nsubs = u16(at + 0x1C),
                 ncuts = u16(at + 0x1E);
        t.name = ref(at + 0x20);
        uint64_t p = u64(at + 0x28);
        if (p)
            for (uint16_t i = 0; i < nactors; i++) t.actors.push_back(actor(p + 0x38 * (uint64_t)i));
        if ((p = u64(at + 0x30)))
            for (uint16_t i = 0; i < nclips; i++) {
                uint64_t q = p + 0x18 * (uint64_t)i;
                EvflClip c;
                c.start_time = f32(q), c.duration = f32(q + 4);
                c.actor = u16(q + 8), c.action = u16(q + 0xA), c.actor_concurrent_clip = (uint8_t)le(q + 0xC, 1);
                c.has_params = opt_container(q + 0x10, c.params);
                t.clips.push_back(c);
            }
        if ((p = u64(at + 0x38)))
            for (uint16_t i = 0; i < nones; i++) {
                uint64_t q = p + 0x18 * (uint64_t)i;
                EvflOneshot o;
                o.time = f32(q), o.actor = u16(q + 4), o.action = u16(q + 6);
                o.has_params = opt_container(q + 0x10, o.params);
                t.oneshots.push_back(o);
            }
        if ((p = u64(at + 0x40)))
            for (uint32_t i = 0; i < 2u * nclips; i++) {
                EvflTrigger tr;
                tr.clip = u16(p + 4 * i), tr.type = (uint8_t)le(p + 4 * i + 2, 1);
                need(tr.type == EvflTrigger::Enter || tr.type == EvflTrigger::Leave || tr.type == EvflTrigger::None,
                     "unknown trigger type");
                t.triggers.push_back(tr);
            }
        if ((p = u64(at + 0x48)))
            for (uint16_t i = 0; i < nsubs; i++) t.subtimelines.push_back(ref(p + 8 * i));
        if ((p = u64(at + 0x50)))
            for (uint16_t i = 0; i < ncuts; i++) {
                uint64_t q = p + 0x18 * (uint64_t)i;
                EvflCut c;
                c.start_time = f32(q), c.x4 = u32(q + 4), c.name = ref(q + 8);
                c.has_params = opt_container(q + 0x10, c.params);
                t.cuts.push_back(c);
            }
        t.has_params = opt_container(at + 0x58, t.params);

        for (const EvflClip &c : t.clips)
            link(c.actor, t.actors.size(), false), link(c.action, t.actors[c.actor].actions.size(), false);
        for (const EvflOneshot &o : t.oneshots)
            link(o.actor, t.actors.size(), false), link(o.action, t.actors[o.actor].actions.size(), false);
        for (const EvflTrigger &tr : t.triggers) link(tr.clip, t.clips.size(), false);
        return t;
    }
};
}  // namespace

EventFlow EventFlow::parse(const std::string &data) {
    Reader r{data};
    Reader::need(data.size() >= 0x48 && !data.compare(0, 8, std::string("BFEVFL\0\0", 8)), "bad magic");
    Reader::need(r.u16(8) == 0x0300, "version isn't 3.0");
    Reader::need(r.le(0xA, 1) == 0, "bad header");
    Reader::need(r.u16(0xC) == 0xFEFF, "not little endian");
    EventFlow f;
    f.name = r.cstr(r.u32(0x10));
    uint16_t nfc = r.u16(0x20), ntl = r.u16(0x22);
    Reader::need(nfc <= 1 && ntl <= 1 && r.u32(0x24) == 0, "bad header");
    if (nfc) {
        uint64_t p = r.u64(r.u64(0x28));
        if (p) f.flowchart = r.flowchart(p), f.has_flowchart = true;
    }
    if (ntl) {
        uint64_t p = r.u64(r.u64(0x38));
        if (p) f.timeline = r.timeline(p), f.has_timeline = true;
    }
    return f;
}

// ---- writing
namespace {
const size_t NO_SLOT = ~(size_t)0;

// the bits of a name read as one big-endian number (bit 0: the last byte's lowest)
int key_bit(const std::string &k, long bit) {
    if (bit < 0 || (size_t)bit >= 8 * k.size()) return 0;
    return ((uint8_t)k[k.size() - 1 - bit / 8] >> (bit % 8)) & 1;
}
long key_bits(const std::string &k) {  // (the number's bit length)
    size_t i = 0;
    while (i < k.size() && !k[i]) i++;
    if (i == k.size()) return 0;
    long n = (long)(k.size() - 1 - i) * 8;
    for (uint8_t b = (uint8_t)k[i]; b; b >>= 1) n++;
    return n;
}
long key_mismatch(const std::string &a, const std::string &b) {  // first differing bit, -1: equal
    long n = std::max(key_bits(a), key_bits(b));
    for (long i = 0; i < n; i++)
        if (key_bit(a, i) != key_bit(b, i)) return i;
    return -1;
}

// Nintendo's dictionary: a radix tree over the names' bits; entry 0 is the root (no name, bit -1). Each insertion
// finds the closest existing name, then places the new node by the first bit where the two differ.
struct DicTree {
    struct Node {
        std::string key;
        long bit;
        int child[2], parent;
    };
    std::vector<Node> n;
    DicTree() { n.push_back(Node{"", -1, {0, 0}, 0}); }

    int closest(const std::string &k) const {
        if (n[0].child[0] == 0) return 0;
        int cur = n[0].child[0], prev;
        do {
            prev = cur;
            cur = n[cur].child[key_bit(k, n[cur].bit)];
        } while (n[cur].bit > n[prev].bit);
        return prev;
    }
    void insert(const std::string &k) {
        if (!key_bits(k)) throw std::runtime_error("bfevfl: empty dictionary name");
        for (const Node &x : n)
            if (x.key == k) throw std::runtime_error("bfevfl: dictionary name used twice: " + k);
        int cur = closest(k);
        long b = key_mismatch(n[cur].key, k);
        while (b < n[n[cur].parent].bit) cur = n[cur].parent;
        int id = (int)n.size();
        Node nw{k, b, {id, id}, cur};
        if (b < n[cur].bit) {  // above cur: take its place under its parent
            int par = n[cur].parent;
            nw.parent = par;
            nw.child[key_bit(k, b) ^ 1] = cur;
            n.push_back(nw);
            n[par].child[key_bit(k, n[par].bit)] = id;
            n[cur].parent = id;
        } else if (b > n[cur].bit) {  // below cur
            nw.child[key_bit(k, b) ^ 1] = key_bit(n[cur].key, b) == (key_bit(k, b) ^ 1) ? cur : 0;
            n.push_back(nw);
            n[cur].child[key_bit(k, n[cur].bit)] = id;
        } else {  // same bit as cur: hang below it on a higher bit (its other side's difference, or k's lowest 1)
            int other = n[cur].child[key_bit(k, b)];
            long nb = 0;
            while (!key_bit(k, nb)) nb++;
            if (other != 0) nb = key_mismatch(n[other].key, k);
            nw.bit = nb;
            nw.child[key_bit(k, nb) ^ 1] = other;
            n.push_back(nw);
            n[cur].child[key_bit(k, b)] = id;
        }
    }
};

// the string pool's order: by the string's bits from the last byte's lowest up to the highest set bit
std::string pool_key(const std::string &s) {
    std::string out;
    long nb = key_bits(s);
    for (long i = 0; i < nb; i++) out += (char)('0' + key_bit(s, i));
    return nb ? out : "0";
}

struct Writer {
    std::string buf;
    size_t pos = 0;
    std::set<size_t> ptrs;
    std::vector<std::string> str_order;
    std::unordered_map<std::string, std::vector<std::pair<size_t, bool>>> str_refs;  // (where, header's C string)

    Writer() { str_order.push_back(""), str_refs[""]; }

    void put(const void *p, size_t n) {
        if (pos + n > buf.size()) buf.resize(pos + n, '\0');
        if (n) memcpy(&buf[pos], p, n);
        pos += n;
    }
    void le(uint64_t v, int n) {
        char b[8];
        for (int i = 0; i < n; i++) b[i] = (char)(uint8_t)(v >> (8 * i));
        put(b, n);
    }
    void u8(uint64_t v) { le(v, 1); }
    void u16(uint64_t v) {
        if (v > 0xFFFF) throw std::runtime_error("bfevfl: value doesn't fit 16 bits");
        le(v, 2);
    }
    void u32(uint64_t v) { le(v, 4); }
    void u64(uint64_t v) { le(v, 8); }
    void f32(float f) {
        uint32_t u;
        memcpy(&u, &f, 4);
        le(u, 4);
    }
    void align(size_t a) { pos = (pos + a - 1) / a * a; }
    void patch(size_t at, uint64_t v, int n = 8) {
        size_t keep = pos;
        pos = at;
        le(v, n);
        pos = keep;
    }
    void here(size_t slot) { patch(slot, pos); }  // (a slot points here)

    size_t slot() {  // a pointer filled in later (relocated)
        size_t at = pos;
        ptrs.insert(at);
        u64(~0ull);
        return at;
    }
    // a pointer that's null when absent; some null pointers are listed for relocation all the same
    size_t opt_slot(bool present, bool reloc_null) {
        if (present) return slot();
        if (reloc_null) ptrs.insert(pos);
        u64(0);
        return NO_SLOT;
    }
    void ref_to(const std::string &s, bool header) {
        auto it = str_refs.find(s);
        if (it == str_refs.end()) str_order.push_back(s), it = str_refs.emplace(s, decltype(it->second)()).first;
        it->second.emplace_back(pos, header);
    }
    void str(const std::string &s) {
        ref_to(s, false);
        ptrs.insert(pos);
        u64(~0ull);
    }
    void header_str(const std::string &s) {
        ref_to(s, true);
        u32(0xFFFFFFFF);
    }
    void pascal(const std::string &s) {
        u16(s.size());
        put(s.data(), s.size());
        u8(0);
    }

    void dic(const std::vector<std::string> &keys) {
        DicTree t;
        for (const std::string &k : keys) t.insert(k);
        put("DIC ", 4);
        u32(t.n.size() - 1);
        for (const DicTree::Node &x : t.n) {
            u32((uint32_t)x.bit), u16(x.child[0]), u16(x.child[1]);
            str(x.key);
        }
    }

    void item(const EvflValue &v) {
        auto head = [&](uint8_t type, size_t count) { u8(type), u8(0), u16(count), u32(0), u64(0); };
        switch (v.type) {
        case EvflValue::Bool: head(v.type, 1), u32(v.b ? 0x80000001u : 0); break;
        case EvflValue::Int: head(v.type, 1), u32((uint32_t)v.i); break;
        case EvflValue::Float: head(v.type, 1), f32(v.f); break;
        case EvflValue::String: case EvflValue::Argument:  // (stored in place, not pooled)
            head(v.type, 1), here(slot()), pascal(v.s);
            break;
        case EvflValue::ActorIdentifier: {
            head(v.type, 2);
            size_t a = slot(), b = slot();
            here(a), pascal(v.actor.name), align(2);
            here(b), pascal(v.actor.sub_name);
            break;
        }
        case EvflValue::IntArray: case EvflValue::BoolArray: {  // (bools as the ints 0 / 1)
            size_t n = v.type == EvflValue::IntArray ? v.ints.size() : v.bools.size();
            if (!n) throw std::runtime_error("bfevfl: empty array");
            head(EvflValue::IntArray, n);
            for (size_t k = 0; k < n; k++)
                u32(v.type == EvflValue::IntArray ? (uint32_t)v.ints[k] : (v.bools[k] ? 1 : 0));
            break;
        }
        case EvflValue::FloatArray:
            if (v.floats.empty()) throw std::runtime_error("bfevfl: empty array");
            head(v.type, v.floats.size());
            for (float x : v.floats) f32(x);
            break;
        case EvflValue::StringArray: {
            if (v.strs.empty()) throw std::runtime_error("bfevfl: empty array");
            head(v.type, v.strs.size());
            std::vector<size_t> slots;
            for (size_t k = 0; k < v.strs.size(); k++) slots.push_back(slot());
            for (size_t k = 0; k < v.strs.size(); k++) align(8), here(slots[k]), pascal(v.strs[k]);
            break;
        }
        default: throw std::runtime_error("bfevfl: unknown value type");
        }
    }
    void container(const EvflContainer &c) {
        u8(1), u8(0), u16(c.items.size()), u32(0);
        size_t dslot = slot();
        std::vector<size_t> slots;
        std::vector<std::string> keys;
        for (const auto &kv : c.items) slots.push_back(slot()), keys.push_back(kv.first);
        here(dslot), dic(keys);
        for (size_t i = 0; i < c.items.size(); i++) align(8), here(slots[i]), item(c.items[i].second);
    }

    // actors: the record and its data (parameters, actions, queries) can come in either order; whichever comes
    // second fills in the pointers
    struct ActorAt {
        size_t act = NO_SLOT, q = NO_SLOT, par = NO_SLOT;  // (the record's slots)
        size_t act_at = 0, q_at = 0, par_at = 0;           // (where the data went)
    };
    void actor_record(const EvflActor &a, ActorAt &at) {
        str(a.id.name), str(a.id.sub_name), str(a.argument_name);
        at.act = opt_slot(!a.actions.empty(), true);
        at.q = opt_slot(!a.queries.empty(), true);
        at.par = opt_slot(a.has_params, false);
        u16(a.actions.size()), u16(a.queries.size()), u16(a.argument_entry_point), u16(a.concurrent_clips);
        if (at.act_at) patch(at.act, at.act_at);
        if (at.q_at) patch(at.q, at.q_at);
        if (at.par_at) patch(at.par, at.par_at);
    }
    void actor_data(const EvflActor &a, ActorAt &at) {
        auto block = [&](size_t s, size_t &where) {
            align(8);
            if (s != NO_SLOT) here(s);
            else where = pos;
        };
        if (a.has_params) block(at.par, at.par_at), container(a.params);
        if (!a.actions.empty()) {
            block(at.act, at.act_at);
            for (const std::string &s : a.actions) str(s);
        }
        if (!a.queries.empty()) {
            block(at.q, at.q_at);
            for (const std::string &s : a.queries) str(s);
        }
    }

    void finish(size_t first_block_slot, size_t first_block, size_t reloc_slot, size_t size_slot) {
        if (first_block > 0xFFFF) throw std::runtime_error("bfevfl: first block past 64 KiB");
        patch(first_block_slot, first_block, 2);
        align(8);
        // string pool
        std::vector<std::string> order = str_order;
        std::unordered_map<std::string, std::string> key_of;
        for (const std::string &s : order) key_of[s] = pool_key(s);
        std::stable_sort(order.begin(), order.end(),
                         [&](const std::string &a, const std::string &b) { return key_of[a] < key_of[b]; });
        put("STR ", 4), u32(0), u64(0), u32(order.size() - 1);
        for (const std::string &s : order) {
            size_t at = pos;
            for (const auto &r : str_refs[s]) patch(r.first, r.second ? at + 2 : at, r.second ? 4 : 8);
            pascal(s);
            align(2);
        }
        size_t data_end = pos;
        // relocation table: one section; each entry a pointer and a mask of which of the 32 pointers from it
        // (8 bytes apart) are pointers too
        align(8);
        size_t relt = pos;
        put("RELT", 4), u32(relt), u32(1), u32(0);
        u64(0), u32(0), u32(data_end), u32(0);
        size_t count_at = pos;
        u32(0);
        std::set<size_t> left = ptrs;
        uint32_t entries = 0;
        for (size_t p : ptrs) {
            if (!left.count(p)) continue;
            uint32_t mask = 0;
            for (int i = 0; i < 32; i++)
                if (left.erase(p + 8 * (size_t)i)) mask |= 1u << i;
            u32(p), u32(mask);
            entries++;
        }
        patch(count_at, entries, 4);
        patch(size_slot, pos, 4);
        patch(reloc_slot, relt, 4);
    }
};

int first_named(const std::vector<std::string> &l, uint16_t idx) {  // (the first entry with idx's name)
    if (idx >= l.size()) throw std::runtime_error("bfevfl: action / query index out of range");
    return index_of(l, l[idx]);
}

void check(uint16_t idx, size_t n, bool optional) {
    if (!((optional && idx == EVFL_NONE) || idx < n)) throw std::runtime_error("bfevfl: index out of range");
}

// the sub flow events an entry point reaches: each straight run walked breadth first, switch cases and forks
// walked in full (in order) as they're met, a fork's join queued after them
void sub_flows(const EvflFlowchart &f, uint16_t start, std::vector<bool> &seen, std::vector<uint16_t> &out) {
    if (start == EVFL_NONE) return;
    std::vector<uint16_t> queue{start};
    for (size_t qi = 0; qi < queue.size(); qi++) {
        uint16_t i = queue[qi];
        if (seen[i]) continue;
        seen[i] = true;
        const EvflEvent &e = f.events[i];
        switch (e.type) {
        case EvflEvent::Switch:
            for (const auto &c : unique_cases(e.cases)) sub_flows(f, c.second, seen, out);
            break;
        case EvflEvent::Fork:
            for (uint16_t x : e.forks) sub_flows(f, x, seen, out);
            queue.push_back(e.join);
            break;
        case EvflEvent::SubFlow:
            if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
            if (e.next != EVFL_NONE) queue.push_back(e.next);
            break;
        default:
            if (e.next != EVFL_NONE) queue.push_back(e.next);
            break;
        }
    }
}

void write_flowchart(Writer &w, const EvflFlowchart &f) {
    size_t na = f.actors.size(), ne = f.events.size(), nep = f.entry_points.size();
    size_t nactions = 0, nqueries = 0;
    for (const EvflActor &a : f.actors) {
        nactions += a.actions.size(), nqueries += a.queries.size();
        check(a.argument_entry_point, nep, true);
    }
    for (const EvflEvent &e : f.events) {
        switch (e.type) {
        case EvflEvent::Action: case EvflEvent::Switch: check(e.actor, na, false); break;
        case EvflEvent::Fork:
            check(e.join, ne, false);
            if (e.forks.empty()) throw std::runtime_error("bfevfl: fork event without forks");
            for (uint16_t x : e.forks) check(x, ne, false);
            break;
        case EvflEvent::SubFlow:
            if (e.entry_point.empty()) throw std::runtime_error("bfevfl: sub flow event without an entry point");
            break;
        default: break;
        }
        if (e.type != EvflEvent::Fork && e.type != EvflEvent::Switch) check(e.next, ne, true);
        for (const auto &c : e.cases) check(c.second, ne, false);
    }
    for (const EvflEntryPoint &ep : f.entry_points) check(ep.main_event, ne, true);

    size_t self = w.pos;
    w.put("EVFL", 4);
    size_t pool_slot = w.pos;
    w.u32(0xFFFFFFFF), w.u32(0), w.u32(0);
    w.u16(na), w.u16(nactions), w.u16(nqueries), w.u16(ne), w.u16(nep), w.u16(0), w.u16(0), w.u16(0);
    w.str(f.name);
    size_t actors_slot = w.opt_slot(na > 0, true), events_slot = w.opt_slot(ne > 0, true);
    size_t dic_slot = w.slot(), eps_slot = w.opt_slot(nep > 0, true);

    std::vector<Writer::ActorAt> aat(na);
    if (na) w.here(actors_slot);
    for (size_t i = 0; i < na; i++) w.actor_record(f.actors[i], aat[i]);

    struct EventAt { size_t params = NO_SLOT, extra = NO_SLOT; };
    std::vector<EventAt> eat(ne);
    if (ne) w.here(events_slot);
    for (size_t i = 0; i < ne; i++) {
        const EvflEvent &e = f.events[i];
        w.str(e.name);
        w.u8(e.type), w.u8(0);
        switch (e.type) {
        case EvflEvent::Action:
            w.u16(e.next), w.u16(e.actor), w.u16(first_named(f.actors[e.actor].actions, e.action));
            eat[i].params = w.opt_slot(!e.params.empty(), false);
            w.u64(0), w.u64(0);
            break;
        case EvflEvent::Switch: {
            size_t nc = unique_cases(e.cases).size();
            w.u16(nc), w.u16(e.actor), w.u16(first_named(f.actors[e.actor].queries, e.action));
            eat[i].params = w.opt_slot(!e.params.empty(), false);
            eat[i].extra = w.opt_slot(nc > 0, true);
            w.u64(0);
            break;
        }
        case EvflEvent::Fork:
            w.u16(e.forks.size()), w.u16(e.join), w.u16(0);
            eat[i].extra = w.slot();
            w.u64(0), w.u64(0);
            break;
        case EvflEvent::Join: w.u16(e.next), w.u16(0), w.u16(0), w.u64(0), w.u64(0), w.u64(0); break;
        case EvflEvent::SubFlow:
            w.u16(e.next), w.u16(0), w.u16(0);
            eat[i].params = w.opt_slot(!e.params.empty(), false);
            w.str(e.flowchart), w.str(e.entry_point);
            break;
        default: throw std::runtime_error("bfevfl: unknown event type");
        }
    }

    std::vector<std::string> names;
    for (const EvflEntryPoint &ep : f.entry_points) names.push_back(ep.name);
    w.here(dic_slot), w.dic(names);
    w.align(8);

    std::vector<std::vector<uint16_t>> subs(nep);
    std::vector<size_t> sub_slots(nep, NO_SLOT);
    if (nep) w.here(eps_slot);
    for (size_t i = 0; i < nep; i++) {
        std::vector<bool> seen(ne, false);
        sub_flows(f, f.entry_points[i].main_event, seen, subs[i]);
        sub_slots[i] = w.opt_slot(!subs[i].empty(), true);
        w.u64(0);
        w.ptrs.insert(w.pos), w.u64(0);
        w.u16(subs[i].size()), w.u16(0), w.u16(f.entry_points[i].main_event), w.u16(0);
    }

    for (size_t i = 0; i < ne; i++) {
        const EvflEvent &e = f.events[i];
        w.align(8);
        if (e.type == EvflEvent::Switch && eat[i].extra != NO_SLOT) {
            w.align(8), w.here(eat[i].extra);
            for (const auto &c : unique_cases(e.cases)) w.u32(c.first), w.u16(c.second), w.align(8);
        }
        if (e.type == EvflEvent::Fork) {
            w.here(eat[i].extra);
            for (uint16_t x : e.forks) w.u16(x);
            w.align(8);
        }
        if (eat[i].params != NO_SLOT) w.here(eat[i].params), w.container(e.params);
    }
    for (size_t i = 0; i < na; i++) w.align(8), w.actor_data(f.actors[i], aat[i]);
    for (size_t i = 0; i < nep; i++) {
        w.align(8);
        if (sub_slots[i] != NO_SLOT) {
            w.here(sub_slots[i]);
            for (uint16_t x : subs[i]) w.u16(x);
            w.align(8);
        }
        w.pos += 0x18;  // (left empty)
    }
    w.align(8);
    w.patch(pool_slot, w.pos - self, 4);
}

// returns where the timeline's header went
size_t write_timeline(Writer &w, const EvflTimeline &t) {
    size_t na = t.actors.size(), nactions = 0;
    for (const EvflActor &a : t.actors) nactions += a.actions.size();
    for (const EvflClip &c : t.clips) check(c.actor, na, false);
    for (const EvflOneshot &o : t.oneshots) check(o.actor, na, false);
    for (const EvflTrigger &tr : t.triggers) check(tr.clip, t.clips.size(), false);

    // triggers in the order they fire (stable)
    std::vector<EvflTrigger> triggers = t.triggers;
    auto when = [&](const EvflTrigger &tr) {
        const EvflClip &c = t.clips[tr.clip];
        if (tr.type == EvflTrigger::Enter) return (double)c.start_time;
        if (tr.type == EvflTrigger::Leave) return (double)c.start_time + (double)c.duration;
        return 0.0;
    };
    std::stable_sort(triggers.begin(), triggers.end(),
                     [&](const EvflTrigger &a, const EvflTrigger &b) { return when(a) < when(b); });

    std::vector<Writer::ActorAt> aat(na);
    for (size_t i = 0; i < na; i++) w.actor_data(t.actors[i], aat[i]), w.align(8);
    size_t params_at = 0;
    if (t.has_params) params_at = w.pos, w.container(t.params);

    w.align(8);
    size_t self = w.pos;
    w.put("TLIN", 4);
    size_t pool_slot = w.pos;
    w.u32(0xFFFFFFFF), w.u32(0), w.u32(0);
    w.f32(t.duration);
    w.u16(na), w.u16(nactions), w.u16(t.clips.size()), w.u16(t.oneshots.size()), w.u16(t.subtimelines.size()),
        w.u16(t.cuts.size());
    w.str(t.name);
    size_t s_actors = w.opt_slot(na > 0, true), s_clips = w.opt_slot(!t.clips.empty(), true);
    size_t s_ones = w.opt_slot(!t.oneshots.empty(), true), s_trig = w.opt_slot(!triggers.empty(), true);
    size_t s_subs = w.opt_slot(!t.subtimelines.empty(), true), s_cuts = w.opt_slot(!t.cuts.empty(), true);
    if (params_at) w.ptrs.insert(w.pos), w.u64(params_at);  // (no field at all without parameters)

    std::vector<size_t> clip_par(t.clips.size()), one_par(t.oneshots.size()), cut_par(t.cuts.size());
    if (na) {
        w.here(s_actors);
        for (size_t i = 0; i < na; i++) w.actor_record(t.actors[i], aat[i]);
        w.align(8);
    }
    if (!t.clips.empty()) {
        w.here(s_clips);
        for (size_t i = 0; i < t.clips.size(); i++) {
            const EvflClip &c = t.clips[i];
            w.f32(c.start_time), w.f32(c.duration), w.u16(c.actor);
            w.u16(first_named(t.actors[c.actor].actions, c.action));
            w.u8(c.actor_concurrent_clip), w.u8(0), w.u8(0), w.u8(0);
            clip_par[i] = w.opt_slot(c.has_params, false);
        }
        w.align(8);
    }
    if (!t.oneshots.empty()) {
        w.here(s_ones);
        for (size_t i = 0; i < t.oneshots.size(); i++) {
            const EvflOneshot &o = t.oneshots[i];
            w.f32(o.time), w.u16(o.actor), w.u16(first_named(t.actors[o.actor].actions, o.action)), w.u64(0);
            one_par[i] = w.opt_slot(o.has_params, false);
        }
        w.align(8);
    }
    if (!t.subtimelines.empty()) {
        w.here(s_subs);
        for (const std::string &s : t.subtimelines) w.str(s);
        w.align(8);
    }
    if (!triggers.empty()) {
        w.here(s_trig);
        for (const EvflTrigger &tr : triggers) w.u16(tr.clip), w.u8(tr.type), w.u8(0);
        w.align(8);
    }
    if (!t.cuts.empty()) {
        w.here(s_cuts);
        for (size_t i = 0; i < t.cuts.size(); i++) {
            const EvflCut &c = t.cuts[i];
            w.f32(c.start_time), w.u32(c.x4), w.str(c.name);
            cut_par[i] = w.opt_slot(c.has_params, false);
        }
        w.align(8);
    }
    for (size_t i = 0; i < t.clips.size(); i++) {
        if (clip_par[i] != NO_SLOT) w.here(clip_par[i]), w.container(t.clips[i].params);
        w.align(8);
    }
    for (size_t i = 0; i < t.oneshots.size(); i++) {
        if (one_par[i] != NO_SLOT) w.here(one_par[i]), w.container(t.oneshots[i].params);
        w.align(8);
    }
    for (size_t i = 0; i < t.cuts.size(); i++) {
        if (cut_par[i] != NO_SLOT) w.here(cut_par[i]), w.container(t.cuts[i].params);
        w.align(8);
    }
    w.align(8);
    w.patch(pool_slot, w.pos - self, 4);
    return self;
}
}  // namespace

std::string EventFlow::write() const {
    if (has_flowchart == has_timeline) return "";
    Writer w;
    w.put("BFEVFL\0\0", 8);
    w.u16(0x0300), w.u8(0), w.u8(0), w.u16(0xFEFF), w.u8(3), w.u8(0);
    w.header_str(name);
    w.u16(0);
    size_t first_slot = w.pos;
    w.u16(0xFFFF);
    size_t reloc_slot = w.pos;
    w.u32(0xFFFFFFFF);
    size_t size_slot = w.pos;
    w.u32(0xFFFFFFFF);
    w.u16(has_flowchart), w.u16(has_timeline), w.u32(0);

    size_t fc_arr = w.opt_slot(has_flowchart, true), fc_dic = w.slot();
    size_t tl_arr = w.opt_slot(has_timeline, true), tl_dic = w.slot();
    size_t fc_ptr = NO_SLOT, tl_ptr = NO_SLOT;
    if (has_flowchart) w.here(fc_arr), fc_ptr = w.slot();
    w.here(fc_dic), w.dic(has_flowchart ? std::vector<std::string>{flowchart.name} : std::vector<std::string>{});
    if (has_timeline) w.here(tl_arr), tl_ptr = w.slot();
    w.here(tl_dic), w.dic(has_timeline ? std::vector<std::string>{timeline.name} : std::vector<std::string>{});

    size_t first = 0;
    if (has_flowchart) {
        first = w.pos;
        w.here(fc_ptr);
        write_flowchart(w, flowchart);
    } else {
        first = write_timeline(w, timeline);
        w.patch(tl_ptr, first);
    }
    w.finish(first_slot, first, reloc_slot, size_slot);
    return w.buf;
}

}  // namespace rando
