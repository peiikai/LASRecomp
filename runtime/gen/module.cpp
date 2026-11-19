// Generate C for a whole NSO module (port of tools/gen.py).
#include "emu.hpp"
#include "gen_internal.hpp"
#include "lasgen.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace lasgen {

static int64_t sext26(uint32_t w) {
    int64_t v = w & 0x03FFFFFF;
    return v & 0x02000000 ? v - 0x04000000 : v;
}

static std::string fname(uint64_t a) { return "f_" + hexs(a); }

static std::string sanitize(const std::string &sym) {
    std::string s = "hle_";
    for (char ch : sym) s += (isalnum((unsigned char)ch) || ch == '_') ? ch : '_';
    return s;
}

static bool is_ident(char ch) { return isalnum((unsigned char)ch) || ch == '_'; }

// gen.GB_USE: re.compile(r'\bGB\b')
static bool uses_gb(const std::string &l) {
    for (size_t p = l.find("GB"); p != std::string::npos; p = l.find("GB", p + 1))
        if ((p == 0 || !is_ident(l[p - 1])) && (p + 2 >= l.size() || !is_ident(l[p + 2]))) return true;
    return false;
}

// gen.case_key / switch_on / rebase_patch (--dyn-base: jump table and dispatch switches compare window offsets, so
// their case labels stay constants; the hand-written patches' code addresses move with the window like the lifted
// ones)
static std::string case_key(uint64_t t) { return DYN_BASE ? hx(t - CANON) : hx(t); }
static std::string switch_on(const std::string &expr) { return DYN_BASE ? expr + " - GB" : expr; }

static std::string rebase_patch(const std::string &code) {
    if (!DYN_BASE) return code;
    // re.sub(r'\b0x(7[0-3][0-9a-f]{8})ull\b', gaddr(...), code)
    std::string out;
    size_t i = 0;
    auto hexd = [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); };
    while (i < code.size()) {
        if (code.compare(i, 3, "0x7") == 0 && (i == 0 || !is_ident(code[i - 1])) && i + 15 <= code.size() &&
            code[i + 3] >= '0' && code[i + 3] <= '3') {
            bool ok = true;
            for (size_t k = i + 4; k < i + 12; k++) ok = ok && hexd(code[k]);
            if (ok && code.compare(i + 12, 3, "ull") == 0 && (i + 15 == code.size() || !is_ident(code[i + 15]))) {
                out += gaddr(std::stoull(code.substr(i + 2, 10), nullptr, 16));
                i += 15;
                continue;
            }
        }
        out += code[i++];
    }
    return out;
}

// ---------------------------------------------------------------- the module

struct Table {
    bool all = false;               // 'all': any instruction of the function
    std::vector<uint64_t> targets;  // resolved targets, sorted
};

struct Scan {
    std::set<uint64_t> labels, ext;
    std::map<uint64_t, Table> tables;
};

using Counter = std::vector<std::pair<std::string, int>>;  // (insertion order, as Python's Counter prints)

struct Module::Impl {
    std::vector<std::unique_ptr<Insn>> cache;
    Counter stats;
    std::unique_ptr<Emu> mu;
    std::set<uint64_t> refs;  // gen.Module._refs
    void count(const std::string &k) {
        for (auto &e : stats)
            if (e.first == k) {
                e.second++;
                return;
            }
        stats.push_back({k, 1});
    }
};

Module::Module(const std::string &name_, const std::string &path, uint64_t base_, const std::string &simd)
    : name(name_), nso(path), base(base_), lifter(base_, simd), impl(std::make_unique<Impl>()) {
    lo = base + nso.text.first;
    hi = base + nso.text.first + nso.text.second;
    words.resize(nso.text.second / 4);
    memcpy(words.data(), &nso.image[nso.text.first], words.size() * 4);
    impl->cache.resize(words.size());
}

Module::~Module() = default;

const Insn &Module::lift(uint64_t a) {
    auto &slot = impl->cache[(a - lo) / 4];
    if (!slot) {
        try {
            slot = std::make_unique<Insn>(lifter.lift(word(a), a));
        } catch (const Unsupported &e) {
            impl->count("unsupported");
            slot = std::make_unique<Insn>(Insn::trap("rt_udf(c, " + gaddr(a) + "); /* " + e.what() + " */"));
        }
    }
    return *slot;
}

// The generator proper: the parts of gen.Module that only module.cpp uses.
class Gen {
  public:
    Module &m;
    const PatchSet &ps;
    const std::map<uint64_t, std::vector<uint32_t>> &rando_code;
    Gen(Module &m_, const PatchSet &ps_, const std::map<uint64_t, std::vector<uint32_t>> &rc)
        : m(m_), ps(ps_), rando_code(rc) {}

    std::vector<uint64_t> boundaries();
    Scan scan(uint64_t entry, uint64_t end);
    std::vector<std::string> emit_function(uint64_t entry, uint64_t end, const std::set<uint64_t> &labels,
                                           const std::map<uint64_t, Table> &tables,
                                           const std::map<uint64_t, std::string> &hle);

  private:
    bool lift_kind_in(uint64_t a, std::initializer_list<Kind> ks) {
        Kind k = m.lift(a).kind;
        for (Kind x : ks)
            if (k == x) return true;
        return false;
    }
    Emu &emu();
    bool jump_table(uint64_t br_pc, uint64_t entry, uint64_t end, std::vector<uint64_t> &out);
    std::vector<int> index_regs(uint64_t start, uint64_t at, int idx);
    bool emulate_table(uint64_t br_pc, uint64_t entry, uint64_t end, uint64_t start, int idx, int count,
                       uint64_t set_at, std::vector<uint64_t> &out);
    bool jump_table_taken(uint64_t br_pc, uint64_t entry, uint64_t end, std::vector<uint64_t> &out);
    bool jump_table_fallthrough(uint64_t br_pc, uint64_t entry, uint64_t end, std::vector<uint64_t> &out);
    bool computed_target(uint64_t br_pc, uint64_t entry);
    std::vector<std::string> render_single(const Insn &ins, uint64_t a,
                                           const std::function<std::string(uint64_t)> &jump);
};

std::vector<uint64_t> Gen::boundaries() {
    uint64_t b = m.base;
    std::set<uint64_t> starts;
    for (int64_t a : eh_frame_functions(m.nso)) starts.insert(b + (uint64_t)a);
    for (auto &s : m.nso.syms)
        if (s.shndx && (s.info & 0xF) == 2) starts.insert(b + s.value);
    for (auto &r : m.nso.relocs)
        if (r.type == R_AARCH64_RELATIVE) starts.insert(b + (uint64_t)r.add);
    std::vector<uint64_t> out;
    for (uint64_t a : starts)
        if (m.in_text(a)) out.push_back(a);
    return out;
}

// ------------------------------------------------------------ jump tables

Emu &Gen::emu() {
    if (!m.impl->mu) m.impl->mu = std::make_unique<Emu>(m.nso, m.base);
    return *m.impl->mu;
}

// Resolve `br xN` targets from a bounds-checked jump table; false if not one.
bool Gen::jump_table(uint64_t br_pc, uint64_t entry, uint64_t end, std::vector<uint64_t> &out) {
    if (jump_table_fallthrough(br_pc, entry, end, out)) return true;
    return jump_table_taken(br_pc, entry, end, out);
}

// The index register plus registers holding the same value in start..at: sources it was copied/masked from, and
// copies of those (the table lookup sometimes recomputes the index from another copy after the bounds check).
std::vector<int> Gen::index_regs(uint64_t start, uint64_t at, int idx) {
    auto copy_src = [](uint32_t w) -> int {
        if ((w & 0x7F800000) == 0x12000000) return (int)bits(w, 5, 5);   // AND (imm)
        if ((w & 0x7FE0FFE0) == 0x2A0003E0) return (int)bits(w, 16, 5);  // MOV (ORR with zr)
        if ((w & 0x7F800000) == 0x53000000 || (w & 0x7F800000) == 0x13000000)
            return (int)bits(w, 5, 5);  // UBFM/SBFM (uxtb, uxth, sxtw...)
        return -1;
    };
    std::set<int> regs = {idx};
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint64_t a = start; a < at; a += 4) {
            uint32_t w = m.word(a);
            int rd = (int)(w & 31), src = copy_src(w);
            if (src < 0 || src >= 31 || rd >= 31) continue;
            for (auto [x, y] : {std::pair<int, int>{rd, src}, {src, rd}})
                if (regs.count(x) && !regs.count(y)) {
                    regs.insert(y);
                    changed = true;
                }
        }
    }
    return {regs.begin(), regs.end()};
}

// (gen.py _set_index: registers from 29 up are written as x29)
static void set_index(Emu &mu, const std::vector<int> &regs, uint64_t i) {
    for (int r : regs) mu.set_x(r < 29 ? r : 29, i);
}

// The br register's value (x30 for the zero register, as gen.py reads it); false if the evaluator could not know it
static bool read_target(Emu &mu, int rn, uint64_t &t) { return mu.get_x(rn < 29 ? rn : rn == 29 ? 29 : 30, t); }

// Reset the registers gen.py resets for each index value and replay address materialization (ADRP/ADR/ADD-imm)
// from the function entry, so table bases computed before loops or branches are known.
static void prime(Emu &mu, Module &m, uint64_t entry, uint64_t start) {
    for (int r = 0; r < 29; r++) mu.set_x(r, Emu::SCRATCH_MEM + 0x80000);
    mu.set_sp(Emu::SCRATCH_MEM + 0x80000);
    for (uint64_t a2 = entry; a2 < start; a2 += 4) {
        uint32_t w2 = m.word(a2);
        if ((w2 & 0x1F000000) == 0x10000000 || (w2 & 0xFF800000) == 0x91000000) mu.step(a2);
    }
}

// single-step (gen.py: Unicorn's `until` is unreliable mid translation-block)
static bool step_to(Emu &mu, uint64_t frm, uint64_t to) {
    uint64_t pc = frm;
    for (int k = 0; k < 64; k++) {
        if (pc == to) return true;
        mu.step(pc);
        pc = mu.pc;
    }
    return false;
}

static void sorted_unique(std::vector<uint64_t> &v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

// Run start..br for each index value (set at address set_at), collect targets.
bool Gen::emulate_table(uint64_t br_pc, uint64_t entry, uint64_t end, uint64_t start, int idx, int count,
                        uint64_t set_at, std::vector<uint64_t> &out) {
    Emu &mu = emu();
    int rn = (int)bits(m.word(br_pc), 5, 5);
    std::vector<uint64_t> targets;
    try {
        for (int i = 0; i < count; i++) {
            prime(mu, m, entry, start);
            if (set_at != start && !step_to(mu, start, set_at)) return false;
            set_index(mu, {idx}, (uint64_t)i);
            if (!step_to(mu, set_at, br_pc)) return false;
            uint64_t t;
            if (!read_target(mu, rn, t) || !(entry <= t && t < end) || t % 4) return false;
            targets.push_back(t);
        }
    } catch (const EmuFault &) {
        return false;
    }
    sorted_unique(targets);
    out = std::move(targets);
    return true;
}

// Form: cmp idx, #N ; b.ls/b.lo table ... table: <address math> ; br
bool Gen::jump_table_taken(uint64_t br_pc, uint64_t entry, uint64_t end, std::vector<uint64_t> &out) {
    // start of the block holding the br
    uint64_t s = br_pc;
    while (s >= entry + 4 && br_pc - (s - 4) <= 4 * 12 && lift_kind_in(s - 4, {Kind::op, Kind::asm_})) s -= 4;
    for (uint64_t g = entry; g < end; g += 4) {
        uint32_t w = m.word(g);
        if ((w & 0xFF000010) != 0x54000000 || (bits(w, 0, 4) != 9 && bits(w, 0, 4) != 3)) continue;
        uint64_t t = g + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
        if (t != s) continue;
        uint32_t cw = g >= entry + 4 ? m.word(g - 4) : 0;
        if ((cw & 0x7F80001F) != 0x7100001F || bits(cw, 22, 1)) continue;
        int limit = (int)bits(cw, 10, 12);
        int count = bits(w, 0, 4) == 9 ? limit + 1 : limit;
        if (count <= 0 || count > 4096) continue;
        return emulate_table(br_pc, entry, end, s, (int)bits(cw, 5, 5), count, s, out);
    }
    return false;
}

bool Gen::jump_table_fallthrough(uint64_t br_pc, uint64_t entry, uint64_t end, std::vector<uint64_t> &out) {
    int rn = (int)bits(m.word(br_pc), 5, 5);
    // find the guarding b.hi / b.hs and its cmp, scanning backwards
    uint64_t a = br_pc - 4;
    bool have_guard = false, have_cmp = false;
    uint64_t guard = 0, cmp = 0;
    while (a >= entry && br_pc - a <= 4 * 16) {
        uint32_t w = m.word(a);
        if ((w & 0xFF000010) == 0x54000000 && (bits(w, 0, 4) == 8 || bits(w, 0, 4) == 2)) {
            guard = a, have_guard = true;
            break;
        }
        if (lift_kind_in(a, {Kind::b, Kind::bl, Kind::blr, Kind::br, Kind::ret, Kind::trap})) return false;
        if (a < 4) break;
        a -= 4;
    }
    if (!have_guard) return false;
    a = guard - 4;
    while (a >= entry && guard - a <= 4 * 8) {
        uint32_t w = m.word(a);
        // SUBS (imm) with Rd = zr, i.e. CMP #imm
        if ((w & 0x7F80001F) == 0x7100001F) {
            cmp = a, have_cmp = true;
            break;
        }
        if (m.lift(a).kind != Kind::op) return false;
        a -= 4;
    }
    if (!have_cmp) return false;
    uint32_t w = m.word(cmp);
    if (bits(w, 22, 1)) return false;
    int limit = (int)bits(w, 10, 12);
    int count = bits(m.word(guard), 0, 4) == 8 ? limit + 1 : limit;
    if (count <= 0 || count > 4096) return false;
    int idx = (int)bits(w, 5, 5);
    // window start: back from cmp to the first control-flow instruction
    uint64_t start = cmp;
    while (start >= entry + 4 && cmp - (start - 4) <= 4 * 12 && lift_kind_in(start - 4, {Kind::op, Kind::asm_}))
        start -= 4;
    std::vector<int> idx_regs = index_regs(start, cmp, idx);
    Emu &mu = emu();
    std::vector<uint64_t> targets;
    try {
        for (int i = 0; i < count; i++) {
            prime(mu, m, entry, start);
            if (!step_to(mu, start, cmp)) return false;
            set_index(mu, idx_regs, (uint64_t)i);
            if (!step_to(mu, cmp, br_pc)) return false;
            uint64_t t;
            if (!read_target(mu, rn, t) || !(entry <= t && t < end) || t % 4) return false;
            targets.push_back(t);
        }
    } catch (const EmuFault &) {
        return false;
    }
    sorted_unique(targets);
    out = std::move(targets);
    return true;
}

// ------------------------------------------------------------ functions

// True if the br register comes from address arithmetic (a table) rather than a load.
bool Gen::computed_target(uint64_t br_pc, uint64_t entry) {
    uint32_t rn = bits(m.word(br_pc), 5, 5);
    uint64_t a = br_pc - 4;
    while (a >= entry && br_pc - a <= 4 * 16) {
        uint32_t w = m.word(a);
        if (!lift_kind_in(a, {Kind::op, Kind::asm_})) return false;
        if ((w & 31) == rn) {
            if ((w & 0x1F000000) == 0x0B000000) return true;  // ADD/SUB (shifted or extended register)
            if ((w & 0x9F000000) == 0x10000000) return true;  // ADR
            return false;
        }
        a -= 4;
    }
    return false;
}

// Collect labels and external entries referenced by a function.
Scan Gen::scan(uint64_t entry, uint64_t end) {
    Scan r;
    r.labels.insert(entry);
    bool terminated = false;
    for (uint64_t a = entry; a < end; a += 4) {
        const Insn &ins = m.lift(a);
        if (ins.kind == Kind::b || ins.kind == Kind::bcond) {
            uint64_t t = ins.target;
            if (entry <= t && t < end) r.labels.insert(t);
            else if (m.in_text(t)) r.ext.insert(t);
        } else if (ins.kind == Kind::bl) {
            if (m.in_text(ins.target)) r.ext.insert(ins.target);
        } else if (ins.kind == Kind::br) {
            std::vector<uint64_t> jt;
            if (jump_table(a, entry, end, jt) && !jt.empty()) {
                r.labels.insert(jt.begin(), jt.end());
                r.tables[a].targets = std::move(jt);
                m.impl->count("jumptables");
            } else if (computed_target(a, entry)) {
                // unresolved table jump: allow jumping to any instruction of the function
                r.tables[a].all = true;
                m.impl->count("br_all_labels");
            } else {
                m.impl->count("br_dispatch");
            }
        }
        terminated = ins.kind == Kind::b || ins.kind == Kind::br || ins.kind == Kind::ret || ins.kind == Kind::trap;
    }
    if (!terminated && end < m.hi) r.ext.insert(end);
    return r;
}

// C for one lifted instruction (no batching); used for patch variants.
std::vector<std::string> Gen::render_single(const Insn &ins, uint64_t a,
                                            const std::function<std::string(uint64_t)> &jump) {
    switch (ins.kind) {
        case Kind::asm_: return {emit_asm_batch({ins.asm_.get()})};
        case Kind::op:
        case Kind::trap: return {ins.code};
        case Kind::b: return {jump(ins.target)};
        case Kind::bcond: return {"if (" + ins.cond + ") { " + jump(ins.target) + " }"};
        case Kind::bl: {
            uint64_t t = ins.target;
            if (m.in_text(t)) {
                m.impl->refs.insert(t);
                return {"c->x[30] = " + gaddr(a + 4) + "; " + fname(t) + "(c);"};
            }
            return {"c->x[30] = " + gaddr(a + 4) + "; c->pc = " + gaddr(t) + "; rt_dispatch(c);"};
        }
        case Kind::blr: return {"c->pc = " + ins.target_text() + "; c->x[30] = " + gaddr(a + 4) + "; rt_dispatch(c);"};
        case Kind::br: return {"c->pc = " + ins.target_text() + "; __attribute__((musttail)) return rt_dispatch(c);"};
        case Kind::ret: return {"return;"};
    }
    throw Unsupported("kind");
}

std::vector<std::string> Gen::emit_function(uint64_t entry, uint64_t end, const std::set<uint64_t> &labels_in,
                                            const std::map<uint64_t, Table> &tables,
                                            const std::map<uint64_t, std::string> &hle) {
    std::vector<std::string> out = {"void " + fname(entry) + "(Cpu *c) {"};
    auto h = hle.find(entry);
    if (h != hle.end()) {
        out.push_back("    " + sanitize(h->second) + "(c);");
        out.push_back("}");
        return out;
    }
    out.push_back("    FN_ENTER(" + gaddr(entry) + ");");
    std::vector<const AsmInsn *> batch;
    auto &refs = m.impl->refs;

    auto flush = [&] {
        if (!batch.empty()) {
            out.push_back("    " + emit_asm_batch(batch));
            batch.clear();
        }
    };

    std::function<std::string(uint64_t)> jump = [&](uint64_t t) -> std::string {
        if (entry <= t && t < end) return "goto L_" + hexs(t) + ";";
        if (!m.in_text(t)) return "c->pc = " + gaddr(t) + "; __attribute__((musttail)) return rt_dispatch(c);";
        refs.insert(t);
        return "__attribute__((musttail)) return " + fname(t) + "(c);";
    };

    // Functions with jump tables also get the all-instruction fallback: a table whose emulation missed some cases then
    // still reaches the right instruction instead of aborting.
    bool all_labels = !tables.empty();
    std::set<uint64_t> labels_own;
    const std::set<uint64_t> *labels = &labels_in;
    auto own = [&] {
        if (labels != &labels_own) {
            labels_own = *labels;
            labels = &labels_own;
        }
    };
    if (all_labels) {
        labels_own.clear();
        for (uint64_t a = entry; a < end; a += 4) labels_own.insert(labels_own.end(), a);
        labels = &labels_own;
    }
    bool rando_here = false, hooks_here = false;
    for (uint64_t a = entry; a < end; a += 4) {
        rando_here = rando_here || rando_code.count(a - m.base);
        hooks_here = hooks_here || ps.hooks.count(a);
    }
    if (rando_here) {
        own();
        for (uint64_t a = entry; a < end; a += 4) {
            auto it = rando_code.find(a - m.base);
            if (it == rando_code.end()) continue;
            for (uint32_t w : it->second) {
                Insn v = m.lifter.lift(w, a);
                if ((v.kind == Kind::b || v.kind == Kind::bcond) && v.has_int_target && entry <= v.target &&
                    v.target < end)
                    labels_own.insert(v.target);
            }
        }
    }
    if (hooks_here) {  // (a hook's jump targets need their labels)
        own();
        for (uint64_t a = entry; a < end; a += 4) {
            auto it = ps.hooks.find(a);
            if (it == ps.hooks.end()) continue;
            for (uint64_t t : it->second.targets) {
                if (!(entry <= t && t < end)) {
                    char b[160];
                    snprintf(b, sizeof b, "hook %#llx: target %#llx is outside its function", (unsigned long long)a,
                             (unsigned long long)t);
                    throw GenError(b);
                }
                labels_own.insert(t);
            }
        }
    }
    Kind last = Kind::op;
    bool have_last = false;
    for (uint64_t a = entry; a < end; a += 4) {
        const Insn &ins = m.lift(a);
        if (labels->count(a)) {
            flush();
            out.push_back("L_" + hexs(a) + ":;");
        }
        auto rc = rando_code.find(a - m.base);
        if (rc != rando_code.end()) {  // randomizer patch site: the active seed picks the instruction
            flush();
            out.push_back("    { extern uint32_t rando_word(uint64_t); uint32_t rw = rando_word(" + gaddr(a) + ");");
            bool first = true;
            for (uint32_t w : rc->second) {
                Insn v = m.lifter.lift(w, a);
                out.push_back(std::string("    ") + (first ? "if" : "else if") + " (rw == " + hx(w) + ") { " +
                              join(render_single(v, a, jump), " ") + " }");
                first = false;
            }
            out.push_back("    else { " + join(render_single(ins, a, jump), " ") + " } }");
            last = Kind::op, have_last = true;
            continue;
        }
        auto hk = ps.hooks.find(a);
        if (hk != ps.hooks.end()) {  // hook site (patches HOOKS): the runtime decides, the instruction stays as it is
            flush();
            uint32_t w = m.image_word(a);
            if (w != hk->second.word) {
                char b[160];
                snprintf(b, sizeof b, "hook %#llx: instruction %#010x, expected %#010x", (unsigned long long)a, w,
                         hk->second.word);
                throw GenError(b);
            }
            std::string body = join(render_single(ins, a, jump), " ");
            std::vector<std::string> jumps;
            int k = 0;
            for (uint64_t t : hk->second.targets) jumps.push_back(cat("else if (h_ == ", k++ + 2, ") goto L_", hexs(t), ";"));
            char cm[64];
            snprintf(cm, sizeof cm, "%#llx", (unsigned long long)a);
            out.push_back("    { int h_ = rt_hook(c, " + gaddr(a) + "); if (h_ == 0) { " + body + " } " + join(jumps, " ") +
                          " }  /* " + cm + ": hook (runtime/hle/hooks.c) */");
            last = Kind::op, have_last = true;
            continue;
        }
        auto pt = ps.patches.find(a);
        if (pt != ps.patches.end()) {  // hand-written replacement for one instruction (patches.cpp)
            flush();
            out.push_back("    " + rebase_patch(pt->second(m, a)));
            last = Kind::op, have_last = true;
            continue;
        }
        Kind k = ins.kind;
        if (k == Kind::asm_) {
            if (batch.size() >= 24) flush();
            batch.push_back(ins.asm_.get());
            try {
                emit_asm_batch(batch);
            } catch (const Unsupported &) {
                batch.pop_back();
                flush();
                batch.push_back(ins.asm_.get());
            }
            last = k, have_last = true;
            continue;
        }
        flush();
        if (k == Kind::op || k == Kind::trap) {
            out.push_back("    " + ins.code);
        } else if (k == Kind::b) {
            out.push_back("    " + jump(ins.target));
        } else if (k == Kind::bcond) {
            out.push_back("    if (" + ins.cond + ") { " + jump(ins.target) + " }");
        } else if (k == Kind::bl) {
            uint64_t t = ins.target;
            if (m.in_text(t)) {
                refs.insert(t);
                out.push_back("    c->x[30] = " + gaddr(a + 4) + "; " + fname(t) + "(c);");
            } else {
                out.push_back("    c->x[30] = " + gaddr(a + 4) + "; c->pc = " + gaddr(t) + "; rt_dispatch(c);");
            }
        } else if (k == Kind::blr) {
            out.push_back("    c->pc = " + ins.target_text() + "; c->x[30] = " + gaddr(a + 4) + "; rt_dispatch(c);");
        } else if (k == Kind::br) {
            auto jt = tables.find(a);
            if (jt != tables.end() && jt->second.all) {
                out.push_back("    c->pc = " + ins.target_text() + "; goto L_dispatch;");
                continue;  // (gen.py: `last` stays as it was)
            }
            if (jt != tables.end()) {
                std::vector<std::string> cases;
                for (uint64_t t : jt->second.targets) cases.push_back("case " + case_key(t) + ": goto L_" + hexs(t) + ";");
                out.push_back("    switch (" + switch_on(ins.target_text()) + ") { " + join(cases, " ") + " default: break; }");
                out.push_back("    c->pc = " + ins.target_text() + "; goto L_dispatch;");
                continue;
            }
            out.push_back("    c->pc = " + ins.target_text() + "; __attribute__((musttail)) return rt_dispatch(c);");
        } else if (k == Kind::ret) {
            out.push_back("    return;");
        }
        last = k, have_last = true;
    }
    flush();
    bool terminal = have_last && (last == Kind::b || last == Kind::br || last == Kind::ret || last == Kind::trap);
    if (!terminal) out.push_back(end < m.hi ? "    " + jump(end) : "    rt_udf(c, " + gaddr(end) + ");");
    if (all_labels) {
        out.push_back("L_dispatch:");
        std::string cases;
        for (uint64_t t = entry; t < end; t += 4) {
            if (t != entry) cases += " ";
            cases += "case " + case_key(t) + ": goto L_" + hexs(t) + ";";
        }
        out.push_back("    switch (" + switch_on("c->pc") + ") { " + cases + " default: break; }");
        out.push_back("    __attribute__((musttail)) return rt_dispatch(c);");
    }
    out.push_back("}");
    if (DYN_BASE)
        for (auto &l : out)
            if (uses_gb(l)) {
                out.insert(out.begin() + 1, "    const uint64_t GB = lasgame_gb;");
                break;
            }
    return out;
}

// ---------------------------------------------------------------- the run (gen.main)

static void write_if_changed(const fs::path &path, const std::string &text) {
    // Leaves unchanged chunks untouched so make only recompiles what a patch affected.
    {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            if (ss.str() == text) return;
        }
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), (std::streamsize)text.size());
    if (!f) throw GenError("cannot write " + path.string());
}

struct SavedBounds {
    const char *key, *what;
    std::vector<uint64_t> offsets;
};
static const std::vector<SavedBounds> &saved_bounds() {
    static const std::vector<SavedBounds> b = {
#include "chunk_bounds.inc"
    };
    return b;
}

// patch_variants.json "code": {"0x6a62f8": ["0x34000240", ...], ...} (offsets from the module base)
static std::map<uint64_t, std::vector<uint32_t>> load_rando_code(const char *path) {
    std::map<uint64_t, std::vector<uint32_t>> out;
    if (!path) return out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;  // (gen.py: only when the file exists)
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    // A small reader for this file's shape: the "code" object of string keys and string arrays.
    size_t p = s.find("\"code\"");
    if (p == std::string::npos) throw GenError("patch_variants.json has no code");
    p = s.find('{', p);
    auto bad = [] { return GenError("patch_variants.json: unexpected format"); };
    if (p == std::string::npos) throw bad();
    auto skip = [&] {
        while (p < s.size() && isspace((unsigned char)s[p])) p++;
    };
    auto str = [&]() -> std::string {
        skip();
        if (p >= s.size() || s[p] != '"') throw bad();
        size_t e = s.find('"', p + 1);
        if (e == std::string::npos) throw bad();
        std::string v = s.substr(p + 1, e - p - 1);
        p = e + 1;
        return v;
    };
    p++;
    skip();
    if (p < s.size() && s[p] == '}') return out;
    for (;;) {
        uint64_t key = std::stoull(str(), nullptr, 16);
        skip();
        if (s[p++] != ':') throw bad();
        skip();
        if (s[p++] != '[') throw bad();
        std::vector<uint32_t> ws;
        skip();
        if (s[p] == ']') p++;
        else
            for (;;) {
                ws.push_back((uint32_t)std::stoull(str(), nullptr, 16));
                skip();
                char ch = s[p++];
                if (ch == ']') break;
                if (ch != ',') throw bad();
            }
        out[key] = std::move(ws);
        skip();
        char ch = s[p++];
        if (ch == '}') break;
        if (ch != ',') throw bad();
    }
    return out;
}

struct Run {
    const LasgenOptions &o;
    void log(const char *fmt, ...) {
        char b[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        if (o.log) o.log(o.ud, b);
        else fprintf(stderr, "%s\n", b), fflush(stderr);
    }
    void check_cancel() {
        if (o.cancel && *o.cancel) throw GenError("cancelled");
    }
};

static std::string stats_text(const Counter &c) {
    std::string s = "{";
    for (size_t i = 0; i < c.size(); i++) s += cat(i ? ", " : "", "'", c[i].first, "': ", c[i].second);
    return s + "}";
}

static void run(const LasgenOptions &o) {
    Run r{o};
    DYN_BASE = o.dyn_base;
    const std::string name = o.name;
    // (first: a generator that ends early still shows it started)
    r.log("%s: started", name.c_str());
    auto t0 = std::chrono::steady_clock::now();
    auto secs = [&] {
        return (long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
    };
    Module m(name, o.nso, o.base, o.simd ? o.simd : "asm");
    PatchSet ps;
    std::string version;
    if (name == "main") {
        version = version_for_build_id(m.nso.build_id);
        PATCH_ADDRESSES = &address_table(version);
        ps = build_patches(version);
        const GameVersion *gv = nullptr;
        for (int i = 0; i < GAME_VERSION_COUNT; i++)
            if (version == GAME_VERSIONS[i].key) gv = &GAME_VERSIONS[i];
        r.log("%s: game version %s (build %.16s), address table %s", name.c_str(), gv->label, gv->build_id,
              version.c_str());
    }
    auto rando_code = load_rando_code(o.patch_variants);
    Gen g(m, ps, rando_code);
    std::vector<uint64_t> starts = g.boundaries();
    std::map<uint64_t, std::string> hle;
    if (o.hle) {
        std::ifstream f(o.hle);
        if (f) {
            std::set<std::string> want;
            std::string l;
            while (std::getline(f, l)) {
                if (!l.empty() && l.back() == '\r') l.pop_back();
                size_t a = l.find_first_not_of(" \t\r\n\f\v"), b = l.find_last_not_of(" \t\r\n\f\v");
                if (a == std::string::npos || l[0] == '#') continue;
                want.insert(l.substr(a, b - a + 1));
            }
            for (auto &s : m.nso.syms)
                if (s.shndx && want.count(s.name)) hle[o.base + s.value] = s.name;
        }
    }

    // Partition text into functions; any branch/call target outside its function becomes a new boundary (splitting
    // the containing function) until a fixpoint is reached.
    std::set<uint64_t> start_set(starts.begin(), starts.end());
    for (size_t i = 0; i < m.words.size(); i++) {
        uint32_t w = m.words[i];
        if ((w & 0xFC000000) == 0x94000000) {
            uint64_t t = m.lo + i * 4 + (uint64_t)(sext26(w) * 4);
            if (m.in_text(t)) start_set.insert(t);
        }
    }
    // Code addresses materialized in registers (callbacks, vtables built at runtime): ADR, or ADRP followed shortly by
    // ADD on the same register.
    std::map<int, std::pair<uint64_t, size_t>> adrp_regs;
    for (size_t i = 0; i < m.words.size(); i++) {
        uint32_t w = m.words[i];
        uint64_t pc = m.lo + i * 4;
        if ((w & 0x9F000000) == 0x10000000) {  // ADR
            int64_t imm = (int64_t)((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3));
            if (imm & (1 << 20)) imm -= 1 << 21;
            if (m.in_text(pc + (uint64_t)imm)) start_set.insert(pc + (uint64_t)imm);
        } else if ((w & 0x9F000000) == 0x90000000) {  // ADRP
            int64_t imm = (int64_t)((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3));
            if (imm & (1 << 20)) imm -= 1 << 21;
            adrp_regs[(int)(w & 31)] = {(pc & ~0xFFFull) + (uint64_t)(imm * 4096), i};
        } else if ((w & 0xFFC00000) == 0x91000000) {  // ADD (imm, 64-bit, no shift)
            int rn = (int)((w >> 5) & 31);
            auto it = adrp_regs.find(rn);
            if (it != adrp_regs.end() && i - it->second.second <= 8) {
                uint64_t t = it->second.first + ((w >> 10) & 0xFFF);
                if (m.in_text(t)) start_set.insert(t);
            }
        }
    }
    std::map<std::pair<uint64_t, uint64_t>, Scan> scanned;
    int rounds = 0;
    std::vector<uint64_t> order;
    for (;;) {
        rounds++;
        order.assign(start_set.begin(), start_set.end());
        std::set<uint64_t> fresh;
        for (size_t i = 0; i < order.size(); i++) {
            uint64_t a = order[i], end = i + 1 < order.size() ? order[i + 1] : m.hi;
            auto key = std::make_pair(a, end);
            auto it = scanned.find(key);
            if (it == scanned.end()) {
                r.check_cancel();
                it = scanned.emplace(key, g.scan(a, end)).first;
            }
            for (uint64_t t : it->second.ext)
                if (!start_set.count(t)) fresh.insert(t);
        }
        if (fresh.empty()) break;
        start_set.insert(fresh.begin(), fresh.end());
    }
    struct Entry {
        uint64_t end;
        const Scan *scan;
    };
    std::map<uint64_t, Entry> entries;
    for (size_t i = 0; i < order.size(); i++) {
        uint64_t a = order[i], end = i + 1 < order.size() ? order[i + 1] : m.hi;
        entries[a] = {end, &scanned.at({a, end})};
    }
    r.log("%s: %zu boundaries, %zu functions, %d rounds, %s, %lds", name.c_str(), starts.size(), entries.size(), rounds,
          stats_text(m.impl->stats).c_str(), secs());

    fs::path outdir = fs::u8path(o.outdir);
    fs::create_directories(outdir);
    order.clear();
    for (auto &e : entries) order.push_back(e.first);
    if (o.limit > 0 && (size_t)o.limit < order.size()) order.resize((size_t)o.limit);

    struct Rendered {
        uint64_t a;
        std::vector<std::string> lines;
        std::set<uint64_t> refs;
    };
    auto render = [&](uint64_t a) {
        r.check_cancel();
        const Entry &e = entries.at(a);
        Rendered f{a, g.emit_function(a, e.end, e.scan->labels, e.scan->tables, hle), {}};
        f.refs.swap(m.impl->refs);
        return f;
    };

    std::string key = m.nso.build_id.substr(0, 16);
    const SavedBounds *saved = nullptr;
    if (!o.new_chunk_bounds)
        for (auto &b : saved_bounds())
            if (key == b.key) saved = &b;
    if (!saved && !o.new_chunk_bounds) r.log("%s: no saved file cuts for build %s, cutting by size", name.c_str(), key.c_str());

    int nchunks = 0;
    size_t done = 0;
    std::vector<int64_t> firsts;
    auto write_chunk = [&](std::vector<Rendered> &fns) {
        std::string text = "#include \"cpu.h\"\n";
        if (m.lifter.simd_c()) text += "#include \"simd.h\"\n";
        if (o.dyn_base) text += "extern __attribute__((visibility(\"hidden\"))) uint64_t lasgame_gb;\n";
        std::set<uint64_t> refs, here;
        for (auto &f : fns) refs.insert(f.refs.begin(), f.refs.end()), here.insert(f.a);
        for (uint64_t a : refs) text += "void " + fname(a) + "(Cpu *c);\n";
        for (auto &h : hle)
            if (here.count(h.first)) text += "void " + sanitize(h.second) + "(Cpu *c);\n";
        bool first = true;
        for (auto &f : fns)
            for (auto &l : f.lines) {
                if (!first) text += '\n';
                text += l;
                first = false;
            }
        text += '\n';
        firsts.push_back(fns.empty() ? -1 : (int64_t)fns[0].a);
        done += fns.size();
        char fn[64];
        snprintf(fn, sizeof fn, "%s_%03d.c", name.c_str(), nchunks);
        write_if_changed(outdir / fn, text);
        nchunks++;
        // (the start screen's setup reads these for its progress bar: runtime/gamebuild.c)
        r.log("%s: progress %zu/%zu files %d", name.c_str(), done, order.size(), nchunks);
    };

    if (saved) {
        // File k holds the functions from starts[k] up to starts[k + 1] (the first file also anything before
        // starts[0]): a changed function changes only its own file, whatever happens before it.
        std::vector<uint64_t> bstarts;
        for (uint64_t off : saved->offsets) bstarts.push_back(o.base + off);
        size_t k = 0;
        std::vector<Rendered> fns;
        for (uint64_t a : order) {
            size_t ub = (size_t)(std::upper_bound(bstarts.begin(), bstarts.end(), a) - bstarts.begin());
            size_t j = ub > 0 ? ub - 1 : 0;
            while (j > k) {
                write_chunk(fns);
                fns.clear();
                k++;
            }
            fns.push_back(render(a));
        }
        while (k < bstarts.size()) {
            write_chunk(fns);
            fns.clear();
            k++;
        }
    } else {
        // Files of at most `chunk` functions and about `chunk_bytes` of C; a big function gets a file of its own (they
        // compile in parallel).
        size_t ci = 0;
        std::unique_ptr<Rendered> pending;
        while (ci < order.size() || pending) {
            std::vector<Rendered> fns;
            size_t size = 0;
            while ((pending || ci < order.size()) && fns.size() < (size_t)o.chunk && size < (size_t)o.chunk_bytes) {
                Rendered f = pending ? std::move(*pending) : render(order[ci]);
                if (pending) pending.reset();
                else ci++;
                size_t n = 0;
                for (auto &l : f.lines) n += l.size() + 1;
                if (n > (size_t)o.big_fn && !fns.empty()) {
                    pending = std::make_unique<Rendered>(std::move(f));
                    break;
                }
                fns.push_back(std::move(f));
                size += n;
                if (n > (size_t)o.big_fn) break;
            }
            write_chunk(fns);
        }
    }
    if (o.new_chunk_bounds) {
        std::string what = name;
        if (name == "main")
            for (int i = 0; i < GAME_VERSION_COUNT; i++)
                if (version == GAME_VERSIONS[i].key) what += std::string(" ") + GAME_VERSIONS[i].label;
        std::vector<std::pair<std::string, std::pair<std::string, std::vector<uint64_t>>>> all;
        for (auto &b : saved_bounds())
            if (key != b.key) all.push_back({b.key, {b.what, b.offsets}});
        std::vector<uint64_t> offs;
        for (int64_t a : firsts) offs.push_back((uint64_t)a - o.base);
        all.push_back({key, {what, offs}});
        std::sort(all.begin(), all.end());
        std::string text =
            "// Where the generator cuts each module's C into files, keyed by the module's build ID (16 hex digits): "
            "file k holds\n// the functions from offset k (from the module base) up to offset k + 1, so a changed "
            "function changes only its own\n// file. Written by `lasgen --new-chunk-bounds` (do not edit by hand); the "
            "same cuts as tools/chunk_bounds.py, which the\n// Python reference generator reads, until that is "
            "retired.\n\n";
        for (auto &e : all) {
            text += "{\"" + e.first + "\", \"" + e.second.first + "\", {\n";
            auto &v = e.second.second;
            for (size_t i = 0; i < v.size(); i += 8) {
                text += "   ";
                for (size_t j = i; j < v.size() && j < i + 8; j++) text += " 0x" + hexs(v[j]) + ",";
                text += "\n";
            }
            text += "}},\n";
        }
        const char *bf = o.bounds_file ? o.bounds_file : "runtime/gen/chunk_bounds.inc";
        write_if_changed(fs::u8path(bf), text);
        r.log("%s: saved %d file cuts for build %s to %s", name.c_str(), nchunks, key.c_str(), bf);
    }
    for (auto &de : fs::directory_iterator(outdir)) {  // an earlier run's extra chunks
        std::string fn = de.path().filename().u8string();
        if (fn.size() == name.size() + 6 && fn.compare(0, name.size(), name) == 0 && fn[name.size()] == '_' &&
            isdigit((unsigned char)fn[name.size() + 1]) && isdigit((unsigned char)fn[name.size() + 2]) &&
            isdigit((unsigned char)fn[name.size() + 3]) && fn.compare(name.size() + 4, 2, ".c") == 0 &&
            std::stoi(fn.substr(name.size() + 1, 3)) >= nchunks)
            fs::remove(de.path());
    }
    {
        std::string f = "#include \"cpu.h\"\n";
        for (uint64_t a : order) f += "void " + fname(a) + "(Cpu *c);\n";
        f += "const uint64_t " + name + "_fn_addrs[] = {\n";
        for (size_t i = 0; i < order.size(); i++) f += (i ? ",\n" : "") + hx(order[i]);
        f += "};\n";
        f += "const GuestFn " + name + "_fn_ptrs[] = {\n";
        for (size_t i = 0; i < order.size(); i++) f += (i ? ",\n" : "") + fname(order[i]);
        f += "};\n";
        f += cat("const uint64_t ", name, "_fn_count = ", order.size(), ";\n");
        if (o.dyn_base) {  // (in each module's table, weak: main and sdk share one; the table keeps CANON addresses)
            f += "// the guest window's start (--dyn-base): the runtime sets it before any game code runs\n";
            f += "__attribute__((weak, visibility(\"hidden\"))) uint64_t lasgame_gb = " + hx(CANON) + ";\n";
            f += "__attribute__((weak, visibility(\"default\"))) void lasgame_set_guest_base(uint64_t gb) "
                 "{ lasgame_gb = gb; }\n";
        }
        write_if_changed(outdir / (name + "_table.c"), f);
    }
    if (name == "main")  // (what this code was generated from: the Makefile's game code library id, GAMELIB_BUILD_ID)
        for (int i = 0; i < GAME_VERSION_COUNT; i++)
            if (version == GAME_VERSIONS[i].key)
                write_if_changed(outdir / "main_build_id.txt", std::string(GAME_VERSIONS[i].build_id) + "\n");
    r.log("%s: wrote %d chunks, %lds total", name.c_str(), nchunks, secs());
}

}  // namespace lasgen

extern "C" void lasgen_defaults(LasgenOptions *o) {
    *o = LasgenOptions{};
    o->simd = "asm";
    o->chunk = 1500;
    o->chunk_bytes = 1500000;
    o->big_fn = 100000;
}

extern "C" bool lasgen_run(const LasgenOptions *o, char *why, size_t why_size) {
    try {
        lasgen::run(*o);
        return true;
    } catch (const std::exception &e) {
        if (why && why_size) snprintf(why, why_size, "%s: %s", o->name ? o->name : "lasgen", e.what());
    }
    lasgen::PATCH_ADDRESSES = nullptr;
    return false;
}
