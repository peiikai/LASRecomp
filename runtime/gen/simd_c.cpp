// Portable C lowering of AArch64 FP / AdvSIMD / crypto / CRC32 instructions (lifter simd "c"; port of tools/simd_c.py).
//
// Every encoding is decoded here at lift time and turned into C over the Cpu struct, calling the static inline
// helpers in runtime/simd.h with constant fields (sizes, lanes, ops), so clang folds the helpers' switches. Covers
// ARMv8.0-A + Crypto + CRC32 (the Switch's Cortex-A57); unallocated or later-architecture encodings (FP16 arithmetic,
// RDM, dot product, FCMA, JSCVT, ...) raise Unsupported.
#include "gen_internal.hpp"

#include <algorithm>

namespace lasgen {

namespace {

using std::string;
using IntList = std::vector<int>;

// C name of a lane op (enum in runtime/simd.h), with the unsigned flag.
string lop(const string &name, int u = 0) { return "LOP_" + name + (u ? " | LOP_U" : ""); }

const char *vu(int sz) {
    static const char *const t[] = {"vu8", "vu16", "vu32", "vu64"};
    return t[sz];
}
const char *vs(int sz) {
    static const char *const t[] = {"vs8", "vs16", "vs32", "vs64"};
    return t[sz];
}
const char *ut(int sz) {
    static const char *const t[] = {"uint8_t", "uint16_t", "uint32_t", "uint64_t"};
    return t[sz];
}

string VR(const string &T, int n) { return cat("VREG(", T, ", ", n, ")"); }
string VQ(const string &T, int Q, int n) { return Q ? cat("VREG(", T, ", ", n, ")") : cat("VQ(", T, ", 0, ", n, ")"); }
string VWr(const string &T, int Q, int d, const string &e) { return cat("VW(", T, ", ", Q, ", ", d, ", ", e, ");"); }

string upper(string s) {
    for (auto &ch : s) ch = (char)toupper((unsigned char)ch);
    return s;
}

string cond(int c) { return c < 14 ? "COND_" + upper(COND_NAMES[c]) + "(c)" : "1"; }

string gen(const string &op, int sz, int ne, int d, int szd, int offd, int keep, int n, int szn, int offn, int m = -1,
           int szm = 0, int offm = 0, int bidx = -1, int64_t bimm = 0) {
    return cat("sl_gen(c, ", op, ", ", sz, ", ", ne, ", ", d, ", ", szd, ", ", offd, ", ", keep, ", ", n, ", ", szn, ", ",
               offn, ", ", m, ", ", szm, ", ", offm, ", ", bidx, ", ", hx_signed(bimm), ");");
}

// Lane-wise op, all operands the same size.
string same(const string &op, int sz, int ne, int d, int n, int m = -1, int64_t bimm = 0, int bidx = -1) {
    return gen(op, sz, ne, d, sz, 0, 0, n, sz, 0, m, sz, 0, bidx, bimm);
}

int lanes(int Q, int sz) { return (8 << Q) >> sz; }

string shuf(const string &, const string &a, const string &b, const IntList &idx) {
    std::vector<string> s;
    for (int i : idx) s.push_back(D(i));
    return "SHUF(" + a + ", " + b + ", " + join(s, ", ") + ")";
}

typedef unsigned __int128 u128_t;

string u128(u128_t v) {
    uint64_t lo = (uint64_t)v, hi = (uint64_t)(v >> 64);
    return hi ? "((u128)" + hx(hi) + " << 64 | " + hx(lo) + ")" : hx(lo);
}

struct FTe {
    const char *F, *VT, *VU, *T, *SF, *SU, *SET;
};
const FTe FT[2] = {{"s", "vf32", "vu32", "float", "VSF32", "VS32", "FP_SETS"},
                   {"d", "vf64", "vu64", "double", "VSF64", "VS64", "FP_SETD"}};

// key = U << 8 | a << 5 | opcode
int k3(int U, int a, int opc) { return U << 8 | a << 5 | opc; }

const char *fop3(int U, int a, int opc) {  // vector three-same FP: (U, a, opcode) -> FOP
    switch (k3(U, a, opc)) {
        case 0 << 8 | 0 << 5 | 0b11000: return "FOP_MAXNM";
        case 0 << 8 | 0 << 5 | 0b11010: return "FOP_ADD";
        case 0 << 8 | 0 << 5 | 0b11011: return "FOP_MULX";
        case 0 << 8 | 0 << 5 | 0b11110: return "FOP_MAX";
        case 0 << 8 | 0 << 5 | 0b11111: return "FOP_RECPS";
        case 0 << 8 | 1 << 5 | 0b11000: return "FOP_MINNM";
        case 0 << 8 | 1 << 5 | 0b11010: return "FOP_SUB";
        case 0 << 8 | 1 << 5 | 0b11110: return "FOP_MIN";
        case 0 << 8 | 1 << 5 | 0b11111: return "FOP_RSQRTS";
        case 1 << 8 | 0 << 5 | 0b11011: return "FOP_MUL";
        case 1 << 8 | 0 << 5 | 0b11111: return "FOP_DIV";
        case 1 << 8 | 1 << 5 | 0b11010: return "FOP_ABD";
    }
    return nullptr;
}

const char *fpairv(int U, int a, int opc) {
    switch (k3(U, a, opc)) {
        case 1 << 8 | 0 << 5 | 0b11000: return "FOP_MAXNM";
        case 1 << 8 | 1 << 5 | 0b11000: return "FOP_MINNM";
        case 1 << 8 | 0 << 5 | 0b11010: return "FOP_ADD";
        case 1 << 8 | 0 << 5 | 0b11110: return "FOP_MAX";
        case 1 << 8 | 1 << 5 | 0b11110: return "FOP_MIN";
    }
    return nullptr;
}

int fcmp3(int U, int a, int opc) {
    switch (k3(U, a, opc)) {
        case 0 << 8 | 0 << 5 | 0b11100: return 0;
        case 1 << 8 | 0 << 5 | 0b11100: return 1;
        case 1 << 8 | 1 << 5 | 0b11100: return 2;
        case 1 << 8 | 0 << 5 | 0b11101: return 3;
        case 1 << 8 | 1 << 5 | 0b11101: return 4;
    }
    return -1;
}

uint64_t vfp_expand_imm(uint64_t imm8, int N) {
    int E = N == 32 ? 8 : 11;
    int F = N - E - 1;
    uint64_t sign = imm8 >> 7;
    uint64_t b6 = (imm8 >> 6) & 1;
    uint64_t exp = ((b6 ^ 1) << (E - 1)) | ((b6 ? ((1ull << (E - 3)) - 1) : 0) << 2) | ((imm8 >> 4) & 3);
    uint64_t frac = (imm8 & 15) << (F - 4);
    return (sign << (N - 1)) | (exp << F) | frac;
}

uint64_t simd_expand_imm(int op, int cmode, uint64_t imm8) {
    int c = cmode >> 1;
    uint64_t imm = 0;
    if (c == 0) {
        imm = (imm8 << 32) | imm8;
    } else if (c == 1) {
        imm = ((imm8 << 8) << 32) | (imm8 << 8);
    } else if (c == 2) {
        imm = ((imm8 << 16) << 32) | (imm8 << 16);
    } else if (c == 3) {
        imm = ((imm8 << 24) << 32) | (imm8 << 24);
    } else if (c == 4) {
        for (int s = 0; s < 64; s += 16) imm += imm8 << s;
    } else if (c == 5) {
        for (int s = 0; s < 64; s += 16) imm += (imm8 << 8) << s;
    } else if (c == 6) {
        uint64_t v = !(cmode & 1) ? ((imm8 << 8) | 0xFF) : ((imm8 << 16) | 0xFFFF);
        imm = (v << 32) | v;
    } else {
        if (!(cmode & 1) && !op) {
            for (int s = 0; s < 64; s += 8) imm += imm8 << s;
        } else if (!(cmode & 1) && op) {
            for (int i = 0; i < 8; i++)
                if ((imm8 >> i) & 1) imm |= 0xFFull << (8 * i);
        } else if ((cmode & 1) && !op) {
            uint64_t v = vfp_expand_imm(imm8, 32);
            imm = (v << 32) | v;
        } else {
            imm = vfp_expand_imm(imm8, 64);
        }
    }
    return imm;
}

int bit_length(uint64_t x) {
    int n = 0;
    while (x) n++, x >>= 1;
    return n;
}

[[noreturn]] void key_error() { throw GenError("KeyError"); }

// Lane indices (into SHUF(a, b)) of the left / right operands of a pairwise op; for Q=0 the padding lanes point at
// a's upper half.
void pair_idx(int Q, int sz, IntList &ev, IntList &od) {
    int ne = lanes(Q, sz), NL = 16 >> sz;
    IntList src;
    for (int i = 0; i < ne; i++) src.push_back(i);
    for (int i = 0; i < ne; i++) src.push_back(NL + i);
    ev.clear(), od.clear();
    for (int k = 0; k < ne; k++) ev.push_back(src[2 * k]), od.push_back(src[2 * k + 1]);
    for (int k = 0; k < NL - ne; k++) ev.push_back(ne % NL), od.push_back(ne % NL);
}

std::pair<int, int> imm5_split(int imm5) {
    if (!(imm5 & 15)) throw Unsupported("copy imm5");
    int sz = bit_length((uint64_t)(imm5 & -imm5)) - 1;
    return {sz, imm5 >> (sz + 1)};
}

int ftype(uint32_t w) {
    int t = (int)bits(w, 22, 2);
    if (t > 1) throw Unsupported("fp type");
    return t;
}

}  // namespace

// ------------------------------------------------------------ top level

std::string SimdC::dp(uint32_t w, uint64_t) {
    int op0 = (int)bits(w, 28, 4), op1 = (int)bits(w, 23, 2), op2 = (int)bits(w, 19, 4), op3 = (int)bits(w, 10, 9);
    if (op0 == 0b0100 && !(op1 & 2) && (op2 & 7) == 5 && (op3 & 0b110000011) == 0b10) return crypto_aes(w);
    if (op0 == 0b0101 && !(op1 & 2) && !(op2 & 4) && (op3 & 0b000100011) == 0) return crypto_sha3(w);
    if (op0 == 0b0101 && !(op1 & 2) && (op2 & 7) == 5 && (op3 & 0b110000011) == 0b10) return crypto_sha2(w);
    if ((op0 & 0b1101) == 0b0101) {  // AdvSIMD scalar
        if (op1 == 0 && !(op2 & 0b1100) && (op3 & 0b000100001) == 1) return scalar_copy(w);
        if (!(op1 & 2) && (op2 & 7) == 4 && (op3 & 0b110000011) == 0b10) return misc2(w, true);
        if (!(op1 & 2) && (op2 & 7) == 6 && (op3 & 0b110000011) == 0b10) return scalar_pairwise(w);
        if (!(op1 & 2) && (op2 & 4) && (op3 & 3) == 0) return three_diff(w, true);
        if (!(op1 & 2) && (op2 & 4) && (op3 & 1)) return three_same(w, true);
        if (op1 == 2 && (op3 & 1)) return shift_imm(w, true);
        if ((op1 & 2) && !(op3 & 1)) return indexed(w, true);
        throw Unsupported("simd scalar " + hex08(w));
    }
    if ((op0 & 0b1011) == 0 && !(op1 & 2) && !(op2 & 4) && (op3 & 0b000100011) == 0) return tbl(w);
    if ((op0 & 0b1011) == 0 && !(op1 & 2) && !(op2 & 4) && (op3 & 0b000100011) == 2) return permute(w);
    if ((op0 & 0b1011) == 0b0010 && !(op1 & 2) && !(op2 & 4) && (op3 & 0b000100001) == 0) return ext(w);
    if ((op0 & 0b1001) == 0) {  // AdvSIMD vector
        if (op1 == 0 && !(op2 & 0b1100) && (op3 & 0b000100001) == 1) return copy(w);
        if (!(op1 & 2) && (op2 & 4) && (op3 & 1)) return three_same(w);
        if (!(op1 & 2) && (op2 & 4) && (op3 & 3) == 0) return three_diff(w);
        if (!(op1 & 2) && (op2 & 7) == 4 && (op3 & 0b110000011) == 0b10) return misc2(w);
        if (!(op1 & 2) && (op2 & 7) == 6 && (op3 & 0b110000011) == 0b10) return across(w);
        if (op1 == 2 && op2 == 0 && (op3 & 1)) return modimm(w);
        if (op1 == 2 && op2 && (op3 & 1)) return shift_imm(w);
        if ((op1 & 2) && !(op3 & 1)) return indexed(w);
        throw Unsupported("simd vector " + hex08(w));
    }
    if ((op0 & 0b0101) == 0b0001) {  // scalar FP
        if (bits(w, 29, 1) || (bits(w, 31, 1) && ((op1 & 2) || ((op2 & 4) && (op3 & 0b111111)))))
            throw Unsupported("fp M/S");
        if (op1 & 2) return fp_dp3(w);
        if (!(op2 & 4)) return fp_fixed(w);
        if ((op3 & 0b111111) == 0) return fp_int(w);
        if ((op3 & 0b11111) == 0b10000) return fp_dp1(w);
        if ((op3 & 0b1111) == 0b1000) return fp_cmp(w);
        if ((op3 & 0b111) == 0b100) return fp_imm(w);
        if ((op3 & 3) == 1) return fp_ccmp(w);
        if ((op3 & 3) == 2) return fp_dp2(w);
        if ((op3 & 3) == 3) return fp_csel(w);
        throw Unsupported("fp op3");
    }
    throw Unsupported("fp/simd " + hex08(w));
}

// ------------------------------------------------------------ crypto

std::string SimdC::crypto_aes(uint32_t w) {
    if (bits(w, 22, 2)) throw Unsupported("aes size");
    int opc = (int)bits(w, 12, 5);
    if (opc < 4 || opc > 7) throw Unsupported("aes opcode");
    return cat("sv_aes(c, ", opc - 4, ", ", (int)bits(w, 0, 5), ", ", (int)bits(w, 5, 5), ");");
}

std::string SimdC::crypto_sha3(uint32_t w) {
    if (bits(w, 22, 2)) throw Unsupported("sha size");
    int opc = (int)bits(w, 12, 3), d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5), m = (int)bits(w, 16, 5);
    if (opc < 3) return cat("sv_sha1hash(c, ", opc, ", ", d, ", ", n, ", ", m, ");");
    if (opc == 3) return cat("sv_sha_misc(c, 0, ", d, ", ", n, ", ", m, ");");
    if (opc == 4 || opc == 5) return cat("sv_sha256hash(c, ", (int)(opc == 4), ", ", d, ", ", n, ", ", m, ");");
    if (opc == 6) return cat("sv_sha_misc(c, 3, ", d, ", ", n, ", ", m, ");");
    throw Unsupported("sha3 opcode");
}

std::string SimdC::crypto_sha2(uint32_t w) {
    if (bits(w, 22, 2)) throw Unsupported("sha size");
    int opc = (int)bits(w, 12, 5), d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5);
    int op = opc == 0 ? 4 : opc == 1 ? 1 : opc == 2 ? 2 : -1;
    if (op < 0) throw Unsupported("sha2 opcode");
    return cat("sv_sha_misc(c, ", op, ", ", d, ", ", n, ", 0);");
}

// ------------------------------------------------------------ copy / permute / table

std::string SimdC::scalar_copy(uint32_t w) {
    if (bits(w, 29, 1) || bits(w, 11, 4)) throw Unsupported("scalar copy");
    auto [sz, idx] = imm5_split((int)bits(w, 16, 5));
    if (sz > 3) throw Unsupported("scalar dup size");
    int d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5);
    static const char *const P[] = {"uint8_t", "sa16", "sa32", "sa64"};
    return cat("c->v[", d, "] = ((", P[sz], " *)&c->v[", n, "])[", idx, "];");
}

std::string SimdC::copy(uint32_t w) {
    int Q = (int)bits(w, 30, 1), op = (int)bits(w, 29, 1), imm5 = (int)bits(w, 16, 5), imm4 = (int)bits(w, 11, 4);
    int d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5);
    auto [sz, idx] = imm5_split(imm5);
    if (sz > 3) throw Unsupported("copy size");
    static const char *const Ps[] = {"uint8_t", "sa16", "sa32", "sa64"};
    string P = Ps[sz];
    if (op) {
        if (!Q) throw Unsupported("ins elem Q");
        int idx2 = imm4 >> sz;
        return cat("((", P, " *)&c->v[", d, "])[", idx, "] = ((", P, " *)&c->v[", n, "])[", idx2, "];");
    }
    if (imm4 == 0) {  // DUP (element)
        if (sz == 3 && !Q) throw Unsupported("dup 1d");
        int nl = 16 >> sz;
        return VWr(vu(sz), Q, d, shuf(vu(sz), VR(vu(sz), n), VR(vu(sz), n), IntList(nl, idx)));
    }
    if (imm4 == 1) {  // DUP (general)
        if (sz == 3 && !Q) throw Unsupported("dup 1d");
        return VWr(vu(sz), Q, d, cat("(", vu(sz), "){} + (", ut(sz), ")", X(n)));
    }
    if (imm4 == 3) {  // INS (general)
        if (!Q) throw Unsupported("ins Q");
        return cat("((", P, " *)&c->v[", d, "])[", idx, "] = (", ut(sz), ")", X(n), ";");
    }
    if (imm4 == 5) {  // SMOV
        if (sz >= 2 + Q) throw Unsupported("smov size");
        static const char *const st[] = {"int8_t", "int16_t", "int32_t"};
        return set_reg(Q, d, cat("(uint64_t)(int64_t)(", st[sz], ")((", P, " *)&c->v[", n, "])[", idx, "]"));
    }
    if (imm4 == 7) {  // UMOV
        if ((Q && sz != 3) || (!Q && sz == 3)) throw Unsupported("umov size");
        return set_reg(Q, d, cat("((", P, " *)&c->v[", n, "])[", idx, "]"));
    }
    throw Unsupported("copy imm4");
}

std::string SimdC::permute(uint32_t w) {
    int Q = (int)bits(w, 30, 1), sz = (int)bits(w, 22, 2), m = (int)bits(w, 16, 5), opc = (int)bits(w, 12, 3),
        n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (opc == 0 || opc == 4 || (sz == 3 && !Q)) throw Unsupported("permute");
    int ne = lanes(Q, sz), NL = 16 >> sz;
    int part = opc >> 2;
    int kind = opc & 3;
    IntList idx;
    for (int i = 0; i < ne; i++) {
        if (kind == 1) {  // UZP
            int j = 2 * i + part;
            idx.push_back(j < ne ? j : NL + j - ne);
        } else if (kind == 2) {  // TRN
            int p = i & ~1;
            idx.push_back(!(i & 1) ? p + part : NL + p + part);
        } else {  // ZIP
            int base = part * ne / 2;
            idx.push_back(!(i & 1) ? base + i / 2 : NL + base + i / 2);
        }
    }
    for (int i = 0; i < NL - ne; i++) idx.push_back(0);
    return VWr(vu(sz), Q, d, shuf(vu(sz), VR(vu(sz), n), VR(vu(sz), m), idx));
}

std::string SimdC::ext(uint32_t w) {
    int Q = (int)bits(w, 30, 1), m = (int)bits(w, 16, 5), imm4 = (int)bits(w, 11, 4), n = (int)bits(w, 5, 5),
        d = (int)bits(w, 0, 5);
    if (bits(w, 22, 2) || (!Q && imm4 >= 8)) throw Unsupported("ext");
    IntList idx;
    if (Q) {
        for (int i = 0; i < 16; i++) idx.push_back(imm4 + i);
    } else {
        for (int i = 0; i < 8; i++) idx.push_back(imm4 + i < 8 ? imm4 + i : 16 + imm4 + i - 8);
        for (int i = 0; i < 8; i++) idx.push_back(0);
    }
    return VWr("vu8", Q, d, shuf("vu8", VR("vu8", n), VR("vu8", m), idx));
}

std::string SimdC::tbl(uint32_t w) {
    int Q = (int)bits(w, 30, 1), m = (int)bits(w, 16, 5), ln = (int)bits(w, 13, 2), op = (int)bits(w, 12, 1),
        n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (bits(w, 22, 2)) throw Unsupported("tbl op2");
    return cat("sv_tbl(c, ", Q, ", ", ln, ", ", op, ", ", d, ", ", n, ", ", m, ");");
}

// ------------------------------------------------------------ three same

std::string SimdC::three_same(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2), m = (int)bits(w, 16, 5),
        opc = (int)bits(w, 11, 5), n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (opc >= 0b11000) return three_same_fp(w, scalar);
    if (scalar) return three_same_scalar(U, sz, opc, d, n, m);
    if (sz == 3 && !Q && opc != 3) throw Unsupported("3same 1d");
    int ne = lanes(Q, sz);
    string T = vu(sz), S = vs(sz);
    string a = VR(T, n), b = VR(T, m), o = VR(T, d);
    string sa = VR(S, n), sb = VR(S, m);
    string x = U ? a : sa, y = U ? b : sb;
    if ((opc == 0 || opc == 2 || opc == 4 || opc == 14 || opc == 15 || opc == 12 || opc == 13) && sz == 3)
        throw Unsupported("3same size");
    if (opc == 0) return same(lop("HADD", U), sz, ne, d, n, m);
    if (opc == 1) return VWr(T, Q, d, "(" + T + ")__builtin_elementwise_add_sat(" + x + ", " + y + ")");
    if (opc == 2) return same(lop("RHADD", U), sz, ne, d, n, m);
    if (opc == 3) {
        string a64 = VR("vu64", n), b64 = VR("vu64", m), o64 = VR("vu64", d);
        string e;
        if (!U) {
            const string t[] = {a64 + " & " + b64, a64 + " & ~" + b64, a64 + " | " + b64, a64 + " | ~" + b64};
            e = t[sz];
        } else {
            const string t[] = {a64 + " ^ " + b64, "(" + o64 + " & " + a64 + ") | (~" + o64 + " & " + b64 + ")",
                                "(" + o64 + " & ~" + b64 + ") | (" + a64 + " & " + b64 + ")",
                                "(" + o64 + " & " + b64 + ") | (" + a64 + " & ~" + b64 + ")"};
            e = t[sz];
        }
        return VWr("vu64", Q, d, e);
    }
    if (opc == 4) return same(lop("HSUB", U), sz, ne, d, n, m);
    if (opc == 5) return VWr(T, Q, d, "(" + T + ")__builtin_elementwise_sub_sat(" + x + ", " + y + ")");
    if (opc == 6) return VWr(T, Q, d, "(" + T + ")(" + x + " > " + y + ")");
    if (opc == 7) return VWr(T, Q, d, "(" + T + ")(" + x + " >= " + y + ")");
    if (8 <= opc && opc <= 11) {
        static const char *const nm[] = {"SHL", "QSHL", "RSHL", "QRSHL"};
        return same(lop(nm[opc - 8], U), sz, ne, d, n, m);
    }
    if (opc == 12) return VWr(T, Q, d, "(" + T + ")__builtin_elementwise_max(" + x + ", " + y + ")");
    if (opc == 13) return VWr(T, Q, d, "(" + T + ")__builtin_elementwise_min(" + x + ", " + y + ")");
    if (opc == 14 || opc == 15) {
        string e = "(" + T + ")__builtin_elementwise_max(" + x + ", " + y + ") - (" + T + ")__builtin_elementwise_min(" +
                   x + ", " + y + ")";
        return VWr(T, Q, d, opc == 15 ? o + " + (" + e + ")" : e);
    }
    if (opc == 16) return VWr(T, Q, d, a + (U ? " - " : " + ") + b);
    if (opc == 17)
        return VWr(T, Q, d, U ? "(" + T + ")(" + a + " == " + b + ")" : "(" + T + ")((" + a + " & " + b + ") != 0)");
    if (sz == 3 && opc != 23) throw Unsupported("3same size");
    if (opc == 18) return VWr(T, Q, d, o + (U ? " - " : " + ") + a + " * " + b);
    if (opc == 19) {
        if (U) {
            if (sz) throw Unsupported("pmul size");
            return same(lop("PMUL"), 0, ne, d, n, m);
        }
        return VWr(T, Q, d, a + " * " + b);
    }
    if (opc == 20 || opc == 21 || opc == 23) {
        if (opc == 23 && U) throw Unsupported("addp U");
        IntList ev, od;
        pair_idx(Q, sz, ev, od);
        string A = (U || opc == 23) ? a : sa, B = (U || opc == 23) ? b : sb;
        string L = shuf(T, A, B, ev), Rr = shuf(T, A, B, od);
        if (opc == 23) return VWr(T, Q, d, "(" + T + ")(" + L + " + " + Rr + ")");
        return VWr(T, Q, d, "(" + T + ")__builtin_elementwise_" + (opc == 20 ? "max" : "min") + "(" + L + ", " + Rr + ")");
    }
    if (opc == 22) {
        if (sz != 1 && sz != 2) throw Unsupported("sqdmulh size");
        return same(lop(U ? "SQRDMULH" : "SQDMULH"), sz, ne, d, n, m);
    }
    throw Unsupported("3same opcode");
}

std::string SimdC::three_same_scalar(int U, int sz, int opc, int d, int n, int m) {
    if (opc == 1 || opc == 5)
        return same(opc == 1 ? lop(U ? "UQADD" : "SQADD") : lop(U ? "UQSUB" : "SQSUB"), sz, 1, d, n, m);
    if (opc == 9 || opc == 11) return same(lop(opc == 9 ? "QSHL" : "QRSHL", U), sz, 1, d, n, m);
    if (opc == 22) {
        if (sz != 1 && sz != 2) throw Unsupported("sqdmulh size");
        return same(lop(U ? "SQRDMULH" : "SQDMULH"), sz, 1, d, n, m);
    }
    if (sz != 3) throw Unsupported("scalar 3same size");
    string a = cat("VS64(", n, ")"), b = cat("VS64(", m, ")");
    string sa = cat("(int64_t)VS64(", n, ")"), sb = cat("(int64_t)VS64(", m, ")");
    string x = U ? a : sa, y = U ? b : sb;
    if (opc == 6) return cat("c->v[", d, "] = (", x, " > ", y, ") ? ~0ull : 0;");
    if (opc == 7) return cat("c->v[", d, "] = (", x, " >= ", y, ") ? ~0ull : 0;");
    if (opc == 8 || opc == 10) return same(lop(opc == 8 ? "SHL" : "RSHL", U), 3, 1, d, n, m);
    if (opc == 16) return cat("c->v[", d, "] = (uint64_t)(", a, U ? " - " : " + ", b, ");");
    if (opc == 17)
        return U ? cat("c->v[", d, "] = (", a, " == ", b, ") ? ~0ull : 0;")
                 : cat("c->v[", d, "] = (", a, " & ", b, ") ? ~0ull : 0;");
    throw Unsupported("scalar 3same opcode");
}

std::string SimdC::three_same_fp(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), a_ = (int)bits(w, 23, 1), sz = (int)bits(w, 22, 1),
        m = (int)bits(w, 16, 5), opc = (int)bits(w, 11, 5), n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (!scalar && sz && !Q) throw Unsupported("fp 2d Q");
    const FTe &f = FT[sz];
    string F = f.F, VT = f.VT, VU = f.VU, SF = f.SF, SET = f.SET;
    int cop = fcmp3(U, a_, opc);
    if (cop >= 0) {
        if (scalar)
            return cat("c->v[", d, "] = (", ut(2 + sz), ")vcmp_", F, "(c, ", cop, ", (", VT, "){", SF, "(", n, ")}, (", VT,
                       "){", SF, "(", m, ")})[0];");
        return VWr(VU, Q, d, cat("vcmp_", F, "(c, ", cop, ", ", VQ(VT, Q, n), ", ", VQ(VT, Q, m), ")"));
    }
    if (scalar) {
        int k = k3(U, a_, opc);
        if (k != k3(0, 0, 0b11011) && k != k3(0, 0, 0b11111) && k != k3(0, 1, 0b11111) && k != k3(1, 1, 0b11010))
            throw Unsupported("scalar fp 3same");
        string fop = fop3(U, a_, opc);
        string x = (fop == "FOP_RECPS" || fop == "FOP_RSQRTS") ? cat("neg_", F, "(", SF, "(", n, "))") : cat(SF, "(", n, ")");
        return cat(SET, "(", d, ", op_", F, "(c, ", fop, ", ", x, ", ", SF, "(", m, "), 0));");
    }
    string A = VQ(VT, Q, n), B = VQ(VT, Q, m);
    if (const char *pv = fpairv(U, a_, opc)) {
        IntList ev, od;
        pair_idx(Q, 2 + sz, ev, od);
        return cat("SV_FOP_", upper(F), "(", pv, ", ", Q, ", ", d, ", ", shuf(VT, A, B, ev), ", ", shuf(VT, A, B, od), ", (",
                   VT, "){});");
    }
    if (U == 0 && opc == 0b11001)  // FMLA / FMLS: the multiplicand is negated for FMLS
        return cat("SV_FOP_", upper(F), "(FOP_MADD, ", Q, ", ", d, ", ", VQ(VT, Q, d), ", ",
                   a_ ? cat("vneg_", F, "(", A, ")") : A, ", ", B, ");");
    const char *fp = fop3(U, a_, opc);
    if (!fp) throw Unsupported("fp 3same opcode");
    string fop = fp;
    if (fop == "FOP_RECPS" || fop == "FOP_RSQRTS") A = cat("vneg_", F, "(", A, ")");
    return cat("SV_FOP_", upper(F), "(", fop, ", ", Q, ", ", d, ", ", A, ", ", B, ", (", VT, "){});");
}

// ------------------------------------------------------------ three different

std::string SimdC::three_diff(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2), m = (int)bits(w, 16, 5),
        opc = (int)bits(w, 12, 4), n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (scalar) {
        if (U || (opc != 9 && opc != 11 && opc != 13) || (sz != 1 && sz != 2)) throw Unsupported("scalar 3diff");
        string op = opc == 9 ? "SQDMLAL" : opc == 11 ? "SQDMLSL" : "SQDMULL";
        return gen(lop(op), sz, 1, d, sz + 1, 0, 0, n, sz, 0, m, sz, 0);
    }
    if (opc == 14) {  // PMULL
        if (U) throw Unsupported("pmull U");
        if (sz == 3) return cat("sl_pmull64(c, ", Q, ", ", d, ", ", n, ", ", m, ");");
        if (sz) throw Unsupported("pmull size");
    }
    if (sz == 3 || opc == 15) throw Unsupported("3diff size");
    int ne = 8 >> sz;
    int off = Q * ne;
    if (opc == 4 || opc == 6) {  // ADDHN/RADDHN/SUBHN/RSUBHN: narrow into the low or high half
        static const char *const nm[] = {"ADDHN", "RADDHN", "SUBHN", "RSUBHN"};
        string op = nm[(opc == 6) * 2 + U];
        return gen(lop(op), sz, ne, d, sz, off, Q, n, sz + 1, 0, m, sz + 1, 0);
    }
    if (opc == 1 || opc == 3) {  // wide
        string op = opc == 1 ? "ADDW" : "SUBW";
        return gen(lop(op, U), sz, ne, d, sz + 1, 0, 0, n, sz + 1, 0, m, sz, off);
    }
    string op;
    switch (opc) {
        case 0: op = "ADDL"; break;
        case 2: op = "SUBL"; break;
        case 5: op = "ABAL"; break;
        case 7: op = "ABDL"; break;
        case 8: op = "MLAL"; break;
        case 10: op = "MLSL"; break;
        case 12: op = "MULL"; break;
        case 9: op = "SQDMLAL"; break;
        case 11: op = "SQDMLSL"; break;
        case 13: op = "SQDMULL"; break;
        case 14: op = "PMULL"; break;
        default: key_error();
    }
    bool sqd = op.rfind("SQD", 0) == 0, pm = op.rfind("PM", 0) == 0;
    if (sqd) {
        if (U || (sz != 1 && sz != 2)) throw Unsupported("sqdmull");
    }
    return gen(lop(op, !(sqd || pm) ? U : 0), sz, ne, d, sz + 1, 0, 0, n, sz, off, m, sz, off);
}

// ------------------------------------------------------------ two-register misc

std::string SimdC::misc2(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2), opc = (int)bits(w, 12, 5),
        n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (opc >= 0b11000 || opc == 0b10110 || opc == 0b10111 || (0b01100 <= opc && opc <= 0b01111)) return misc2_fp(w, scalar);
    if (scalar) return misc2_scalar(U, sz, opc, d, n);
    int ne = lanes(Q, sz);
    string T = vu(sz), S = vs(sz);
    if (opc == 0 || (opc == 1 && !U)) {  // REV64 / REV32 / REV16
        int gsz = (U == 0 && opc == 0) ? 3 : (U == 1 && opc == 0) ? 2 : (U == 0 && opc == 1) ? 1 : -1;
        if (gsz < 0 || sz >= gsz) throw Unsupported("rev");
        int es = 1 << sz;
        int gb = 1 << gsz;
        IntList idx;
        for (int i = 0; i < 16; i++) {
            int g = i / gb * gb, o = i % gb;
            int e = o / es, b = o % es;
            idx.push_back(g + (gb / es - 1 - e) * es + b);
        }
        return VWr("vu8", Q, d, shuf("vu8", VR("vu8", n), VR("vu8", n), idx));
    }
    if (sz == 3 && !Q && opc != 0b10010 && opc != 0b10100 && opc != 0b10011) throw Unsupported("misc 1d");
    if (opc == 2 || opc == 6) {
        if (sz == 3) throw Unsupported("addlp size");
        return cat("sl_addlp(c, ", U, ", ", (int)(opc == 6), ", ", Q, ", ", sz, ", ", d, ", ", n, ");");
    }
    if (opc == 3) return same(lop(U ? "USQADD" : "SUQADD"), sz, ne, d, n);
    if (opc == 4) {
        if (sz == 3) throw Unsupported("clz size");
        return same(lop(U ? "CLZ" : "CLS"), sz, ne, d, n);
    }
    if (opc == 5) {
        if (!U) {
            if (sz) throw Unsupported("cnt size");
            return same(lop("CNT"), 0, ne, d, n);
        }
        if (sz == 0) return VWr("vu64", Q, d, "~" + VR("vu64", n));
        if (sz == 1) return same(lop("RBIT"), 0, lanes(Q, 0), d, n);
        throw Unsupported("not/rbit size");
    }
    if (opc == 7) return same(lop(U ? "SQNEG" : "SQABS"), sz, ne, d, n);
    string a = VR(T, n), sa = VR(S, n);
    if (opc == 8) return VWr(T, Q, d, "(" + T + ")(" + sa + (U ? " >= " : " > ") + "0)");
    if (opc == 9) return VWr(T, Q, d, "(" + T + ")(" + sa + (U ? " <= " : " == ") + "0)");
    if (opc == 10 && !U) return VWr(T, Q, d, "(" + T + ")(" + sa + " < 0)");
    if (opc == 11) {
        if (U) return VWr(T, Q, d, "-" + a);
        int sh = (8 << sz) - 1;
        return VWr(T, Q, d, cat("((", T, ")", sa, " ^ (", T, ")(", sa, " >> ", sh, ")) - (", T, ")(", sa, " >> ", sh, ")"));
    }
    if (opc == 0b10010 || opc == 0b10100) {  // XTN / SQXTUN / SQXTN / UQXTN
        if (sz == 3) throw Unsupported("xtn size");
        string op = opc == 0b10010 ? (U ? "SQXTUN" : "XTN") : (U ? "UQXTN" : "SQXTN");
        int ne2 = 8 >> sz;
        return gen(lop(op), sz, ne2, d, sz, Q * ne2, Q, n, sz + 1, 0);
    }
    if (opc == 0b10011 && U) {  // SHLL
        if (sz == 3) throw Unsupported("shll size");
        int ne2 = 8 >> sz;
        return gen(lop("SHLL"), sz, ne2, d, sz + 1, 0, 0, n, sz, Q * ne2);
    }
    throw Unsupported("misc2 opcode");
}

std::string SimdC::misc2_scalar(int U, int sz, int opc, int d, int n) {
    if (opc == 3) return same(lop(U ? "USQADD" : "SUQADD"), sz, 1, d, n);
    if (opc == 7) return same(lop(U ? "SQNEG" : "SQABS"), sz, 1, d, n);
    if (opc == 0b10010 || opc == 0b10100) {
        if (sz == 3 || (opc == 0b10010 && !U)) throw Unsupported("scalar xtn");
        string op = opc == 0b10010 ? "SQXTUN" : (U ? "UQXTN" : "SQXTN");
        return gen(lop(op), sz, 1, d, sz, 0, 0, n, sz + 1, 0);
    }
    if (sz != 3) throw Unsupported("scalar misc size");
    string a = cat("VS64(", n, ")"), sa = cat("(int64_t)VS64(", n, ")");
    if (opc == 8) return cat("c->v[", d, "] = (", sa, U ? " >= " : " > ", "0) ? ~0ull : 0;");
    if (opc == 9) return cat("c->v[", d, "] = (", sa, U ? " <= " : " == ", "0) ? ~0ull : 0;");
    if (opc == 10 && !U) return cat("c->v[", d, "] = (", sa, " < 0) ? ~0ull : 0;");
    if (opc == 11)
        return U ? cat("c->v[", d, "] = (uint64_t)-", a, ";")
                 : cat("c->v[", d, "] = (", sa, " < 0) ? (uint64_t)-", a, " : ", a, ";");
    throw Unsupported("scalar misc opcode");
}

// FP two-register misc, vector and scalar.
std::string SimdC::misc2_fp(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), a_ = (int)bits(w, 23, 1), sz = (int)bits(w, 22, 1),
        opc = (int)bits(w, 12, 5), n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    const FTe &f = FT[sz];
    string F = f.F, VT = f.VT, VU = f.VU, SF = f.SF, SU = f.SU, SET = f.SET;
    if (opc == 0b10110 || opc == 0b10111) {  // FCVTN / FCVTL / FCVTXN
        if (a_) throw Unsupported("fcvtn a");
        if (opc == 0b10110 && U) {
            if (!sz) throw Unsupported("fcvtxn size");
            if (scalar) return cat("FP_SETS(", d, ", simd_d2s_odd(c, VSF64(", n, ")));");
            return cat("sv_fcvtn(c, 1, ", Q, ", 1, ", d, ", ", n, ");");
        }
        if (scalar || U) throw Unsupported("fcvtn/l");
        if (opc == 0b10110) return cat("sv_fcvtn(c, ", sz, ", ", Q, ", 0, ", d, ", ", n, ");");
        return cat("sv_fcvtl(c, ", sz, ", ", Q, ", ", d, ", ", n, ");");
    }
    if (!scalar && sz && !Q) throw Unsupported("fp misc 2d Q");
    string x = VQ(VT, Q, n);
    if (0b01100 <= opc && opc <= 0b01111) {
        if (!a_) throw Unsupported("fp16 misc");
        if (opc == 0b01111) {  // FABS / FNEG
            if (scalar) throw Unsupported("scalar fabs");
            string mask = !sz ? "0x80000000u" : "0x8000000000000000ull";
            return VWr(VU, Q, d, U ? VR(VU, n) + " ^ " + mask : VR(VU, n) + " & ~" + mask);
        }
        int op, swap;
        if (opc == 0b01100 && U == 0) op = 2, swap = 0;
        else if (opc == 0b01100 && U == 1) op = 1, swap = 0;
        else if (opc == 0b01101 && U == 0) op = 0, swap = 0;
        else if (opc == 0b01101 && U == 1) op = 1, swap = 1;
        else if (opc == 0b01110 && U == 0) op = 2, swap = 1;
        else throw Unsupported("fcm zero");
        if (scalar) {
            string a = cat("(", VT, "){", SF, "(", n, ")}"), b = "(" + VT + "){}";
            if (swap) std::swap(a, b);
            return cat("c->v[", d, "] = (", ut(2 + sz), ")vcmp_", F, "(c, ", op, ", ", a, ", ", b, ")[0];");
        }
        string a = x, b = "(" + VT + "){}";
        if (swap) std::swap(a, b);
        return VWr(VU, Q, d, cat("vcmp_", F, "(c, ", op, ", ", a, ", ", b, ")"));
    }
    if (opc == 0b11111) {
        if (!a_) throw Unsupported("fp misc 11111");
        if (U) {
            if (scalar) throw Unsupported("scalar fsqrt");
            return cat("SV_FOP_", upper(F), "(FOP_SQRT, ", Q, ", ", d, ", ", x, ", ", x, ", (", VT, "){});");
        }
        if (!scalar) throw Unsupported("vector frecpx");
        return cat(SET, "(", d, ", recpx_", F, "(c, ", SF, "(", n, ")));");
    }
    if (opc == 0b11000 || opc == 0b11001) {  // FRINT*
        if (scalar) throw Unsupported("scalar frint");
        int k = U << 2 | a_ << 1 | (opc & 1), rm;
        switch (k) {
            case 0b000: rm = 0; break;
            case 0b001: rm = 2; break;
            case 0b010: rm = 1; break;
            case 0b011: rm = 3; break;
            case 0b100: rm = 4; break;
            case 0b101: rm = 5; break;
            case 0b111: rm = 6; break;
            default: throw Unsupported("frint");
        }
        return VWr(VT, Q, d, cat("vfrint_", F, "(c, ", x, ", ", rm, ")"));
    }
    if (opc == 0b11010 || opc == 0b11011 || (opc == 0b11100 && !a_)) {  // FCVT[NPMZA][SU]
        int rm = opc == 0b11010 ? a_ : opc == 0b11011 ? 2 + a_ : 4;
        if (scalar)
            return cat("c->v[", d, "] = (", ut(2 + sz), ")toint_", F, "(c, ", SF, "(", n, "), ", rm, ", 0, ", 32 << sz, ", ",
                       U, ");");
        return VWr(VU, Q, d, cat("vtoint_", F, "(c, ", x, ", ", rm, ", 0, ", U, ")"));
    }
    if (opc == 0b11100) {  // URECPE / URSQRTE
        if (scalar || sz) throw Unsupported("urecpe");
        return same(lop(U ? "URSQRTE" : "URECPE"), 2, lanes(Q, 2), d, n);
    }
    if (opc == 0b11101) {
        if (!a_) {  // SCVTF / UCVTF
            if (scalar) return cat(SET, "(", d, ", fromint_", F, "(c, ", SU, "(", n, "), ", 32 << sz, ", ", U, ", 0));");
            return VWr(VT, Q, d, cat("vfromint_", F, "(c, ", VR(VU, n), ", 0, ", U, ")"));
        }
        if (scalar)  // FRECPE / FRSQRTE
            return cat(SET, "(", d, ", ", U ? "rsqrte" : "recpe", "_", F, "(c, ", SF, "(", n, ")));");
        return VWr(VT, Q, d, cat("v", U ? "rsqrte" : "recpe", "_", F, "(c, ", x, ")"));
    }
    throw Unsupported("fp misc opcode");
}

// ------------------------------------------------------------ across lanes / pairwise

std::string SimdC::across(uint32_t w) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2), opc = (int)bits(w, 12, 5),
        n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (opc == 0b01100 || opc == 0b01111) {
        if (!U) throw Unsupported("fp16 across");
        if (!Q || (sz & 1)) throw Unsupported("fp across size");
        int hi = sz >> 1;
        const char *fop = opc == 0b01100 ? (hi ? "FOP_MINNM" : "FOP_MAXNM") : (hi ? "FOP_MIN" : "FOP_MAX");
        return cat("sv_fred_s(c, ", fop, ", ", d, ", ", n, ");");
    }
    int op = opc == 0b00011 ? 1 : opc == 0b01010 ? 2 : opc == 0b11010 ? 3 : opc == 0b11011 ? 0 : -1;
    if (op < 0 || (op == 0 && U) || sz == 3 || (sz == 2 && !Q)) throw Unsupported("across");
    return cat("sl_across(c, ", op, ", ", U, ", ", Q, ", ", sz, ", ", d, ", ", n, ");");
}

std::string SimdC::scalar_pairwise(uint32_t w) {
    int U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2), opc = (int)bits(w, 12, 5), n = (int)bits(w, 5, 5),
        d = (int)bits(w, 0, 5);
    if (!U) {
        if (opc == 0b11011 && sz == 3) return cat("c->v[", d, "] = (uint64_t)(VS64(", n, ") + VHI64(", n, "));");
        throw Unsupported("scalar pairwise");
    }
    int hi = sz >> 1;
    const char *fop = nullptr;
    if (opc == 0b01100) fop = hi ? "FOP_MINNM" : "FOP_MAXNM";
    else if (opc == 0b01101 && !hi) fop = "FOP_ADD";
    else if (opc == 0b01111) fop = hi ? "FOP_MIN" : "FOP_MAX";
    if (!fop) throw Unsupported("scalar fp pairwise");
    const FTe &f = FT[sz & 1];
    return cat(f.SET, "(", d, ", pair_", f.F, "(c, ", fop, ", ", VR(f.VT, n), "[0], ", VR(f.VT, n), "[1]));");
}

// ------------------------------------------------------------ modified immediate

std::string SimdC::modimm(uint32_t w) {
    int Q = (int)bits(w, 30, 1), op = (int)bits(w, 29, 1), cmode = (int)bits(w, 12, 4), o2 = (int)bits(w, 11, 1),
        d = (int)bits(w, 0, 5);
    uint64_t imm8 = (bits(w, 16, 3) << 5) | bits(w, 5, 5);
    if (o2) throw Unsupported("fmov half imm");
    if (cmode == 15 && op && !Q) throw Unsupported("fmov 1d");
    uint64_t imm = simd_expand_imm(op, cmode, imm8);
    int c4 = cmode;
    if ((c4 < 8 && (c4 & 1)) || (8 <= c4 && c4 < 12 && (c4 & 1))) {  // ORR / BIC
        if (op) {
            imm = ~imm;
            return VWr("vu64", Q, d, VR("vu64", d) + " & " + hx(imm));
        }
        return VWr("vu64", Q, d, VR("vu64", d) + " | " + hx(imm));
    }
    if (op && !(c4 == 14 || c4 == 15)) imm = ~imm;  // MVNI
    u128_t v = Q ? ((u128_t)imm | ((u128_t)imm << 64)) : (u128_t)imm;
    return cat("c->v[", d, "] = ", u128(v), ";");
}

// ------------------------------------------------------------ shift by immediate

std::string SimdC::shift_imm(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), immh = (int)bits(w, 19, 4), immb = (int)bits(w, 16, 3),
        opc = (int)bits(w, 11, 5), n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (!immh) throw Unsupported("shift immh");
    int sz = bit_length((uint64_t)immh) - 1;
    int es = 8 << sz;
    int ih = (immh << 3) | immb;
    int rsh = 2 * es - ih, lsh = ih - es;
    if (scalar) Q = 1;
    int ne = scalar ? 1 : lanes(Q, sz);
    bool narrow = 0b10000 <= opc && opc <= 0b10011;
    if (opc == 0b11100 || opc == 0b11111) {  // fixed-point conversions
        if (sz < 2) throw Unsupported("fp16 fixed");
        if (sz == 3 && !Q) throw Unsupported("fixed 1d");
        const FTe &f = FT[sz - 2];
        int fb = rsh;
        if (opc == 0b11100) {
            if (scalar) return cat(f.SET, "(", d, ", fromint_", f.F, "(c, ", f.SU, "(", n, "), ", es, ", ", U, ", ", fb, "));");
            return VWr(f.VT, Q, d, cat("vfromint_", f.F, "(c, ", VR(f.VU, n), ", ", fb, ", ", U, ")"));
        }
        if (scalar)
            return cat("c->v[", d, "] = (", ut(sz), ")toint_", f.F, "(c, ", f.SF, "(", n, "), 3, ", fb, ", ", es, ", ", U, ");");
        return VWr(f.VU, Q, d, cat("vtoint_", f.F, "(c, ", VQ(f.VT, Q, n), ", 3, ", fb, ", ", U, ")"));
    }
    if (narrow) {
        if (sz == 3) throw Unsupported("narrow shift size");
        ne = scalar ? 1 : 8 >> sz;
        string name;
        int u = 0;
        switch (opc << 1 | U) {
            case 0b10000 << 1 | 0: name = "SHRN"; break;
            case 0b10001 << 1 | 0: name = "RSHRN"; break;
            case 0b10000 << 1 | 1: name = "QSHRUN"; break;
            case 0b10001 << 1 | 1: name = "QRSHRUN"; break;
            case 0b10010 << 1 | 0: name = "QSHRN"; break;
            case 0b10010 << 1 | 1: name = "QSHRN", u = 1; break;
            case 0b10011 << 1 | 0: name = "QRSHRN"; break;
            case 0b10011 << 1 | 1: name = "QRSHRN", u = 1; break;
        }
        if (scalar && (name == "SHRN" || name == "RSHRN")) throw Unsupported("scalar shrn");
        int part = scalar ? 0 : Q;
        return gen(lop(name, u), sz, ne, d, sz, part * ne, part, n, sz + 1, 0, -1, 0, 0, -1, rsh);
    }
    if (opc == 0b10100) {  // SSHLL / USHLL
        if (scalar || sz == 3) throw Unsupported("sshll");
        ne = 8 >> sz;
        return gen(lop("SHLLI", U), sz, ne, d, sz + 1, 0, 0, n, sz, Q * ne, -1, 0, 0, -1, lsh);
    }
    if (sz == 3 && !Q) throw Unsupported("shift 1d");
    if (scalar && sz != 3 && opc != 0b01100 && opc != 0b01110) throw Unsupported("scalar shift size");
    string T = vu(sz), S = vs(sz);
    if (opc == 0 || opc == 2) {  // SSHR/USHR, SSRA/USRA
        string e;
        if (U)
            e = rsh >= es ? "(" + T + "){}" : cat("(", VR(T, n), " >> ", rsh, ")");
        else
            e = cat("(", T, ")(", VR(S, n), " >> ", std::min(rsh, es - 1), ")");
        if (opc == 2) e = VR(T, d) + " + " + e;
        return !scalar ? VWr(T, Q, d, e) : cat("c->v[", d, "] = (uint64_t)((", e, ")[0]);");
    }
    if (opc == 4 || opc == 6) return same(lop(opc == 4 ? "RSHRI" : "RSRAI", U), sz, ne, d, n, -1, rsh);
    if (opc == 8) {
        if (!U) throw Unsupported("sri U");
        return same(lop("SRI"), sz, ne, d, n, -1, rsh);
    }
    if (opc == 10) {
        if (U) return same(lop("SLI"), sz, ne, d, n, -1, lsh);
        string e = cat(VR(T, n), " << ", lsh);
        return !scalar ? VWr(T, Q, d, e) : cat("c->v[", d, "] = (uint64_t)((", e, ")[0]);");
    }
    if (opc == 12) {
        if (!U) throw Unsupported("sqshlu U");
        return same(lop("QSHLUI"), sz, ne, d, n, -1, lsh);
    }
    if (opc == 14) return same(lop("QSHLI", U), sz, ne, d, n, -1, lsh);
    throw Unsupported("shift imm opcode");
}

// ------------------------------------------------------------ indexed element

std::string SimdC::indexed(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2), L = (int)bits(w, 21, 1),
        M = (int)bits(w, 20, 1), rm = (int)bits(w, 16, 4), opc = (int)bits(w, 12, 4), H = (int)bits(w, 11, 1),
        n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    bool fp = (opc == 1 || opc == 5 || opc == 9) && !(U && opc != 9);
    if (fp) {
        if (sz < 2) throw Unsupported("fp16 indexed");
        int fsz = sz & 1;
        int idx;
        if (fsz) {
            if (L) throw Unsupported("indexed L");
            idx = H;
        } else {
            idx = (H << 1) | L;
        }
        int m = (M << 4) | rm;
        if (!scalar && fsz && !Q) throw Unsupported("indexed 2d Q");
        const FTe &f = FT[fsz];
        string F = f.F, VT = f.VT, SF = f.SF, SET = f.SET;
        if (scalar) {
            string el = cat(VR(VT, m), "[", idx, "]");
            if (opc == 9)
                return cat(SET, "(", d, ", op_", F, "(c, ", U ? "FOP_MULX" : "FOP_MUL", ", ", SF, "(", n, "), ", el, ", 0));");
            string x = opc == 1 ? cat(SF, "(", n, ")") : cat("neg_", F, "(", SF, "(", n, "))");
            return cat(SET, "(", d, ", op_", F, "(c, FOP_MADD, ", SF, "(", d, "), ", x, ", ", el, "));");
        }
        string el = cat("vsplat_", F, "(", VR(VT, m), ", ", idx, ", ", Q, ")");
        if (opc == 9)
            return cat("SV_FOP_", upper(F), "(", U ? "FOP_MULX" : "FOP_MUL", ", ", Q, ", ", d, ", ", VQ(VT, Q, n), ", ", el,
                       ", (", VT, "){});");
        string x = opc == 1 ? VQ(VT, Q, n) : cat("vneg_", F, "(", VQ(VT, Q, n), ")");
        return cat("SV_FOP_", upper(F), "(FOP_MADD, ", Q, ", ", d, ", ", VQ(VT, Q, d), ", ", x, ", ", el, ");");
    }
    int idx, m;
    if (sz == 1) {
        idx = (H << 2) | (L << 1) | M, m = rm;
    } else if (sz == 2) {
        idx = (H << 1) | L, m = (M << 4) | rm;
    } else {
        throw Unsupported("indexed size");
    }
    string T = vu(sz);
    if (scalar) {
        string op;
        if (U == 0) {
            if (opc == 3) op = "SQDMLAL";
            else if (opc == 7) op = "SQDMLSL";
            else if (opc == 11) op = "SQDMULL";
            else if (opc == 12) op = "SQDMULH";
            else if (opc == 13) op = "SQRDMULH";
        }
        if (op.empty()) throw Unsupported("scalar indexed");
        if (op == "SQDMULH" || op == "SQRDMULH") return gen(lop(op), sz, 1, d, sz, 0, 0, n, sz, 0, m, sz, 0, idx);
        return gen(lop(op), sz, 1, d, sz + 1, 0, 0, n, sz, 0, m, sz, 0, idx);
    }
    int ne = lanes(Q, sz);
    string el = shuf(T, VR(T, m), VR(T, m), IntList(16 >> sz, idx));
    if (U == 0 && opc == 8) return VWr(T, Q, d, VR(T, n) + " * " + el);
    if (U == 1 && (opc == 0 || opc == 4)) return VWr(T, Q, d, VR(T, d) + (opc == 0 ? " + " : " - ") + VR(T, n) + " * " + el);
    if (U == 0 && (opc == 12 || opc == 13)) return same(lop(opc == 12 ? "SQDMULH" : "SQRDMULH"), sz, ne, d, n, m, 0, idx);
    string lng;
    switch (opc) {
        case 2: lng = "MLAL"; break;
        case 6: lng = "MLSL"; break;
        case 10: lng = "MULL"; break;
        case 3: lng = "SQDMLAL"; break;
        case 7: lng = "SQDMLSL"; break;
        case 11: lng = "SQDMULL"; break;
    }
    bool sq = lng.rfind("SQ", 0) == 0;
    if (lng.empty() || (U && sq)) throw Unsupported("indexed opcode");
    ne = 8 >> sz;
    return gen(lop(lng, !sq ? U : 0), sz, ne, d, sz + 1, 0, 0, n, sz, Q * ne, m, sz, 0, idx);
}

// ------------------------------------------------------------ scalar FP

std::string SimdC::fp_dp3(uint32_t w) {
    int t = ftype(w);
    int o1 = (int)bits(w, 21, 1), o0 = (int)bits(w, 15, 1);
    int d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5), m = (int)bits(w, 16, 5), a = (int)bits(w, 10, 5);
    int na = o1, nn = o1 ^ o0;  // FMADD 00, FMSUB 01 (-n), FNMADD 10 (-a, -n), FNMSUB 11 (-a)
    return cat("fp_fma_", FT[t].F, "(c, ", na, ", ", nn, ", ", d, ", ", n, ", ", m, ", ", a, ");");
}

std::string SimdC::fp_dp2(uint32_t w) {
    int t = ftype(w);
    int opc = (int)bits(w, 12, 4), d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5), m = (int)bits(w, 16, 5);
    static const char *const fops[] = {"FOP_MUL", "FOP_DIV", "FOP_ADD", "FOP_SUB", "FOP_MAX",
                                       "FOP_MIN", "FOP_MAXNM", "FOP_MINNM", "FOP_NMUL"};
    if (opc > 8) throw Unsupported("fp dp2");
    return cat("fp_op_", FT[t].F, "(c, ", fops[opc], ", ", d, ", ", n, ", ", m, ");");
}

std::string SimdC::fp_dp1(uint32_t w) {
    int t = (int)bits(w, 22, 2);
    int opc = (int)bits(w, 15, 6), d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5);
    if (t == 3) {  // half source: only FCVT
        if (opc == 4) return cat("FP_SETS(", d, ", simd_h2s(c, VS16(", n, ")));");
        if (opc == 5) return cat("FP_SETD(", d, ", simd_h2d(c, VS16(", n, ")));");
        throw Unsupported("fp16 dp1");
    }
    if (t == 2) throw Unsupported("fp type");
    const FTe &f = FT[t];
    string sign = !t ? "0x80000000u" : "0x8000000000000000ull";
    if (opc == 0) return cat("c->v[", d, "] = ", f.SU, "(", n, ");");
    if (opc == 1) return cat("c->v[", d, "] = ", f.SU, "(", n, ") & ~", sign, ";");
    if (opc == 2) return cat("c->v[", d, "] = ", f.SU, "(", n, ") ^ ", sign, ";");
    if (opc == 3) return cat("fp_op_", f.F, "(c, FOP_SQRT, ", d, ", ", n, ", ", n, ");");
    if (opc == 4 || opc == 5 || opc == 7) {
        int to = opc == 4 ? 0 : opc == 5 ? 1 : 3;
        if (to == t) throw Unsupported("fcvt same");
        if (to == 3) return cat("c->v[", d, "] = ", !t ? "simd_s2h" : "simd_dd2h", "(c, ", f.SF, "(", n, "));");
        return to ? cat("FP_SETD(", d, ", simd_s2d(c, VSF32(", n, ")));") : cat("FP_SETS(", d, ", simd_d2s(c, VSF64(", n, ")));");
    }
    int rm;
    switch (opc) {
        case 8: rm = 0; break;
        case 9: rm = 1; break;
        case 10: rm = 2; break;
        case 11: rm = 3; break;
        case 12: rm = 4; break;
        case 14: rm = 5; break;
        case 15: rm = 6; break;
        default: throw Unsupported("fp dp1 opcode");
    }
    return cat(f.SET, "(", d, ", frint_", f.F, "(c, ", f.SF, "(", n, "), ", rm, "));");
}

std::string SimdC::fp_cmp(uint32_t w) {
    int t = ftype(w);
    if (bits(w, 14, 2) || bits(w, 0, 3)) throw Unsupported("fcmp op");
    const FTe &f = FT[t];
    int n = (int)bits(w, 5, 5), m = (int)bits(w, 16, 5), zero = (int)bits(w, 3, 1);
    string b = zero ? "0" : cat(f.SF, "(", m, ")");
    return cat("c->nzcv = cmp_", f.F, "(c, ", f.SF, "(", n, "), ", b, ");");
}

std::string SimdC::fp_ccmp(uint32_t w) {
    int t = ftype(w);
    const FTe &f = FT[t];
    int n = (int)bits(w, 5, 5), m = (int)bits(w, 16, 5), cnd = (int)bits(w, 12, 4), nzcv = (int)bits(w, 0, 4);
    return cat("c->nzcv = ", cond(cnd), " ? cmp_", f.F, "(c, ", f.SF, "(", n, "), ", f.SF, "(", m, ")) : ",
               hx((uint64_t)nzcv << 28), ";");
}

std::string SimdC::fp_csel(uint32_t w) {
    int t = ftype(w);
    string SU = FT[t].SU;
    int d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5), m = (int)bits(w, 16, 5), cnd = (int)bits(w, 12, 4);
    return cat("c->v[", d, "] = ", cond(cnd), " ? ", SU, "(", n, ") : ", SU, "(", m, ");");
}

std::string SimdC::fp_imm(uint32_t w) {
    int t = ftype(w);
    if (bits(w, 5, 5)) throw Unsupported("fmov imm5");
    uint64_t imm8 = bits(w, 13, 8);
    int d = (int)bits(w, 0, 5);
    return cat("c->v[", d, "] = ", hx(vfp_expand_imm(imm8, 32 << t)), ";");
}

std::string SimdC::fp_fixed(uint32_t w) {
    int sf = (int)bits(w, 31, 1), t = (int)bits(w, 22, 2), rmode = (int)bits(w, 19, 2), opc = (int)bits(w, 16, 3),
        scale = (int)bits(w, 10, 6), n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (t > 1) throw Unsupported("fixed type");
    if (!sf && scale < 32) throw Unsupported("fixed scale");
    const FTe &f = FT[t];
    int fb = 64 - scale, ib = sf ? 64 : 32;
    if (rmode == 0 && (opc == 2 || opc == 3)) {
        string src = sf ? X(n) : W(n);
        return cat(f.SET, "(", d, ", fromint_", f.F, "(c, ", src, ", ", ib, ", ", opc & 1, ", ", fb, "));");
    }
    if (rmode == 3 && (opc == 0 || opc == 1))
        return set_reg(sf, d, cat("toint_", f.F, "(c, ", f.SF, "(", n, "), 3, ", fb, ", ", ib, ", ", opc, ")"));
    throw Unsupported("fixed opcode");
}

std::string SimdC::fp_int(uint32_t w) {
    int sf = (int)bits(w, 31, 1), t = (int)bits(w, 22, 2), rmode = (int)bits(w, 19, 2), opc = (int)bits(w, 16, 3),
        n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (opc == 6 || opc == 7) {  // FMOV
        if (t == 2 && sf && rmode == 1)
            return opc == 6 ? set_reg(1, d, cat("VHI64(", n, ")")) : cat("VHI64(", d, ") = ", X(n), ";");
        if (rmode || t != sf || t > 1) throw Unsupported("fmov general");
        if (opc == 6) return set_reg(sf, d, sf ? cat("VS64(", n, ")") : cat("VS32(", n, ")"));
        return cat("c->v[", d, "] = ", sf ? X(n) : W(n), ";");
    }
    if (t > 1) throw Unsupported("fp int type");
    const FTe &f = FT[t];
    int ib = sf ? 64 : 32;
    if (opc == 2 || opc == 3) {
        if (rmode) throw Unsupported("cvtf rmode");
        return cat(f.SET, "(", d, ", fromint_", f.F, "(c, ", sf ? X(n) : W(n), ", ", ib, ", ", opc & 1, ", 0));");
    }
    int rm;
    if (opc == 4 || opc == 5) {
        if (rmode) throw Unsupported("fcvta rmode");
        rm = 4;
    } else {
        rm = rmode;
    }
    return set_reg(sf, d, cat("toint_", f.F, "(c, ", f.SF, "(", n, "), ", rm, ", 0, ", ib, ", ", opc & 1, ")"));
}

// ------------------------------------------------------------ loads / stores

std::string SimdC::ldst(uint32_t w, uint64_t pc) {
    int rt = (int)bits(w, 0, 5);
    if (bits(w, 27, 3) == 0b011 && bits(w, 24, 2) == 0) {  // literal
        int opc = (int)bits(w, 30, 2);
        if (opc == 3) throw Unsupported("ldr literal opc");
        uint64_t addr = pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
        static const char *const ld[] = {"LD32", "LD64", "LD128"};
        return cat("c->v[", rt, "] = ", ld[opc], "(", gaddr(addr), ");");
    }
    if (bits(w, 31, 1) == 0 && (bits(w, 24, 6) == 0b001100 || bits(w, 24, 6) == 0b001101)) return ldst_struct(w);
    if (bits(w, 27, 3) == 0b101) return ldst_pair(w);
    if (bits(w, 27, 3) == 0b111) return ldst_reg(w);
    throw Unsupported("ldst simd " + hex08(w));
}

std::string SimdC::ldst_pair(uint32_t w) {
    int opc = (int)bits(w, 30, 2), mode = (int)bits(w, 23, 2), L = (int)bits(w, 22, 1);
    int64_t imm7 = sext(bits(w, 15, 7), 7);
    int rt2 = (int)bits(w, 10, 5), rn = (int)bits(w, 5, 5), rt = (int)bits(w, 0, 5);
    if (opc == 3) throw Unsupported("pair opc 3");
    int scale = 2 + opc;
    int64_t off = imm7 * (1 << scale);
    string base = XS(rn);
    bool post = mode == 1, wb = mode == 1 || mode == 3;
    string ea = post ? base : (off >= 0 ? cat(base, " + ", off) : cat(base, " - ", -off));
    static const char *const lds[] = {"LD32", "LD64", "LD128"}, *const sts[] = {"ST32", "ST64", "ST128"};
    string ld = lds[scale - 2], st = sts[scale - 2];
    int sz = 1 << scale;
    std::vector<string> lines = {"uint64_t ea = " + ea + ";"};
    if (L) {
        lines.push_back(cat("u128 a = ", ld, "(ea), b = ", ld, "(ea + ", sz, ");"));
        lines.push_back(cat("c->v[", rt, "] = a; c->v[", rt2, "] = b;"));
    } else {
        string v1, v2;
        if (scale == 4) {
            v1 = cat("c->v[", rt, "]"), v2 = cat("c->v[", rt2, "]");
        } else {
            const char *c_ = scale == 2 ? "VS32" : "VS64";
            v1 = cat(c_, "(", rt, ")"), v2 = cat(c_, "(", rt2, ")");
        }
        lines.push_back(cat(st, "(ea, ", v1, "); ", st, "(ea + ", sz, ", ", v2, ");"));
    }
    if (wb) {
        string dst = rn == 31 ? "c->sp" : cat("c->x[", rn, "]");
        if (!post)
            lines.push_back(dst + " = ea;");
        else
            lines.push_back(off >= 0 ? cat(dst, " = ea + ", off, ";") : cat(dst, " = ea - ", -off, ";"));
    }
    return "{ " + join(lines, " ") + " }";
}

std::string SimdC::ldst_reg(uint32_t w) {
    int size = (int)bits(w, 30, 2), opc = (int)bits(w, 22, 2), rn = (int)bits(w, 5, 5), rt = (int)bits(w, 0, 5);
    int scale;
    if (opc & 2) {
        if (size) throw Unsupported("ldst simd size");
        scale = 4;
    } else {
        scale = size;
    }
    int L = opc & 1;
    string base = XS(rn);
    string wb, ea;
    if (bits(w, 24, 2) == 1) {
        ea = base + " + " + hx(bits(w, 10, 12) << scale);
    } else if (bits(w, 24, 2) == 0) {
        int kind = (int)bits(w, 10, 2);
        if (bits(w, 21, 1)) {
            if (kind != 2) throw Unsupported("ldst simd reg kind");
            int rm = (int)bits(w, 16, 5), option = (int)bits(w, 13, 3), S = (int)bits(w, 12, 1);
            if (option != 2 && option != 3 && option != 6 && option != 7) throw Unsupported("ldst simd reg option");
            ea = base + " + " + extend_expr(rm, option, S ? scale : 0);
        } else {
            int64_t imm9 = sext(bits(w, 12, 9), 9);
            string si = imm9 >= 0 ? cat("+ ", imm9) : cat("- ", -imm9);
            if (kind == 0) {
                ea = base + " " + si;
            } else if (kind == 1) {
                ea = base, wb = base + " " + si;
            } else if (kind == 3) {
                ea = base + " " + si, wb = "ea";
            } else {
                throw Unsupported("ldst simd unprivileged");
            }
        }
    } else {
        throw Unsupported("ldst simd");
    }
    std::vector<string> parts = {"uint64_t ea = " + ea + ";"};
    if (L) {
        static const char *const ld[] = {"LD8", "LD16", "LD32", "LD64", "LD128"};
        parts.push_back(cat("c->v[", rt, "] = ", ld[scale], "(ea);"));
    } else {
        static const char *const st[] = {"ST8", "ST16", "ST32", "ST64", "ST128"};
        string src = scale == 4 ? cat("c->v[", rt, "]") : cat("VS", 8 << scale, "(", rt, ")");
        parts.push_back(cat(st[scale], "(ea, ", src, ");"));
    }
    if (!wb.empty()) parts.push_back((rn == 31 ? string("c->sp") : cat("c->x[", rn, "]")) + " = " + wb + ";");
    return "{ " + join(parts, " ") + " }";
}

std::string SimdC::ldst_struct(uint32_t w) {
    int Q = (int)bits(w, 30, 1), L = (int)bits(w, 22, 1), post = (int)bits(w, 23, 1), rm = (int)bits(w, 16, 5),
        rn = (int)bits(w, 5, 5), rt = (int)bits(w, 0, 5), sz = (int)bits(w, 10, 2);
    if (!post && rm) throw Unsupported("ldst struct rm");
    string base = XS(rn);
    int total;
    string acc;
    if (bits(w, 24, 1) == 0) {  // multiple structures
        if (bits(w, 21, 1)) throw Unsupported("ldst multiple bit21");
        int opc = (int)bits(w, 12, 4);
        int selem, rpt;
        switch (opc) {
            case 0: selem = 4, rpt = 1; break;
            case 2: selem = 1, rpt = 4; break;
            case 4: selem = 3, rpt = 1; break;
            case 6: selem = 1, rpt = 3; break;
            case 7: selem = 1, rpt = 1; break;
            case 8: selem = 2, rpt = 1; break;
            case 10: selem = 1, rpt = 2; break;
            default: throw Unsupported("ldst multiple opcode");
        }
        if (sz == 3 && !Q && selem > 1) throw Unsupported("ldst multiple 1d");
        total = (8 << Q) * selem * rpt;
        if (selem == 1) {  // LD1/ST1: whole registers
            int step = Q ? 16 : 8;
            std::vector<string> a;
            for (int i = 0; i < rpt; i++) {
                int r = (rt + i) % 32;
                if (L)
                    a.push_back(cat("c->v[", r, "] = ", Q ? "LD128" : "LD64", "(ea + ", i * step, ");"));
                else
                    a.push_back(cat(Q ? "ST128" : "ST64", "(ea + ", i * step, ", ",
                                    Q ? cat("c->v[", r, "]") : cat("VS64(", r, ")"), ");"));
            }
            acc = join(a, " ");
        } else {
            acc = cat("sl_", L ? "ldm" : "stm", "(c, ", Q, ", ", sz, ", ", selem, ", ", rpt, ", ", rt, ", ea);");
        }
    } else {  // single structure
        int opc = (int)bits(w, 13, 3), S = (int)bits(w, 12, 1), R_ = (int)bits(w, 21, 1);
        int scale = opc >> 1;
        int selem = ((opc & 1) << 1 | R_) + 1;
        if (scale == 3) {
            if (!L || S) throw Unsupported("ldnr");
            total = selem << sz;
            acc = cat("sl_ldr(c, ", Q, ", ", sz, ", ", selem, ", ", rt, ", ea);");
        } else {
            int idx;
            if (scale == 0) {
                idx = (Q << 3) | (S << 2) | sz;
            } else if (scale == 1) {
                if (sz & 1) throw Unsupported("ld1 h size");
                idx = (Q << 2) | (S << 1) | (sz >> 1);
            } else {
                if (sz & 2) throw Unsupported("ld1 s size");
                if (sz & 1) {
                    if (S) throw Unsupported("ld1 d S");
                    idx = Q, scale = 3;
                } else {
                    idx = (Q << 1) | S;
                }
            }
            total = selem << scale;
            acc = cat("sl_", L ? "lds" : "sts", "(c, ", scale, ", ", selem, ", ", idx, ", ", rt, ", ea);");
        }
    }
    std::vector<string> parts = {"uint64_t ea = " + base + ";", acc};
    if (post) {
        string dst = rn == 31 ? "c->sp" : cat("c->x[", rn, "]");
        parts.push_back(rm == 31 ? cat(dst, " = ea + ", total, ";") : cat(dst, " = ea + ", X(rm), ";"));
    }
    return "{ " + join(parts, " ") + " }";
}

// ------------------------------------------------------------ CRC32

std::string SimdC::crc(uint32_t w) {
    int sf = (int)bits(w, 31, 1), rm = (int)bits(w, 16, 5), C = (int)bits(w, 12, 1), sz = (int)bits(w, 10, 2),
        rn = (int)bits(w, 5, 5), rd = (int)bits(w, 0, 5);
    if ((sz == 3) != (bool)sf) throw Unsupported("crc size");
    string val = sz == 3 ? X(rm) : W(rm);
    return set_reg(0, rd, cat(C ? "CRC32C" : "CRC32", "(", W(rn), ", ", val, ", ", 1 << sz, ")"));
}

}  // namespace lasgen
