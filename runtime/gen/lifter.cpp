// AArch64 -> C static recompiler (port of tools/lifter.py).
//
// Integer, memory, and control flow are translated to C. FP/SIMD instructions (and CRC32) are either re-emitted as
// inline assembly (simd "asm", AArch64 hosts), with guest V registers loaded from / stored to the Cpu struct around
// each batch, so their semantics are bit-exact by construction, or lowered to portable C (simd "c", simd_c.cpp +
// runtime/simd.h).
#include "gen_internal.hpp"

#include <algorithm>
#include <cstdio>

namespace lasgen {

const char *const COND_NAMES[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                    "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
const int SCRATCH[9] = {9, 10, 11, 12, 13, 14, 15, 16, 17};

thread_local bool DYN_BASE = false;

std::string hexs(uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "%llx", (unsigned long long)v);
    return b;
}

std::string hx(uint64_t v) { return "0x" + hexs(v) + "ull"; }

std::string hx_signed(int64_t v) { return v < 0 ? "0x-" + hexs((uint64_t)-v) + "ull" : hx((uint64_t)v); }

std::string hex08(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "%08x", v);
    return b;
}

// gen.py --dyn-base (Android): the guest window starts wherever the process has room instead of at CANON. Addresses
// the code derives from its own pc (ADR / ADRP, literal loads, return addresses, branch and dispatch targets, svc /
// brk / udf sites) are then emitted as GB + offset, GB being the window's actual start (a local of every function
// that uses it, read from lasgame_gb). Without the flag the output is unchanged. From LASRecompAndroid by mamcjr (MIT).
std::string gaddr(uint64_t v) {
    if (DYN_BASE && CANON <= v && v < CANON + WINDOW) return "(GB + 0x" + hexs(v - CANON) + "ull)";
    return hx(v);
}

std::string join(const std::vector<std::string> &v, const std::string &sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}

// ---------------------------------------------------------------- bit helpers

static uint64_t ror(uint64_t v, int r, int size) {
    r %= size;
    return r ? (((v >> r) | (v << (size - r))) & ones(size)) : v;
}

static uint64_t replicate(uint64_t v, int esize, int size) {
    uint64_t out = 0;
    for (int i = 0; i < size; i += esize) out |= v << i;
    return out;
}

static int bit_length(uint64_t x) {
    int n = 0;
    while (x) n++, x >>= 1;
    return n;
}

static std::pair<uint64_t, uint64_t> decode_bit_masks(int N, int imms, int immr, int size, bool immediate) {
    int x = (N << 6) | (~imms & 0x3F);
    if (x == 0) throw Unsupported("reserved bitmask");
    int length = bit_length((uint64_t)x) - 1;
    if (length < 1) throw Unsupported("reserved bitmask");
    int levels = (int)ones(length);
    if (immediate && (imms & levels) == levels) throw Unsupported("reserved bitmask");
    int S = imms & levels;
    int Rr = immr & levels;
    int diff = (S - Rr) & 0x3F;
    int esize = 1 << length;
    int d = diff & levels;
    uint64_t welem = ones(S + 1);
    uint64_t telem = ones(d + 1);
    uint64_t wmask = replicate(ror(welem, Rr, esize), esize, size);
    uint64_t tmask = replicate(telem, esize, size);
    return {wmask, tmask};
}

// ---------------------------------------------------------------- register helpers

std::string X(int r) { return r == 31 ? "0ull" : cat("c->x[", r, "]"); }
std::string XS(int r) { return r == 31 ? "c->sp" : cat("c->x[", r, "]"); }
std::string W(int r) { return r == 31 ? "0u" : cat("(uint32_t)c->x[", r, "]"); }
std::string WS(int r) { return r == 31 ? "(uint32_t)c->sp" : cat("(uint32_t)c->x[", r, "]"); }

std::string R(int sf, int r, bool sp) {
    if (sf) return sp ? XS(r) : X(r);
    return sp ? WS(r) : W(r);
}

// Statement writing expr to Rd (zero-extending 32-bit results).
std::string set_reg(int sf, int r, const std::string &expr, bool sp) {
    if (r == 31 && !sp) return "(void)(" + expr + ");";
    std::string dst = r == 31 ? "c->sp" : cat("c->x[", r, "]");
    if (sf) return dst + " = (uint64_t)(" + expr + ");";
    return dst + " = (uint32_t)(" + expr + ");";
}

std::string shift_expr(int sf, const std::string &val, int typ, int amt) {
    if (amt == 0 && typ != 3) return val;
    if (typ == 0) return cat("(", val, " << ", amt, ")");
    if (typ == 1) return cat("(", val, " >> ", amt, ")");
    if (typ == 2)
        return sf ? cat("(uint64_t)((int64_t)", val, " >> ", amt, ")") : cat("(uint32_t)((int32_t)", val, " >> ", amt, ")");
    return cat("ROR", sf ? 64 : 32, "(", val, ", ", amt, ")");
}

std::string extend_expr(int r, int option, int shift) {
    std::string base = X(r), e;
    switch (option) {
        case 0: e = "(uint64_t)(uint8_t)" + base; break;
        case 1: e = "(uint64_t)(uint16_t)" + base; break;
        case 2: e = "(uint64_t)(uint32_t)" + base; break;
        case 4: e = "(uint64_t)(int64_t)(int8_t)" + base; break;
        case 5: e = "(uint64_t)(int64_t)(int16_t)" + base; break;
        case 6: e = "(uint64_t)(int64_t)(int32_t)" + base; break;
        default: e = base; break;
    }
    return shift ? cat("(", e, " << ", shift, ")") : e;
}

static std::string upper(std::string s) {
    for (auto &ch : s) ch = (char)toupper((unsigned char)ch);
    return s;
}

std::string cond_expr(int cond) {
    return cond < 14 ? "COND_" + upper(COND_NAMES[cond]) + "(c)" : "1";
}

// ---------------------------------------------------------------- Lifter

Lifter::Lifter(uint64_t base_, const std::string &simd) : base(base_) {
    if (simd != "asm" && simd != "c") throw GenError("simd='" + simd + "'");
    if (simd == "c") sc = std::make_unique<SimdC>();
}

// pc is absolute.
Insn Lifter::lift(uint32_t w, uint64_t pc) {
    int op0 = (int)bits(w, 25, 4);
    if ((op0 & 0b1110) == 0b1000) return dp_imm(w, pc);
    if ((op0 & 0b1110) == 0b1010) return branch_sys(w, pc);
    if ((op0 & 0b0101) == 0b0100) return ldst(w, pc);
    if ((op0 & 0b0111) == 0b0101) return dp_reg(w, pc);
    if ((op0 & 0b0111) == 0b0111) return fp_simd(w, pc);
    // padding, trap words (e7ffdefe), header data: trap if ever executed
    return Insn::trap("rt_udf(c, " + gaddr(pc) + ");");
}

// ------------------------------------------------------------ data processing (imm)

Insn Lifter::dp_imm(uint32_t w, uint64_t pc) {
    int op = (int)bits(w, 23, 3);
    int sf = (int)bits(w, 31, 1);
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5);
    if (op == 0 || op == 1) {  // PC-relative
        int64_t imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21);
        uint64_t val;
        if (bits(w, 31, 1))
            val = (pc & ~0xFFFull) + ((uint64_t)imm << 12);
        else
            val = pc + (uint64_t)imm;
        return Insn::op(set_reg(1, rd, gaddr(val)));
    }
    if (op == 2) {  // add/sub imm
        int opc = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1), sh = (int)bits(w, 22, 1);
        uint64_t imm = bits(w, 10, 12) << (sh ? 12 : 0);
        std::string a = R(sf, rn, true);
        if (S) {
            std::string fn = std::string(opc ? "subs" : "adds") + (sf ? "64" : "32");
            return Insn::op(set_reg(sf, rd, fn + "(c, " + a + ", " + hx(imm) + ")"));
        }
        return Insn::op(set_reg(sf, rd, a + (opc ? " - " : " + ") + hx(imm), true));
    }
    if (op == 4) {  // logical imm
        int opc = (int)bits(w, 29, 2), N = (int)bits(w, 22, 1);
        if (!sf && N) throw Unsupported("logical imm N=1 32-bit");
        int size = sf ? 64 : 32;
        uint64_t imm = decode_bit_masks(N, (int)bits(w, 10, 6), (int)bits(w, 16, 6), size, true).first;
        std::string a = R(sf, rn);
        if (opc == 0) return Insn::op(set_reg(sf, rd, a + " & " + hx(imm), true));
        if (opc == 1) return Insn::op(set_reg(sf, rd, a + " | " + hx(imm), true));
        if (opc == 2) return Insn::op(set_reg(sf, rd, a + " ^ " + hx(imm), true));
        return Insn::op(set_reg(sf, rd, cat("logic", size, "(c, ", a, " & ", hx(imm), ")")));
    }
    if (op == 5) {  // move wide
        int opc = (int)bits(w, 29, 2), hw = (int)bits(w, 21, 2);
        if (!sf && hw > 1) throw Unsupported("movw hw");
        uint64_t imm = bits(w, 5, 16) << (hw * 16);
        if (opc == 0) return Insn::op(set_reg(sf, rd, hx(~imm & ones(sf ? 64 : 32))));
        if (opc == 2) return Insn::op(set_reg(sf, rd, hx(imm)));
        if (opc == 3) {
            uint64_t mask = ~(0xFFFFull << (hw * 16));
            return Insn::op(set_reg(sf, rd, "(" + X(rd) + " & " + hx(mask) + ") | " + hx(imm)));
        }
        throw Unsupported("movw opc");
    }
    if (op == 6) {  // bitfield
        int opc = (int)bits(w, 29, 2), N = (int)bits(w, 22, 1);
        int immr = (int)bits(w, 16, 6), imms = (int)bits(w, 10, 6);
        int size = sf ? 64 : 32;
        if (N != sf) throw Unsupported("bitfield N");
        auto [wmask, tmask] = decode_bit_masks(N, imms, immr, size, false);
        std::string src = R(sf, rn);
        std::string rot = immr ? cat("ROR", size, "(", src, ", ", immr, ")") : src;
        if (opc == 2)  // UBFM
            return Insn::op(set_reg(sf, rd, rot + " & " + hx(wmask & tmask)));
        if (opc == 0) {  // SBFM
            std::string top = cat("(uint", size, "_t)(-(int", size, "_t)((", src, " >> ", imms, ") & 1))");
            return Insn::op(set_reg(sf, rd, "(" + top + " & " + hx(~tmask & ones(size)) + ") | (" + rot + " & " +
                                                hx(wmask & tmask) + ")"));
        }
        if (opc == 1) {  // BFM
            std::string dst = R(sf, rd);
            std::string bot = "((" + dst + " & " + hx(~wmask & ones(size)) + ") | (" + rot + " & " + hx(wmask) + "))";
            return Insn::op(
                set_reg(sf, rd, "(" + dst + " & " + hx(~tmask & ones(size)) + ") | (" + bot + " & " + hx(tmask) + ")"));
        }
        throw Unsupported("bitfield opc");
    }
    if (op == 7) {  // EXTR
        int rm = (int)bits(w, 16, 5), lsb = (int)bits(w, 10, 6);
        int size = sf ? 64 : 32;
        if (lsb == 0) return Insn::op(set_reg(sf, rd, R(sf, rm)));
        return Insn::op(
            set_reg(sf, rd, cat("(", R(sf, rm), " >> ", lsb, ") | ((uint", size, "_t)", R(sf, rn), " << ", size - lsb, ")")));
    }
    throw Unsupported("dp_imm " + hex08(w));
}

// ------------------------------------------------------------ branches / system

Insn Lifter::branch_sys(uint32_t w, uint64_t pc) {
    if (bits(w, 26, 5) == 0b00101) {  // B / BL
        uint64_t t = pc + (uint64_t)(sext(bits(w, 0, 26), 26) * 4);
        return Insn::jump(bits(w, 31, 1) ? Kind::bl : Kind::b, t);
    }
    if (bits(w, 25, 7) == 0b0101010 && !bits(w, 4, 1)) {  // B.cond
        uint64_t t = pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
        int cond = (int)bits(w, 0, 4);
        if (cond >= 14) return Insn::jump(Kind::b, t);
        return Insn::jump(Kind::bcond, t, "COND_" + upper(COND_NAMES[cond]) + "(c)");
    }
    if (bits(w, 25, 6) == 0b011010) {  // CBZ/CBNZ
        int sf = (int)bits(w, 31, 1), nz = (int)bits(w, 24, 1), rt = (int)bits(w, 0, 5);
        uint64_t t = pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
        return Insn::jump(Kind::bcond, t, R(sf, rt) + (nz ? " != " : " == ") + "0");
    }
    if (bits(w, 25, 6) == 0b011011) {  // TBZ/TBNZ
        int b = (int)((bits(w, 31, 1) << 5) | bits(w, 19, 5));
        int nz = (int)bits(w, 24, 1), rt = (int)bits(w, 0, 5);
        uint64_t t = pc + (uint64_t)(sext(bits(w, 5, 14), 14) * 4);
        return Insn::jump(Kind::bcond, t, cat("((", X(rt), " >> ", b, ") & 1) ", nz ? "!=" : "==", " 0"));
    }
    if (bits(w, 25, 7) == 0b1101011) {  // branch register
        int opc = (int)bits(w, 21, 4), rn = (int)bits(w, 5, 5);
        if (bits(w, 16, 5) != 31 || bits(w, 10, 6) || bits(w, 0, 5)) throw Unsupported("branch reg variant");
        if (opc == 0) return Insn::reg(Kind::br, X(rn));
        if (opc == 1) return Insn::reg(Kind::blr, X(rn));
        if (opc == 2) {
            Insn i;
            i.kind = Kind::ret;
            return i;
        }
        throw Unsupported("branch reg opc");
    }
    if (bits(w, 24, 8) == 0b11010100) {  // exception generation
        int opc = (int)bits(w, 21, 3), ll = (int)bits(w, 0, 5), imm = (int)bits(w, 5, 16);
        if (opc == 0 && ll == 1) return Insn::op(cat("rt_svc(c, ", imm, ", ", gaddr(pc), ");"));
        if (opc == 1 && ll == 0) return Insn::trap(cat("rt_brk(c, ", imm, ", ", gaddr(pc), ");"));
        throw Unsupported("exception");
    }
    if (bits(w, 22, 10) == 0b1101010100) return system(w, pc);
    throw Unsupported("branch_sys " + hex08(w));
}

Insn Lifter::system(uint32_t w, uint64_t) {
    int L = (int)bits(w, 21, 1);
    int op0 = (int)bits(w, 19, 2), op1 = (int)bits(w, 16, 3), crn = (int)bits(w, 12, 4), crm = (int)bits(w, 8, 4),
        op2 = (int)bits(w, 5, 3), rt = (int)bits(w, 0, 5);
    if (op0 == 0 && !L) {
        if (crn == 2 && op1 == 3 && rt == 31) return Insn::op("/* hint */");  // hints
        if (crn == 3 && op1 == 3 && rt == 31) {                                // barriers
            if (op2 == 2) return Insn::op("c->excl_addr = ~0ull;");
            return Insn::op("__atomic_thread_fence(__ATOMIC_SEQ_CST);");
        }
        throw Unsupported("pstate/system " + hex08(w));
    }
    if (op0 == 1) {  // SYS
        if (!L && op1 == 3 && crn == 7 && crm == 4 && op2 == 1)  // DC ZVA
            return Insn::op("memset((void*)(uintptr_t)(" + X(rt) + " & ~63ull), 0, 64);");
        return Insn::op("/* cache maintenance */");
    }
    auto key = std::make_tuple(op0, op1, crn, crm, op2);
    // f'{key}' of the Python tuple
    std::string key_text = cat("(", op0, ", ", op1, ", ", crn, ", ", crm, ", ", op2, ")");
    auto is = [&](int a, int b, int c, int d, int e) { return key == std::make_tuple(a, b, c, d, e); };
    if (L) {  // MRS
        const char *src = nullptr;
        if (is(3, 3, 13, 0, 3)) src = "c->tpidrro";
        else if (is(3, 3, 13, 0, 2)) src = "c->tpidr";
        else if (is(3, 3, 14, 0, 0)) src = "19200000ull";
        else if (is(3, 3, 14, 0, 1)) src = "rt_ticks()";
        else if (is(3, 3, 14, 0, 2)) src = "rt_ticks()";
        else if (is(3, 3, 4, 2, 0)) src = "c->nzcv";
        else if (is(3, 3, 4, 4, 0)) src = sc ? "c->fpcr" : "rt_get_fpcr()";
        else if (is(3, 3, 4, 4, 1)) src = sc ? "c->fpsr" : "rt_get_fpsr()";
        else if (is(3, 3, 0, 0, 7)) src = "4ull";
        else if (is(3, 3, 0, 0, 1)) src = "0x8444c004ull";
        if (!src) throw Unsupported("mrs " + key_text);
        return Insn::op(set_reg(1, rt, src));
    }
    std::string v = X(rt);
    if (is(3, 3, 13, 0, 2)) return Insn::op("c->tpidr = " + v + ";");
    if (is(3, 3, 4, 2, 0)) return Insn::op("c->nzcv = " + v + " & 0xf0000000ull;");
    if (is(3, 3, 4, 4, 0)) return Insn::op(sc ? "c->fpcr = " + v + " & 0x7c00000ull;" : "rt_set_fpcr(" + v + ");");
    if (is(3, 3, 4, 4, 1)) return Insn::op(sc ? "c->fpsr = " + v + " & 0xf800009full;" : "rt_set_fpsr(" + v + ");");
    throw Unsupported("msr " + key_text);
}

// ------------------------------------------------------------ loads / stores

Insn Lifter::ldst(uint32_t w, uint64_t pc) {
    int V = (int)bits(w, 26, 1);
    if (V) {
        if (sc) return Insn::op(sc->ldst(w, pc));
        return ldst_simd(w, pc);
    }
    if (bits(w, 24, 6) == 0b001000) return ldst_excl(w, pc);
    if (bits(w, 27, 3) == 0b011 && bits(w, 24, 2) == 0) {  // literal
        int opc = (int)bits(w, 30, 2), rt = (int)bits(w, 0, 5);
        uint64_t addr = pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
        if (opc == 0) return Insn::op(set_reg(0, rt, "LD32(" + gaddr(addr) + ")"));
        if (opc == 1) return Insn::op(set_reg(1, rt, "LD64(" + gaddr(addr) + ")"));
        if (opc == 2) return Insn::op(set_reg(1, rt, "(int64_t)(int32_t)LD32(" + gaddr(addr) + ")"));
        return Insn::op("/* prfm */");
    }
    if (bits(w, 27, 3) == 0b101) return ldst_pair(w);
    if (bits(w, 27, 3) == 0b111) return ldst_reg(w);
    throw Unsupported("ldst " + hex08(w));
}

Insn Lifter::ldst_excl(uint32_t w, uint64_t) {
    int size = (int)bits(w, 30, 2), o2 = (int)bits(w, 23, 1), L = (int)bits(w, 22, 1), o1 = (int)bits(w, 21, 1),
        rs = (int)bits(w, 16, 5), rn = (int)bits(w, 5, 5), rt = (int)bits(w, 0, 5);
    int nbits = 8 << size;
    std::string addr = XS(rn);
    if (o1) throw Unsupported("exclusive pair / CAS");
    if (o2 == 0) {
        if (L) return Insn::op(set_reg(1, rt, cat("ldx", nbits, "(c, ", addr, ")")));
        return Insn::op(cat("{ uint32_t st = stx", nbits, "(c, ", addr, ", ", X(rt), "); ", set_reg(0, rs, "st"), " }"));
    }
    if (L) return Insn::op(set_reg(1, rt, cat("lda", nbits, "(", addr, ")")));
    return Insn::op(cat("stl", nbits, "(", addr, ", ", X(rt), ");"));
}

Insn Lifter::ldst_pair(uint32_t w) {
    int opc = (int)bits(w, 30, 2), mode = (int)bits(w, 23, 2), L = (int)bits(w, 22, 1);
    int64_t imm7 = sext(bits(w, 15, 7), 7);
    int rt2 = (int)bits(w, 10, 5), rn = (int)bits(w, 5, 5), rt = (int)bits(w, 0, 5);
    if (opc == 3) throw Unsupported("pair opc 3");
    int scale = opc == 2 ? 3 : 2;
    int64_t off = imm7 * (1 << scale);
    std::string base = XS(rn);
    bool post = mode == 1;
    bool wb = mode == 1 || mode == 3;
    std::string ea = post ? base : (off >= 0 ? cat(base, " + ", off) : cat(base, " - ", -off));
    std::vector<std::string> lines = {"uint64_t ea = " + ea + ";"};
    if (L) {
        if (opc == 2)
            lines.push_back("uint64_t a = LD64(ea), b = LD64(ea + 8);");
        else if (opc == 1)  // LDPSW
            lines.push_back("uint64_t a = (int64_t)(int32_t)LD32(ea), b = (int64_t)(int32_t)LD32(ea + 4);");
        else
            lines.push_back("uint64_t a = LD32(ea), b = LD32(ea + 4);");
    } else {
        if (opc == 2)
            lines.push_back("ST64(ea, " + X(rt) + "); ST64(ea + 8, " + X(rt2) + ");");
        else
            lines.push_back("ST32(ea, " + W(rt) + "); ST32(ea + 4, " + W(rt2) + ");");
    }
    if (wb) {
        std::string dst = rn == 31 ? "c->sp" : cat("c->x[", rn, "]");
        if (!post)
            lines.push_back(dst + " = ea;");
        else
            lines.push_back(off >= 0 ? cat(dst, " = ea + ", off, ";") : cat(dst, " = ea - ", -off, ";"));
    }
    if (L) {
        lines.push_back(set_reg(1, rt, "a"));
        lines.push_back(set_reg(1, rt2, "b"));
    }
    return Insn::op("{ " + join(lines, " ") + " }");
}

Insn Lifter::ldst_reg(uint32_t w) {
    int size = (int)bits(w, 30, 2), opc = (int)bits(w, 22, 2), rn = (int)bits(w, 5, 5), rt = (int)bits(w, 0, 5);
    std::string base = XS(rn);
    std::string wb;
    std::string ea;
    if (bits(w, 24, 2) == 1) {
        ea = base + " + " + hx(bits(w, 10, 12) << size);
    } else {
        int kind = (int)bits(w, 10, 2);
        if (bits(w, 21, 1)) {
            if (kind != 2) throw Unsupported("ldst reg atomic/pac");
            int rm = (int)bits(w, 16, 5), option = (int)bits(w, 13, 3), S = (int)bits(w, 12, 1);
            if (option != 2 && option != 3 && option != 6 && option != 7) throw Unsupported("ldst reg option");
            ea = base + " + " + extend_expr(rm, option, S ? size : 0);
        } else {
            int64_t imm9 = sext(bits(w, 12, 9), 9);
            std::string sign_imm = imm9 >= 0 ? cat("+ ", imm9) : cat("- ", -imm9);
            if (kind == 0 || kind == 2) {  // unscaled, unprivileged
                ea = base + " " + sign_imm;
            } else if (kind == 1) {  // post
                ea = base;
                wb = base + " " + sign_imm;
            } else {  // pre
                ea = base + " " + sign_imm;
                wb = "ea";
            }
        }
    }
    int nbits = 8 << size;
    std::vector<std::string> parts;
    std::string load;
    if (opc == 0) {
        parts.push_back(cat("ST", nbits, "(ea, ", size == 3 ? X(rt) : W(rt), ");"));
    } else if (opc == 1) {
        parts.push_back(cat("uint64_t v = LD", nbits, "(ea);"));
        load = set_reg(1, rt, "v");
    } else if (size == 3) {
        return Insn::op("/* prfm */");
    } else if (size == 2 && opc == 3) {
        throw Unsupported("ldst size2 opc3");
    } else {
        parts.push_back(cat("uint64_t v = (uint64_t)(int64_t)(int", nbits, "_t)LD", nbits, "(ea);"));
        load = set_reg(opc == 2 ? 1 : 0, rt, "v");
    }
    parts.insert(parts.begin(), "uint64_t ea = " + ea + ";");
    if (!wb.empty())  // writeback before the register load, so a load into rn wins
        parts.push_back((rn == 31 ? std::string("c->sp") : cat("c->x[", rn, "]")) + " = " + wb + ";");
    if (!load.empty()) parts.push_back(load);
    return Insn::op("{ " + join(parts, " ") + " }");
}

// FP/SIMD loads and stores are passed through as asm; GPR address regs are remapped.
Insn Lifter::ldst_simd(uint32_t w, uint64_t pc) {
    int rt = (int)bits(w, 0, 5);
    if (bits(w, 27, 3) == 0b011 && bits(w, 24, 2) == 0) {  // literal -> LDR (imm, offset 0) via scratch
        int opc = (int)bits(w, 30, 2);
        int size, o;
        if (opc == 0) size = 2, o = 1;
        else if (opc == 1) size = 3, o = 1;
        else if (opc == 2) size = 0, o = 3;
        else throw GenError("KeyError: 3");
        uint64_t addr = pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
        uint32_t nw = ((uint32_t)size << 30) | 0x3D000000u | ((uint32_t)o << 22) | (31u << 5) | (uint32_t)rt;
        AsmInsn a;
        a.word = nw;
        a.vregs = {rt};
        a.consts = {{5, addr}};
        return Insn::as(std::move(a));
    }
    if (bits(w, 31, 1) == 0 && (bits(w, 24, 6) == 0b001100 || bits(w, 24, 6) == 0b001101)) {  // structures
        int post = (int)bits(w, 23, 1);
        int rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5);
        int n;
        if (bits(w, 24, 1) == 0) {
            switch (bits(w, 12, 4)) {
                case 0: case 2: n = 4; break;
                case 4: case 6: n = 3; break;
                case 7: n = 1; break;
                case 8: case 10: n = 2; break;
                default: throw Unsupported("ldst multiple opcode");
            }
        } else {
            n = (int)(((bits(w, 13, 1) << 1) | bits(w, 21, 1)) + 1);
        }
        AsmInsn a;
        a.word = w;
        for (int i = 0; i < n; i++) a.vregs.insert((rt + i) % 32);
        a.gprs.push_back({5, rn == 31 ? GPR_SP : rn});
        if (post && rm != 31) a.gprs.push_back({16, rm});
        return Insn::as(std::move(a));
    }
    int rn = (int)bits(w, 5, 5);
    AsmInsn a;
    a.word = w;
    a.gprs.push_back({5, rn == 31 ? GPR_SP : rn});
    a.vregs = {rt};
    if (bits(w, 27, 3) == 0b101) {  // pair
        a.vregs.insert((int)bits(w, 10, 5));
    } else if (bits(w, 27, 3) == 0b111 && bits(w, 24, 2) == 0 && bits(w, 21, 1) && bits(w, 10, 2) == 2) {
        int rm = (int)bits(w, 16, 5);
        if (rm != 31) a.gprs.push_back({16, rm});
    }
    return Insn::as(std::move(a));
}

// ------------------------------------------------------------ data processing (reg)

Insn Lifter::dp_reg(uint32_t w, uint64_t) {
    int sf = (int)bits(w, 31, 1);
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5);
    int size = sf ? 64 : 32;
    int op1 = (int)bits(w, 28, 1);
    if (!op1) {
        if (bits(w, 24, 4) == 0b1010) {  // logical shifted
            int opc = (int)bits(w, 29, 2), sh = (int)bits(w, 22, 2), N = (int)bits(w, 21, 1), amt = (int)bits(w, 10, 6);
            if (!sf && amt >= 32) throw Unsupported("logical shift amount");
            std::string b = shift_expr(sf, R(sf, rm), sh, amt);
            if (N) b = cat("(uint", size, "_t)~", b);
            std::string a = R(sf, rn);
            std::string expr = opc == 1 ? a + " | " + b : opc == 2 ? a + " ^ " + b : a + " & " + b;
            if (opc == 3) return Insn::op(set_reg(sf, rd, cat("logic", size, "(c, (uint", size, "_t)(", expr, "))")));
            return Insn::op(set_reg(sf, rd, expr));
        }
        if (bits(w, 24, 4) == 0b1011) {  // add/sub shifted or extended
            int op = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1);
            std::string a, b;
            bool sp_ok;
            if (bits(w, 21, 1) == 0) {
                int sh = (int)bits(w, 22, 2), amt = (int)bits(w, 10, 6);
                if (sh == 3) throw Unsupported("add shift ror");
                a = R(sf, rn);
                b = shift_expr(sf, R(sf, rm), sh, amt);
                sp_ok = false;
            } else {
                if (bits(w, 22, 2)) throw Unsupported("add ext opt");
                int option = (int)bits(w, 13, 3), amt = (int)bits(w, 10, 3);
                a = R(sf, rn, true);
                b = extend_expr(rm, option, amt);
                if (!sf) b = "(uint32_t)(" + b + ")";
                sp_ok = !S;
            }
            if (S) {
                std::string fn = cat(op ? "subs" : "adds", size);
                return Insn::op(set_reg(sf, rd, cat(fn, "(c, ", a, ", (uint", size, "_t)(", b, "))")));
            }
            return Insn::op(set_reg(sf, rd, a + (op ? " - " : " + ") + b, sp_ok));
        }
        throw Unsupported("dp_reg " + hex08(w));
    }
    int op2 = (int)bits(w, 21, 4);
    if (bits(w, 24, 4) == 0b1011) {  // 3-source
        int op54 = (int)bits(w, 29, 2), op31 = (int)bits(w, 21, 3), o0 = (int)bits(w, 15, 1), ra = (int)bits(w, 10, 5);
        if (op54) throw Unsupported("dp3 op54");
        const char *sgn = o0 ? "-" : "+";
        if (op31 == 0) return Insn::op(set_reg(sf, rd, cat(R(sf, ra), " ", sgn, " ", R(sf, rn), " * ", R(sf, rm))));
        if (op31 == 1)
            return Insn::op(set_reg(
                1, rd, cat(X(ra), " ", sgn, " (uint64_t)((int64_t)(int32_t)", W(rn), " * (int64_t)(int32_t)", W(rm), ")")));
        if (op31 == 5)
            return Insn::op(set_reg(1, rd, cat(X(ra), " ", sgn, " (uint64_t)", W(rn), " * (uint64_t)", W(rm))));
        if (op31 == 2)
            return Insn::op(set_reg(
                1, rd, "(uint64_t)(((__int128)(int64_t)" + X(rn) + " * (__int128)(int64_t)" + X(rm) + ") >> 64)"));
        if (op31 == 6)
            return Insn::op(set_reg(1, rd, "(uint64_t)(((unsigned __int128)" + X(rn) + " * " + X(rm) + ") >> 64)"));
        throw Unsupported("dp3");
    }
    if (op2 == 0) {  // add/sub with carry
        int op = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1);
        std::string fn = cat(op ? "sbc" : "adc", S ? "s" : "", size);
        return Insn::op(set_reg(sf, rd, fn + "(c, " + R(sf, rn) + ", " + R(sf, rm) + ")"));
    }
    if (op2 == 2) {  // conditional compare
        int op = (int)bits(w, 30, 1), imm_form = (int)bits(w, 11, 1), cond = (int)bits(w, 12, 4), nzcv = (int)bits(w, 0, 4);
        std::string b = imm_form ? hx((uint64_t)rm) : R(sf, rm);
        std::string fn = cat(op ? "subs" : "adds", size);
        return Insn::op("if (" + cond_expr(cond) + ") (void)" + fn + "(c, " + R(sf, rn) + ", " + b + "); " +
                        "else c->nzcv = " + hx((uint64_t)nzcv << 28) + ";");
    }
    if (op2 == 4) {  // conditional select
        int op = (int)bits(w, 30, 1), o2 = (int)bits(w, 10, 2), cond = (int)bits(w, 12, 4);
        std::string b = R(sf, rm), alt;
        if (op == 0 && o2 == 0) alt = b;
        else if (op == 0 && o2 == 1) alt = b + " + 1";
        else if (op == 1 && o2 == 0) alt = "~" + b;
        else if (op == 1 && o2 == 1) alt = "-" + b;
        else throw Unsupported("csel variant");
        return Insn::op(set_reg(sf, rd, cat("(", cond_expr(cond), ") ? ", R(sf, rn), " : (uint", size, "_t)(", alt, ")")));
    }
    if (op2 == 6) {
        int opcode = (int)bits(w, 10, 6);
        if (bits(w, 30, 1)) {  // 1-source
            if (bits(w, 16, 5)) throw Unsupported("dp1 opcode2");
            std::string a = R(sf, rn), expr;
            switch (opcode) {
                case 0: expr = cat("rbit", size, "(", a, ")"); break;
                case 1: expr = cat("rev16_", size, "(", a, ")"); break;
                case 2: expr = !sf ? "__builtin_bswap32(" + a + ")" : "rev32_64(" + a + ")"; break;
                case 3: if (sf) expr = "__builtin_bswap64(" + a + ")"; break;
                case 4: expr = cat("clz", size, "(", a, ")"); break;
                case 5: expr = cat("cls", size, "(", a, ")"); break;
            }
            if (expr.empty()) throw Unsupported("dp1");
            return Insn::op(set_reg(sf, rd, expr));
        }
        if (16 <= opcode && opcode <= 23) {  // CRC32: host has the same instruction
            if (sc) return Insn::op(SimdC::crc(w));
            AsmInsn a;
            a.word = w;
            for (auto [p, r] : {std::pair<int, int>{0, rd}, {5, rn}, {16, rm}})
                if (r != 31) a.gprs.push_back({p, r});
            return Insn::as(std::move(a));
        }
        std::string a = R(sf, rn), b = R(sf, rm), expr;
        switch (opcode) {
            case 2: expr = cat("udiv", size, "(", a, ", ", b, ")"); break;
            case 3: expr = cat("sdiv", size, "(", a, ", ", b, ")"); break;
            case 8: expr = cat(a, " << (", b, " & ", size - 1, ")"); break;
            case 9: expr = cat(a, " >> (", b, " & ", size - 1, ")"); break;
            case 10: expr = cat("(uint", size, "_t)((int", size, "_t)", a, " >> (", b, " & ", size - 1, "))"); break;
            case 11: expr = cat("ROR", size, "(", a, ", ", b, " & ", size - 1, ")"); break;
        }
        if (expr.empty()) throw Unsupported("dp2");
        return Insn::op(set_reg(sf, rd, expr));
    }
    throw Unsupported("dp_reg " + hex08(w));
}

// ------------------------------------------------------------ FP / SIMD data processing

Insn Lifter::fp_simd(uint32_t w, uint64_t pc) {
    if (sc) return Insn::op(sc->dp(w, pc));
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5), ra = (int)bits(w, 10, 5);
    std::set<int> vregs = {rd, rn, rm, ra};
    std::vector<std::pair<int, int>> gprs;
    bool flags = false;
    if ((w & 0x5F20FC00) == 0x1E200000) {  // FP <-> integer
        int opcode = (int)bits(w, 16, 3);
        if (opcode == 0 || opcode == 1 || opcode == 4 || opcode == 5 || opcode == 6) {
            gprs = {{0, rd}};
            vregs = {rn};
        } else {
            gprs = {{5, rn}};
            vregs = {rd};
        }
    } else if ((w & 0x5F200000) == 0x1E000000) {  // FP <-> fixed point
        int opcode = (int)bits(w, 16, 3);
        if (opcode == 0 || opcode == 1) {
            gprs = {{0, rd}};
            vregs = {rn};
        } else {
            gprs = {{5, rn}};
            vregs = {rd};
        }
    } else if ((w & 0x9FE08400) == 0x0E000400 && bits(w, 29, 1) == 0) {  // AdvSIMD copy
        int imm4 = (int)bits(w, 11, 4);
        if (imm4 == 1 || imm4 == 3) {
            gprs = {{5, rn}};
            vregs = {rd};
        } else if (imm4 == 5 || imm4 == 7) {
            gprs = {{0, rd}};
            vregs = {rn};
        } else {
            vregs = {rd, rn};
        }
    } else if ((w & 0xBFE08C00) == 0x0E000000) {  // TBL/TBX
        int n = (int)bits(w, 13, 2) + 1;
        vregs = {rd, rm};
        for (int i = 0; i < n; i++) vregs.insert((rn + i) % 32);
    }
    if ((w & 0x5F203C00) == 0x1E202000 || (w & 0x5F200C00) == 0x1E200400 || (w & 0x5F200C00) == 0x1E200C00) flags = true;
    AsmInsn a;
    a.word = w;
    a.vregs = vregs;
    for (auto &g : gprs)
        if (g.second != 31) a.gprs.push_back(g);  // register 31 is zr in these encodings
    a.flags = flags;
    return Insn::as(std::move(a));
}

// Emit one inline-asm block for a run of FP/SIMD instructions.
std::string emit_asm_batch(const std::vector<const AsmInsn *> &batch) {
    std::set<int> vregs;
    std::vector<std::pair<int, int>> gmap;  // (guest reg, scratch), insertion order
    bool flags = false;
    std::vector<std::pair<int, uint64_t>> consts;
    std::vector<uint32_t> words;
    auto find = [&](int g) -> int {
        for (auto &e : gmap)
            if (e.first == g) return e.second;
        return -1;
    };
    for (const AsmInsn *a : batch) {
        vregs.insert(a->vregs.begin(), a->vregs.end());
        flags |= a->flags;
        uint32_t w = a->word;
        for (auto [pos, g] : a->gprs) {
            if (find(g) < 0) {
                if (gmap.size() + consts.size() >= 9) throw Unsupported("scratch exhausted");
                gmap.push_back({g, SCRATCH[gmap.size() + consts.size()]});
            }
            w = (w & ~(31u << pos)) | ((uint32_t)find(g) << pos);
        }
        for (auto [pos, val] : a->consts) {
            size_t k = gmap.size() + consts.size();
            if (k >= 9) throw GenError("IndexError: list index out of range");
            int s = SCRATCH[k];
            consts.push_back({s, val});
            w = (w & ~(31u << pos)) | ((uint32_t)s << pos);
        }
        words.push_back(w);
    }
    std::vector<std::string> lines;
    std::vector<std::string> ins = {"\"r\"(c)"};
    for (auto [g, s] : gmap) {
        int off = g == GPR_SP ? OFF_SP : OFF_X + 8 * g;
        lines.push_back(cat("ldr x", s, ", [%0, #", off, "]"));
    }
    for (auto [s, val] : consts) {
        ins.push_back("\"r\"(" + gaddr(val) + ")");
        lines.push_back(cat("mov x", s, ", %", (int)ins.size() - 1));
    }
    for (int v : vregs) lines.push_back(cat("ldr q", v, ", [%0, #", OFF_V + 16 * v, "]"));
    if (flags) {
        lines.push_back(cat("ldr x16, [%0, #", OFF_NZCV, "]"));
        lines.push_back("msr nzcv, x16");
    }
    for (uint32_t x : words) lines.push_back(".inst 0x" + hex08(x));
    if (flags) {
        lines.push_back("mrs x16, nzcv");
        lines.push_back(cat("str x16, [%0, #", OFF_NZCV, "]"));
    }
    for (int v : vregs) lines.push_back(cat("str q", v, ", [%0, #", OFF_V + 16 * v, "]"));
    for (auto [g, s] : gmap) {
        int off = g == GPR_SP ? OFF_SP : OFF_X + 8 * g;
        lines.push_back(cat("str x", s, ", [%0, #", off, "]"));
    }
    std::vector<std::string> clob;
    for (int v : vregs) clob.push_back(cat("\"v", v, "\""));
    std::set<int> xs;
    for (auto &e : gmap) xs.insert(e.second);
    for (auto &e : consts) xs.insert(e.first);
    if (flags) xs.insert(16);
    for (int s : xs) clob.push_back(cat("\"x", s, "\""));
    clob.push_back("\"cc\"");
    clob.push_back("\"memory\"");
    std::string body = join(lines, "\\n\\t");
    return "__asm__ volatile(\"" + body + "\" :: " + join(ins, ", ") + " : " + join(clob, ", ") + ");";
}

}  // namespace lasgen
