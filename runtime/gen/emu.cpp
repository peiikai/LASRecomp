// A small AArch64 evaluator for the generator's jump-table resolution (replaces the Unicorn engine gen.py steps).
//
// gen.py runs the instructions between a jump table's bounds check and its `br` once per index value and reads the
// branch target. This evaluator runs them with the semantics of the Arm Architecture Reference Manual and keeps the
// state the Python engine keeps between tables (one engine per module): memory writes persist, as do x29, x30 and the
// flags (gen.py resets x0..x28 and sp before each index). The windows are integer code (address arithmetic, the table
// load, branches) with some unrelated FP work mixed in: FP arithmetic is not computed, its results are "unknown" (see
// emu.hpp), which is enough as long as they don't decide the target. Instructions it does not run (system registers,
// exclusives, svc) stop the table, as an engine error does in gen.py. LASGEN_EMU_DEBUG=1 logs every stop.
#include "emu.hpp"

#include <cstdio>
#include <cstdlib>

namespace lasgen {

Emu::Emu(const Nso &nso, uint64_t base_) : base(base_), image(nso.image), scratch(SCRATCH_SIZE, 0) {
    debug = getenv("LASGEN_EMU_DEBUG") != nullptr;
    for (bool &k : xk) k = true;
    for (bool &k : vknown) k = true;
}

void Emu::unsupported(uint32_t w) {
    if (debug) fprintf(stderr, "[emu] unsupported %08x at %llx\n", w, (unsigned long long)pc);
    throw EmuFault();
}

void Emu::need_known(const char *what) {
    if (!unk) return;
    if (debug) fprintf(stderr, "[emu] unknown %s at %llx\n", what, (unsigned long long)pc);
    throw EmuFault();
}

// ---------------------------------------------------------------- memory

uint8_t *Emu::mem(uint64_t a, size_t n) {
    if (a >= base && a - base <= image.size() && n <= image.size() - (a - base)) return &image[a - base];
    if (a >= SCRATCH_MEM && a - SCRATCH_MEM <= SCRATCH_SIZE && n <= SCRATCH_SIZE - (a - SCRATCH_MEM))
        return &scratch[a - SCRATCH_MEM];
    if (debug) fprintf(stderr, "[emu] unmapped %llx at %llx\n", (unsigned long long)a, (unsigned long long)pc);
    throw EmuFault();
}

bool Emu::is_tainted(uint64_t a, int size) const {
    if (tainted.empty()) return false;
    for (int i = 0; i < size; i++)
        if (tainted.count(a + i)) return true;
    return false;
}

void Emu::taint(uint64_t a, int size, bool unknown) {
    for (int i = 0; i < size; i++)
        if (unknown) tainted.insert(a + i);
        else if (!tainted.empty()) tainted.erase(a + i);
}

uint64_t Emu::load(uint64_t a, int size, bool &known) {
    uint8_t *p = mem(a, size);
    uint64_t v = 0;
    memcpy(&v, p, size);
    known = !is_tainted(a, size);
    return v;
}

void Emu::store(uint64_t a, int size, uint64_t v, bool known) {
    memcpy(mem(a, size), &v, size);
    taint(a, size, !known);
}

void Emu::vload(int r, uint64_t a, int size) {
    uint8_t *p = mem(a, size);
    unsigned __int128 val = 0;
    memcpy(&val, p, size);
    v[r] = val;
    vknown[r] = !is_tainted(a, size);
}

void Emu::vstore(int r, uint64_t a, int size) {
    memcpy(mem(a, size), &v[r], size);
    taint(a, size, !vknown[r]);
}

// ---------------------------------------------------------------- registers

uint64_t Emu::xr(int r) {  // zr
    if (r == 31) return 0;
    unk |= !xk[r];
    return x[r];
}

uint64_t Emu::xsp(int r) {  // sp
    if (r == 31) {
        unk |= !spk;
        return sp;
    }
    return xr(r);
}

void Emu::wxk(int r, uint64_t v, bool known) {
    if (r != 31) x[r] = v, xk[r] = known;
}

void Emu::wxspk(int r, uint64_t v, bool known) {
    if (r == 31) sp = v, spk = known;
    else x[r] = v, xk[r] = known;
}

static uint64_t mask_of(int sf) { return sf ? ~0ull : 0xFFFFFFFFull; }

// AddWithCarry, returning the result and setting NZCV when flags
uint64_t Emu::add_with_carry(int sf, uint64_t a, uint64_t b, int carry, bool flags) {
    uint64_t m = mask_of(sf);
    a &= m, b &= m;
    uint64_t r;
    int N, Z, C, V;
    if (sf) {
        unsigned __int128 u = (unsigned __int128)a + b + carry;
        __int128 s = (__int128)(int64_t)a + (__int128)(int64_t)b + carry;
        r = (uint64_t)u;
        N = (int)(r >> 63), Z = r == 0, C = (u >> 64) != 0, V = (__int128)(int64_t)r != s;
    } else {
        uint64_t u = a + b + (uint64_t)carry;
        int64_t s = (int64_t)(int32_t)a + (int64_t)(int32_t)b + carry;
        r = u & m;
        N = (int)(r >> 31) & 1, Z = r == 0, C = (u >> 32) != 0, V = (int64_t)(int32_t)r != s;
    }
    if (flags) set_flags((uint64_t)(N << 3 | Z << 2 | C << 1 | V) << 28);
    return r;
}

void Emu::logic_flags(int sf, uint64_t r) {
    int N = (int)(sf ? r >> 63 : (r >> 31) & 1), Z = (r & mask_of(sf)) == 0;
    set_flags((uint64_t)(N << 3 | Z << 2) << 28);
}

bool Emu::cond_holds(int cond) {
    if (!flags_known) {
        if (debug) fprintf(stderr, "[emu] condition on unknown flags at %llx\n", (unsigned long long)pc);
        throw EmuFault();
    }
    int N = (int)(nzcv >> 31) & 1, Z = (int)(nzcv >> 30) & 1, C = (int)(nzcv >> 29) & 1, V = (int)(nzcv >> 28) & 1;
    bool r;
    switch (cond >> 1) {
        case 0: r = Z; break;
        case 1: r = C; break;
        case 2: r = N; break;
        case 3: r = V; break;
        case 4: r = C && !Z; break;
        case 5: r = N == V; break;
        case 6: r = N == V && !Z; break;
        default: r = true; break;
    }
    if ((cond & 1) && cond != 15) r = !r;
    return r;
}

static uint64_t ror_n(uint64_t v, int r, int size) {
    r %= size;
    if (!r) return v;
    return ((v >> r) | (v << (size - r))) & mask_of(size == 64);
}

static uint64_t shift_reg(int sf, uint64_t v, int typ, int amt) {
    int size = sf ? 64 : 32;
    v &= mask_of(sf);
    switch (typ) {
        case 0: return (v << amt) & mask_of(sf);
        case 1: return v >> amt;
        case 2: return sf ? (uint64_t)((int64_t)v >> amt) : (uint64_t)(uint32_t)((int32_t)(uint32_t)v >> amt);
        default: return ror_n(v, amt, size);
    }
}

static uint64_t extend_reg(uint64_t v, int option, int shift) {
    switch (option) {
        case 0: v = (uint8_t)v; break;
        case 1: v = (uint16_t)v; break;
        case 2: v = (uint32_t)v; break;
        case 4: v = (uint64_t)(int64_t)(int8_t)v; break;
        case 5: v = (uint64_t)(int64_t)(int16_t)v; break;
        case 6: v = (uint64_t)(int64_t)(int32_t)v; break;
        default: break;
    }
    return v << shift;
}

static int bit_len(uint64_t x) {
    int n = 0;
    while (x) n++, x >>= 1;
    return n;
}

// DecodeBitMasks; false for reserved encodings (UNDEFINED)
static bool bit_masks(int N, int imms, int immr, int size, bool immediate, uint64_t &wmask, uint64_t &tmask) {
    int len = bit_len((uint64_t)((N << 6) | (~imms & 0x3F))) - 1;
    if (len < 1) return false;
    int levels = (1 << len) - 1;
    if (immediate && (imms & levels) == levels) return false;
    int S = imms & levels, R = immr & levels, d = (S - R) & levels;
    int esize = 1 << len;
    uint64_t emask = esize == 64 ? ~0ull : (1ull << esize) - 1;
    uint64_t welem = S + 1 >= 64 ? ~0ull : (1ull << (S + 1)) - 1;
    uint64_t telem = d + 1 >= 64 ? ~0ull : (1ull << (d + 1)) - 1;
    uint64_t wr = R ? (((welem >> R) | (welem << (esize - R))) & emask) : welem;
    wmask = tmask = 0;
    for (int i = 0; i < size; i += esize) wmask |= wr << i, tmask |= telem << i;
    return true;
}

// ---------------------------------------------------------------- one instruction

void Emu::step(uint64_t at) {
    pc = at;
    unk = false;
    bool known;
    uint32_t w = (uint32_t)load(pc, 4, known);
    if (!known) unsupported(w);  // (code overwritten with an unknown value)
    uint64_t next = pc + 4;
    int op0 = (int)bits(w, 25, 4);
    int sf = (int)bits(w, 31, 1);
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5);
    uint64_t m = mask_of(sf);

    if ((op0 & 0b1110) == 0b1000) {  // data processing (imm)
        int op = (int)bits(w, 23, 3);
        if (op == 0 || op == 1) {  // ADR / ADRP
            int64_t imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21);
            wx(rd, bits(w, 31, 1) ? (pc & ~0xFFFull) + ((uint64_t)imm << 12) : pc + (uint64_t)imm);
        } else if (op == 2) {  // add/sub imm
            int opc = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1);
            uint64_t imm = bits(w, 10, 12) << (bits(w, 22, 1) ? 12 : 0);
            uint64_t a = xsp(rn);
            uint64_t r = opc ? add_with_carry(sf, a, ~imm, 1, S) : add_with_carry(sf, a, imm, 0, S);
            if (S) wx(rd, r);
            else wxsp(rd, r);
        } else if (op == 4) {  // logical imm
            int opc = (int)bits(w, 29, 2), N = (int)bits(w, 22, 1);
            uint64_t wm, tm;
            if ((!sf && N) || !bit_masks(N, (int)bits(w, 10, 6), (int)bits(w, 16, 6), sf ? 64 : 32, true, wm, tm))
                unsupported(w);
            uint64_t a = xr(rn) & m, r;
            if (opc == 0) r = a & wm;
            else if (opc == 1) r = a | wm;
            else if (opc == 2) r = a ^ wm;
            else r = a & wm, logic_flags(sf, r);
            r &= m;
            if (opc == 3) wx(rd, r);
            else wxsp(rd, r);
        } else if (op == 5) {  // move wide
            int opc = (int)bits(w, 29, 2), hw = (int)bits(w, 21, 2);
            if ((!sf && hw > 1) || opc == 1) unsupported(w);
            uint64_t imm = bits(w, 5, 16) << (hw * 16);
            if (opc == 0) wx(rd, ~imm & m);
            else if (opc == 2) wx(rd, imm);
            else {
                uint64_t old = xr(rd);
                wx(rd, ((old & ~(0xFFFFull << (hw * 16))) | imm) & m);
            }
        } else if (op == 6) {  // bitfield
            int opc = (int)bits(w, 29, 2), N = (int)bits(w, 22, 1);
            int immr = (int)bits(w, 16, 6), imms = (int)bits(w, 10, 6), size = sf ? 64 : 32;
            uint64_t wm, tm;
            if (N != sf || opc == 3 || (!sf && (immr >= 32 || imms >= 32)) || !bit_masks(N, imms, immr, size, false, wm, tm))
                unsupported(w);
            uint64_t src = xr(rn) & m, bot = ror_n(src, immr, size), r;
            if (opc == 2) r = bot & wm & tm;  // UBFM
            else if (opc == 0) {               // SBFM
                uint64_t top = ((src >> imms) & 1) ? m : 0;
                r = (top & ~tm) | (bot & wm & tm);
            } else {  // BFM
                uint64_t dst = xr(rd) & m;
                uint64_t b2 = (dst & ~wm) | (bot & wm);
                r = (dst & ~tm) | (b2 & tm);
            }
            wx(rd, r & m);
        } else if (op == 7) {  // EXTR
            int lsb = (int)bits(w, 10, 6), size = sf ? 64 : 32;
            if ((!sf && lsb >= 32) || bits(w, 22, 1) != (uint64_t)sf || bits(w, 29, 2) || bits(w, 21, 1)) unsupported(w);
            uint64_t hi = xr(rn) & m, lo = xr(rm) & m;
            wx(rd, lsb ? ((lo >> lsb) | (hi << (size - lsb))) & m : lo);
        } else {
            unsupported(w);
        }
    } else if ((op0 & 0b1110) == 0b1010) {  // branches, exception generation, system
        if (bits(w, 26, 5) == 0b00101) {  // B / BL
            uint64_t t = pc + (uint64_t)(sext(bits(w, 0, 26), 26) * 4);
            if (bits(w, 31, 1)) wxk(30, pc + 4, true);
            next = t;
        } else if (bits(w, 25, 7) == 0b0101010 && !bits(w, 4, 1) && !bits(w, 24, 1)) {  // B.cond
            if (cond_holds((int)bits(w, 0, 4))) next = pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
        } else if (bits(w, 25, 6) == 0b011010) {  // CBZ / CBNZ
            uint64_t v0 = xr((int)bits(w, 0, 5)) & m;
            need_known("branch condition");
            if ((v0 != 0) == (bool)bits(w, 24, 1)) next = pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
        } else if (bits(w, 25, 6) == 0b011011) {  // TBZ / TBNZ
            int b = (int)((bits(w, 31, 1) << 5) | bits(w, 19, 5));
            uint64_t v0 = (xr((int)bits(w, 0, 5)) >> b) & 1;
            need_known("branch condition");
            if (v0 == bits(w, 24, 1)) next = pc + (uint64_t)(sext(bits(w, 5, 14), 14) * 4);
        } else if (bits(w, 25, 7) == 0b1101011) {  // branch register
            int opc = (int)bits(w, 21, 4);
            if (bits(w, 16, 5) != 31 || bits(w, 10, 6) || bits(w, 0, 5) || opc > 2) unsupported(w);
            uint64_t t = xr(rn);
            need_known("branch target");
            if (opc == 1) wxk(30, pc + 4, true);
            next = t;
        } else if (bits(w, 22, 10) == 0b1101010100 && bits(w, 19, 3) == 0 && bits(w, 0, 5) == 31 &&
                   bits(w, 16, 3) == 3 && (bits(w, 12, 4) == 2 || bits(w, 12, 4) == 3)) {
            // hints and barriers: nothing to do here
        } else {
            unsupported(w);
        }
    } else if ((op0 & 0b0101) == 0b0100) {  // loads and stores
        if (bits(w, 26, 1)) {
            simd_ldst(w);
        } else if (bits(w, 27, 3) == 0b011 && bits(w, 24, 2) == 0) {  // literal
            int opc = (int)bits(w, 30, 2), rt = (int)bits(w, 0, 5);
            uint64_t addr = pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
            if (opc == 0) {
                uint64_t v0 = load(addr, 4, known);
                wxk(rt, v0, known);
            } else if (opc == 1) {
                uint64_t v0 = load(addr, 8, known);
                wxk(rt, v0, known);
            } else if (opc == 2) {
                uint64_t v0 = load(addr, 4, known);
                wxk(rt, (uint64_t)(int64_t)(int32_t)v0, known);
            }
            // (prfm: nothing)
        } else if (bits(w, 27, 3) == 0b101) {  // pair
            int opc = (int)bits(w, 30, 2), mode = (int)bits(w, 23, 2), L = (int)bits(w, 22, 1);
            int rt = (int)bits(w, 0, 5), rt2 = (int)bits(w, 10, 5);
            if (opc == 3 || (opc == 1 && (!L || mode == 0))) unsupported(w);  // (mode 0: LDNP / STNP)
            int scale = opc == 2 ? 3 : 2, esz = 1 << scale;
            int64_t off = sext(bits(w, 15, 7), 7) * esz;
            uint64_t b = xsp(rn);
            need_known("address");
            uint64_t ea = mode == 1 ? b : b + (uint64_t)off;
            if (L) {
                bool k1, k2;
                uint64_t a = load(ea, esz, k1), c2 = load(ea + esz, esz, k2);
                if (opc == 1) a = (uint64_t)(int64_t)(int32_t)a, c2 = (uint64_t)(int64_t)(int32_t)c2;
                if (mode & 1) wxspk(rn, mode == 1 ? b + (uint64_t)off : ea, true);
                wxk(rt, a, k1);
                wxk(rt2, c2, k2);
            } else {
                unk = false;
                uint64_t a = xr(rt);
                bool k1 = !unk;
                unk = false;
                uint64_t c2 = xr(rt2);
                bool k2 = !unk;
                store(ea, esz, a, k1);
                store(ea + esz, esz, c2, k2);
                if (mode & 1) wxspk(rn, mode == 1 ? b + (uint64_t)off : ea, true);
            }
        } else if (bits(w, 27, 3) == 0b111) {  // register: unsigned offset, unscaled, pre / post, register offset
            int size = (int)bits(w, 30, 2), opc = (int)bits(w, 22, 2), rt = (int)bits(w, 0, 5);
            uint64_t b = xsp(rn), ea = 0;
            int wbmode = 0;  // 1 post, 2 pre
            int64_t imm9 = 0;
            if (bits(w, 24, 2) == 1) {
                ea = b + (bits(w, 10, 12) << size);
            } else if (bits(w, 24, 2) == 0) {
                int kind = (int)bits(w, 10, 2);
                if (bits(w, 21, 1)) {
                    if (kind != 2) unsupported(w);
                    int option = (int)bits(w, 13, 3);
                    if (!(option & 2)) unsupported(w);
                    ea = b + extend_reg(xr(rm), option, bits(w, 12, 1) ? size : 0);
                } else {
                    imm9 = sext(bits(w, 12, 9), 9);
                    if (kind == 2) unsupported(w);  // unprivileged
                    if (kind == 1) ea = b, wbmode = 1;
                    else ea = b + (uint64_t)imm9, wbmode = kind == 3 ? 2 : 0;
                }
            } else {
                unsupported(w);
            }
            need_known("address");
            int nbytes = 1 << size;
            uint64_t nb_addr = wbmode == 1 ? b + (uint64_t)imm9 : ea;
            if (opc == 0) {
                uint64_t v0 = xr(rt);
                store(ea, nbytes, v0, !unk);
                if (wbmode) wxspk(rn, nb_addr, true);
            } else if (opc == 1) {
                uint64_t v0 = load(ea, nbytes, known);
                if (wbmode) wxspk(rn, nb_addr, true);
                wxk(rt, v0, known);
            } else if (size == 3) {
                if (opc != 2) unsupported(w);  // (prfm: nothing)
            } else if (size == 2 && opc == 3) {
                unsupported(w);
            } else {
                uint64_t v0 = load(ea, nbytes, known);
                int nb = 8 << size;
                v0 = (uint64_t)((int64_t)(v0 << (64 - nb)) >> (64 - nb));
                if (wbmode) wxspk(rn, nb_addr, true);
                wxk(rt, opc == 2 ? v0 : v0 & 0xFFFFFFFFull, known);
            }
        } else {
            unsupported(w);  // exclusives, atomics
        }
    } else if ((op0 & 0b0111) == 0b0101) {  // data processing (register)
        int size = sf ? 64 : 32;
        if (!bits(w, 28, 1)) {
            if (bits(w, 24, 4) == 0b1010) {  // logical shifted
                int opc = (int)bits(w, 29, 2), sh = (int)bits(w, 22, 2), N = (int)bits(w, 21, 1), amt = (int)bits(w, 10, 6);
                if (!sf && amt >= 32) unsupported(w);
                uint64_t b = shift_reg(sf, xr(rm), sh, amt);
                if (N) b = ~b & m;
                uint64_t a = xr(rn) & m, r;
                if (opc == 1) r = a | b;
                else if (opc == 2) r = a ^ b;
                else r = a & b;
                if (opc == 3) logic_flags(sf, r);
                wx(rd, r & m);
            } else if (bits(w, 24, 4) == 0b1011) {  // add/sub shifted or extended
                int op = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1);
                uint64_t a, b;
                bool to_sp = false;
                if (!bits(w, 21, 1)) {
                    int sh = (int)bits(w, 22, 2), amt = (int)bits(w, 10, 6);
                    if (sh == 3 || (!sf && amt >= 32)) unsupported(w);
                    a = xr(rn);
                    b = shift_reg(sf, xr(rm), sh, amt);
                } else {
                    int amt = (int)bits(w, 10, 3);
                    if (bits(w, 22, 2) || amt > 4) unsupported(w);
                    a = xsp(rn);
                    b = extend_reg(xr(rm), (int)bits(w, 13, 3), amt);
                    to_sp = !S;
                }
                uint64_t r = op ? add_with_carry(sf, a, ~b, 1, S) : add_with_carry(sf, a, b, 0, S);
                if (to_sp) wxsp(rd, r);
                else wx(rd, r);
            } else {
                unsupported(w);
            }
        } else {
            int op2 = (int)bits(w, 21, 4);
            if (bits(w, 24, 4) == 0b1011) {  // 3-source
                int op31 = (int)bits(w, 21, 3), o0 = (int)bits(w, 15, 1), ra = (int)bits(w, 10, 5);
                if (bits(w, 29, 2)) unsupported(w);
                uint64_t r = 0, n = xr(rn), mm = xr(rm);
                if (op31 == 0) {
                    uint64_t p = (n * mm) & m, acc = xr(ra);
                    r = (o0 ? acc - p : acc + p) & m;
                } else if ((op31 == 1 || op31 == 5) && sf) {
                    uint64_t p = op31 == 1 ? (uint64_t)((int64_t)(int32_t)n * (int64_t)(int32_t)mm)
                                           : (uint64_t)(uint32_t)n * (uint32_t)mm;
                    uint64_t acc = xr(ra);
                    r = o0 ? acc - p : acc + p;
                } else if (op31 == 2 && sf && !o0) {
                    r = (uint64_t)(((__int128)(int64_t)n * (__int128)(int64_t)mm) >> 64);
                } else if (op31 == 6 && sf && !o0) {
                    r = (uint64_t)(((unsigned __int128)n * mm) >> 64);
                } else {
                    unsupported(w);
                }
                wx(rd, r);
            } else if (bits(w, 24, 4) == 0b1010 && op2 == 0) {  // add/sub with carry
                if (bits(w, 10, 6)) unsupported(w);
                int op = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1);
                unk |= !flags_known;
                int carry = (int)(nzcv >> 29) & 1;
                uint64_t b = op ? ~xr(rm) : xr(rm);
                wx(rd, add_with_carry(sf, xr(rn), b, carry, S));
            } else if (bits(w, 24, 4) == 0b1010 && op2 == 2) {  // conditional compare
                if (!bits(w, 29, 1) || bits(w, 10, 1) || bits(w, 4, 1)) unsupported(w);
                int op = (int)bits(w, 30, 1), cond = (int)bits(w, 12, 4);
                if (!flags_known) {  // (FCMP before: the flags stay unknown)
                    xr(rn), xr(rm);
                } else if (cond_holds(cond)) {
                    uint64_t b = bits(w, 11, 1) ? (uint64_t)rm : xr(rm);
                    uint64_t a = xr(rn);
                    if (op) add_with_carry(sf, a, ~b, 1, true);
                    else add_with_carry(sf, a, b, 0, true);
                } else {
                    set_flags(bits(w, 0, 4) << 28);
                }
            } else if (bits(w, 24, 4) == 0b1010 && op2 == 4) {  // conditional select
                int op = (int)bits(w, 30, 1), o2 = (int)bits(w, 10, 2), cond = (int)bits(w, 12, 4);
                if (bits(w, 29, 1) || o2 > 1) unsupported(w);
                uint64_t r;
                if (!flags_known) {  // a select on FCMP's flags: an unknown value
                    r = xr(rn), xr(rm);
                    unk = true;
                } else if (cond_holds(cond)) r = xr(rn);
                else {
                    r = xr(rm);
                    if (op) r = ~r;
                    if (o2) r += 1;
                }
                wx(rd, r & m);
            } else if (bits(w, 24, 4) == 0b1010 && op2 == 6) {
                int opcode = (int)bits(w, 10, 6);
                if (bits(w, 29, 1)) unsupported(w);
                uint64_t a = xr(rn) & m, r = 0;
                if (bits(w, 30, 1)) {  // 1-source
                    if (bits(w, 16, 5)) unsupported(w);
                    if (opcode == 0) {
                        for (int i = 0; i < size; i++)
                            if ((a >> i) & 1) r |= 1ull << (size - 1 - i);
                    } else if (opcode == 1 || opcode == 2 || (opcode == 3 && sf)) {
                        int cont = opcode == 1 ? 16 : opcode == 2 ? 32 : 64;  // REV16, REV32 / REV (32-bit), REV
                        for (int base0 = 0; base0 < size; base0 += cont)
                            for (int i = 0; i < cont; i += 8)
                                r |= ((a >> (base0 + i)) & 0xFF) << (base0 + cont - 8 - i);
                    } else if (opcode == 4) {
                        r = (uint64_t)(size - bit_len(a));
                    } else if (opcode == 5) {
                        uint64_t t = ((a >> 1) ^ a) & (m >> 1);
                        r = (uint64_t)(size - 1 - bit_len(t));
                    } else {
                        unsupported(w);
                    }
                } else {  // 2-source
                    uint64_t b = xr(rm) & m;
                    if (opcode == 2) r = b ? a / b : 0;
                    else if (opcode == 3) {
                        if (!b) r = 0;
                        else if (sf) {
                            int64_t sa = (int64_t)a, sb = (int64_t)b;
                            r = (sa == INT64_MIN && sb == -1) ? a : (uint64_t)(sa / sb);
                        } else {
                            int64_t sa = (int32_t)(uint32_t)a, sb = (int32_t)(uint32_t)b;
                            r = (uint64_t)(sa / sb);
                        }
                    } else if (opcode >= 8 && opcode <= 11) {
                        r = shift_reg(sf, a, opcode - 8, (int)(b & (uint64_t)(size - 1)));
                    } else {
                        unsupported(w);  // (CRC32, PAC)
                    }
                }
                wx(rd, r & m);
            } else {
                unsupported(w);
            }
        }
    } else if ((op0 & 0b0111) == 0b0111) {
        fp_simd(w);
    } else {
        unsupported(w);  // unallocated
    }
    pc = next;
}

// ---------------------------------------------------------------- FP / SIMD

// FP/SIMD loads and stores: the memory effects (and faults) exactly; the values are kept, so a spill and reload stays
// known.
void Emu::simd_ldst(uint32_t w) {
    int rt = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5);
    if (bits(w, 27, 3) == 0b011 && bits(w, 24, 2) == 0) {  // literal
        int opc = (int)bits(w, 30, 2);
        if (opc == 3) unsupported(w);
        vload(rt, pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4), 4 << opc);
        return;
    }
    if (bits(w, 27, 3) == 0b101) {  // pair
        int opc = (int)bits(w, 30, 2), mode = (int)bits(w, 23, 2), L = (int)bits(w, 22, 1), rt2 = (int)bits(w, 10, 5);
        if (opc == 3) unsupported(w);
        int esz = 4 << opc;
        int64_t off = sext(bits(w, 15, 7), 7) * esz;
        uint64_t b = xsp(rn);
        need_known("address");
        uint64_t ea = mode == 1 ? b : b + (uint64_t)off;
        if (L) {
            vload(rt, ea, esz);
            vload(rt2, ea + esz, esz);
        } else {
            vstore(rt, ea, esz);
            vstore(rt2, ea + esz, esz);
        }
        if (mode & 1) wxspk(rn, mode == 1 ? b + (uint64_t)off : ea, true);
        return;
    }
    if (bits(w, 27, 3) == 0b111) {  // register
        int size = (int)bits(w, 30, 2), opc = (int)bits(w, 22, 2);
        if ((opc & 2) && size) unsupported(w);
        int esz = (opc & 2) ? 16 : 1 << size;
        bool L = opc & 1;
        uint64_t b = xsp(rn), ea = 0;
        int wbmode = 0;
        int64_t imm9 = 0;
        if (bits(w, 24, 2) == 1) {
            ea = b + bits(w, 10, 12) * (uint64_t)esz;
        } else if (bits(w, 24, 2) == 0) {
            int kind = (int)bits(w, 10, 2);
            if (bits(w, 21, 1)) {
                int option = (int)bits(w, 13, 3);
                if (kind != 2 || !(option & 2)) unsupported(w);
                int sh = esz == 16 ? 4 : size;
                ea = b + extend_reg(xr(rm), option, bits(w, 12, 1) ? sh : 0);
            } else {
                if (kind == 2) unsupported(w);
                imm9 = sext(bits(w, 12, 9), 9);
                if (kind == 1) ea = b, wbmode = 1;
                else ea = b + (uint64_t)imm9, wbmode = kind == 3 ? 2 : 0;
            }
        } else {
            unsupported(w);
        }
        need_known("address");
        if (L) vload(rt, ea, esz);
        else vstore(rt, ea, esz);
        if (wbmode) wxspk(rn, wbmode == 1 ? b + (uint64_t)imm9 : ea, true);
        return;
    }
    if (bits(w, 31, 1) == 0 && bits(w, 24, 6) == 0b001100 && bits(w, 21, 1) == 0) {  // LD1 / ST1 (multiple)
        int n = 0;
        switch (bits(w, 12, 4)) {
            case 7: n = 1; break;
            case 10: n = 2; break;
            case 6: n = 3; break;
            case 2: n = 4; break;
            default: unsupported(w);  // (LD2..LD4 interleave)
        }
        int esz = bits(w, 30, 1) ? 16 : 8;
        uint64_t b = xsp(rn);
        need_known("address");
        for (int i = 0; i < n; i++) {
            int r = (rt + i) % 32;
            if (bits(w, 22, 1)) vload(r, b + (uint64_t)(i * esz), esz);
            else vstore(r, b + (uint64_t)(i * esz), esz);
        }
        if (bits(w, 23, 1)) {
            uint64_t inc = rm == 31 ? (uint64_t)(n * esz) : xr(rm);
            wxsp(rn, b + inc);
        }
        return;
    }
    unsupported(w);
}

// FP/SIMD data processing. Moves between integer and FP registers keep the value; anything else makes its result
// unknown (and FCMP the flags). Encodings SimdC refuses are treated as not instructions.
void Emu::fp_simd(uint32_t w) {
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5);
    try {
        SimdC().dp(w, pc);
    } catch (const Unsupported &) {
        unsupported(w);
    }
    if ((w & 0x5F20FC00) == 0x1E200000) {  // FP <-> integer
        int sf = (int)bits(w, 31, 1), type = (int)bits(w, 22, 2), rmode = (int)bits(w, 19, 2), opcode = (int)bits(w, 16, 3);
        bool plain = rmode == 0 && ((sf && type == 1) || (!sf && type == 0));
        if (opcode == 7 && plain) {  // FMOV general -> FP
            uint64_t g = xr(rn);
            v[rd] = sf ? g : (uint32_t)g;
            vknown[rd] = !unk;
        } else if (opcode == 6 && plain) {  // FMOV FP -> general
            uint64_t b = (uint64_t)v[rn];
            wxk(rd, sf ? b : (uint32_t)b, vknown[rn]);
        } else if (sf && type == 2 && rmode == 1 && opcode == 7) {  // FMOV Vd.D[1], Xn
            uint64_t g = xr(rn);
            v[rd] = (v[rd] & (((unsigned __int128)1 << 64) - 1)) | (unsigned __int128)g << 64;
            vknown[rd] = vknown[rd] && !unk;
        } else if (sf && type == 2 && rmode == 1 && opcode == 6) {  // FMOV Xd, Vn.D[1]
            wxk(rd, (uint64_t)(v[rn] >> 64), vknown[rn]);
        } else if (opcode == 0 || opcode == 1 || opcode == 4 || opcode == 5) {  // FCVT* to integer: not computed
            wxk(rd, 0, false);
            if (!sf) wxk(rd, 0, false);
        } else {  // SCVTF / UCVTF
            vknown[rd] = false;
        }
        return;
    }
    if ((w & 0x9FE08400) == 0x0E000400 && bits(w, 29, 1) == 0) {  // AdvSIMD copy
        int imm4 = (int)bits(w, 11, 4);
        if (imm4 == 5 || imm4 == 7) {  // SMOV / UMOV: not computed
            wxk(rd, 0, false);
            return;
        }
    }
    if ((w & 0x5F203C00) == 0x1E202000 || (w & 0x5F200C00) == 0x1E200400) {  // FCMP / FCCMP
        flags_known = false;
        return;
    }
    vknown[rd] = false;
}

}  // namespace lasgen
