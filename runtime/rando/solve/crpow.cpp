// x ** y, correctly rounded (crpow.h).
//
// Double-double arithmetic (Dekker, Knuth; error-free sums and an fma product) for the fast step; 320-bit fixed point
// for the rare inputs it can't decide. Written for LAS Recomp from the textbook algorithms.
#include "crpow.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace rando {
namespace solve {

namespace {

// ---- double-double: hi + lo, |lo| <= ulp(hi) / 2. The sums are error-free whatever the compiler contracts (no
// products in them); the products use an explicit fma.
struct DD {
    double hi, lo;
};

inline DD two_sum(double a, double b) {
    double s = a + b, bb = s - a;
    return {s, (a - (s - bb)) + (b - bb)};
}
inline DD fast_two_sum(double a, double b) {  // |a| >= |b|
    double s = a + b;
    return {s, b - (s - a)};
}
inline DD two_prod(double a, double b) {
    double p = a * b;
    return {p, fma(a, b, -p)};
}
inline DD dd_add(DD a, DD b) {
    DD s = two_sum(a.hi, b.hi), t = two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = fast_two_sum(s.hi, s.lo);
    s.lo += t.lo;
    return fast_two_sum(s.hi, s.lo);
}
inline DD dd_mul(DD a, DD b) {
    DD p = two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return fast_two_sum(p.hi, p.lo);
}
inline DD dd_mul_d(DD a, double b) {
    DD p = two_prod(a.hi, b);
    p.lo += a.lo * b;
    return fast_two_sum(p.hi, p.lo);
}
inline DD dd_div(DD a, DD b) {
    double q1 = a.hi / b.hi;
    DD r = dd_add(a, dd_mul_d(b, -q1));
    double q2 = r.hi / b.hi;
    r = dd_add(r, dd_mul_d(b, -q2));
    double q3 = r.hi / b.hi;
    return dd_add(fast_two_sum(q1, q2), DD{q3, 0});
}

const double SQRT2 = 1.4142135623730951;
const DD LN2 = {0x1.62e42fefa39efp-1, 0x1.abc9e3b39803fp-56};

struct Tables {
    DD inv_odd[48];   // 1 / (2k + 1)
    DD inv_fact[16];  // 1 / n!
    DD ln_c[92];      // ln(1 + j / 128), j = -38 .. 53
    DD exp_j[47];     // exp(j / 64), j = -23 .. 23
    Tables() {
        for (int k = 0; k < 48; k++) inv_odd[k] = dd_div(DD{1, 0}, DD{(double)(2 * k + 1), 0});
        inv_fact[0] = {1, 0};
        for (int n = 1; n < 16; n++) inv_fact[n] = dd_div(inv_fact[n - 1], DD{(double)n, 0});
        for (int j = -38; j <= 53; j++) {  // 2 atanh((c - 1) / (c + 1)), |s| < 0.21: 47 terms
            double c = 1 + j / 128.0;
            DD s = dd_div(DD{c - 1, 0}, two_sum(c, 1)), s2 = dd_mul(s, s), p = inv_odd[47];
            for (int k = 46; k >= 0; k--) p = dd_add(dd_mul(p, s2), inv_odd[k]);
            ln_c[j + 38] = dd_mul_d(dd_mul(s, p), 2.0);
        }
        for (int j = -23; j <= 23; j++) {  // Taylor, |r| <= 0.36: 34 terms
            DD r = {j / 64.0, 0}, sum = {1, 0}, term = {1, 0};
            for (int n = 1; n <= 34; n++) {
                term = dd_div(dd_mul(term, r), DD{(double)n, 0});
                sum = dd_add(sum, term);
            }
            exp_j[j + 23] = sum;
        }
    }
};
const Tables &tables() {
    static const Tables t;
    return t;
}

// round U = u (a value below 2^53 known within +-err) to the nearest integer, ties to even; false if err leaves it
// open
bool round_checked(double uh, double ul, double err, double *out) {
    double n = nearbyint(uh);  // (|uh| < 2^53: exact, and uh - n is exact)
    double d = (uh - n) + ul;
    err += 0x1p-50;  // (the rounding of that sum)
    if (d + err < 0.5 && d - err > -0.5) return *out = n, true;
    if (d - err > 0.5) return *out = n + 1, true;
    if (d + err < -0.5) return *out = n - 1, true;
    return false;
}

}  // namespace

double cr_pow_fast(double x, double y, bool *ok) {
    const Tables &T = tables();
    *ok = true;
    // ln x = k ln 2 + ln c + 2 atanh((m - c) / (m + c)), m in [sqrt(1/2), sqrt(2)], c = 1 + j/128 nearest m
    int e;
    double f = frexp(x, &e), m = 2 * f;
    int k = e - 1;
    if (m > SQRT2) m = f, k = e;
    int j = (int)nearbyint((m - 1) * 128);
    double c = 1 + j / 128.0;
    DD s = dd_div(DD{m - c, 0}, two_sum(m, c)), s2 = dd_mul(s, s), p = T.inv_odd[7];  // (|s| < 2^-8.5: 8 terms)
    for (int i = 6; i >= 0; i--) p = dd_add(dd_mul(p, s2), T.inv_odd[i]);
    DD lnm = dd_add(T.ln_c[j + 38], dd_mul_d(dd_mul(s, p), 2.0));
    DD kl = two_prod((double)k, LN2.hi);
    kl = dd_add(kl, DD{k * LN2.lo, 0});
    DD z = dd_mul_d(dd_add(kl, lnm), y);  // y ln x
    if (z.hi < -745.14) return 0.0;      // (below 2^-1075.0001: rounds to 0)
    if (z.hi > 709.0) return *ok = false, 0.0;  // (x > 1 with an overflowing result: not handled here)
    // exp z = 2^kk exp(j2/64) exp(r2), |r2| <= 1/128
    double kk = nearbyint(z.hi / LN2.hi);
    DD kh = two_prod(kk, LN2.hi);
    DD r = dd_add(z, DD{-kh.hi, -kh.lo});
    r = dd_add(r, DD{-kk * LN2.lo, 0});
    int j2 = (int)nearbyint(r.hi * 64);
    if (j2 < -23 || j2 > 23) return *ok = false, 0.0;
    DD r2 = dd_add(r, DD{-j2 / 64.0, 0}), q = T.inv_fact[13];
    for (int n = 12; n >= 0; n--) q = dd_add(dd_mul(q, r2), T.inv_fact[n]);
    DD E = dd_mul(T.exp_j[j2 + 23], q);
    // E 2^kk as an integer U at the result's precision: 53 bits, or 2^-1074 steps below 2^-1022
    int ex;
    frexp(E.hi, &ex);
    int b = ex - 1;
    if (E.hi == ldexp(1.0, b) && E.lo < 0) b--;
    int vb = b + (int)kk;
    int sc = vb >= -1022 ? 52 - vb : 1074;
    int sh = (int)kk + sc;
    double uh = ldexp(E.hi, sh), ul = ldexp(E.lo, sh), n;
    // (relative error: ln x within 2^-102, y ln x then within 745 * 2^-102 absolute, the reductions 2^-98: 2^-92 in
    // all; tested with 2^-88)
    if (!round_checked(uh, ul, fabs(uh) * 0x1p-88, &n)) return *ok = false, 0.0;
    return ldexp(n, -sc);
}

// ---- 320-bit fixed point: value = +-(w as an unsigned integer) * 2^-320, w in 32-bit words, least significant first

namespace {

const int W = 12, FRAC = 320;

struct Big {
    bool neg = false;
    uint32_t w[W] = {};
};

int mag_cmp(const Big &a, const Big &b) {
    for (int i = W - 1; i >= 0; i--)
        if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
    return 0;
}
bool is_zero(const Big &a) {
    for (int i = 0; i < W; i++)
        if (a.w[i]) return false;
    return true;
}
Big mag_add(const Big &a, const Big &b) {
    Big r;
    uint64_t c = 0;
    for (int i = 0; i < W; i++) c += (uint64_t)a.w[i] + b.w[i], r.w[i] = (uint32_t)c, c >>= 32;
    return r;
}
Big mag_sub(const Big &a, const Big &b) {  // |a| >= |b|
    Big r;
    int64_t c = 0;
    for (int i = 0; i < W; i++) {
        c += (int64_t)a.w[i] - b.w[i];
        r.w[i] = (uint32_t)c;
        c = c < 0 ? -1 : 0;
    }
    return r;
}
Big add(const Big &a, const Big &b) {
    Big r;
    if (a.neg == b.neg) r = mag_add(a, b), r.neg = a.neg;
    else if (mag_cmp(a, b) >= 0) r = mag_sub(a, b), r.neg = a.neg;
    else r = mag_sub(b, a), r.neg = b.neg;
    if (is_zero(r)) r.neg = false;
    return r;
}
Big negate(Big a) {
    if (!is_zero(a)) a.neg = !a.neg;
    return a;
}
Big mul(const Big &a, const Big &b) {  // (truncated to 2^-320)
    uint32_t p[2 * W] = {};
    for (int i = 0; i < W; i++) {
        uint64_t c = 0;
        for (int j = 0; j < W; j++) {
            c += (uint64_t)a.w[i] * b.w[j] + p[i + j];
            p[i + j] = (uint32_t)c;
            c >>= 32;
        }
        p[i + W] = (uint32_t)c;
    }
    Big r;
    memcpy(r.w, p + FRAC / 32, sizeof r.w);
    r.neg = a.neg != b.neg && !is_zero(r);
    return r;
}
Big mul_u(const Big &a, uint32_t m) {
    Big r = a;
    uint64_t c = 0;
    for (int i = 0; i < W; i++) c += (uint64_t)a.w[i] * m, r.w[i] = (uint32_t)c, c >>= 32;
    return r;
}
Big div_u(const Big &a, uint32_t d) {
    Big r = a;
    uint64_t rem = 0;
    for (int i = W - 1; i >= 0; i--) {
        uint64_t cur = rem << 32 | a.w[i];
        r.w[i] = (uint32_t)(cur / d);
        rem = cur % d;
    }
    if (is_zero(r)) r.neg = false;
    return r;
}
Big div(const Big &a, const Big &b) {  // a / b, |a / b| < 2^64: (|a| << 320) / |b|, bit by bit
    uint32_t num[2 * W] = {};
    memcpy(num + FRAC / 32, a.w, sizeof a.w);
    Big q, rem;
    for (int bit = 2 * W * 32 - 1; bit >= 0; bit--) {
        uint32_t top = rem.w[W - 1] >> 31;
        for (int i = W - 1; i > 0; i--) rem.w[i] = rem.w[i] << 1 | rem.w[i - 1] >> 31;
        rem.w[0] = rem.w[0] << 1 | ((num[bit / 32] >> (bit % 32)) & 1);
        if (top || mag_cmp(rem, b) >= 0) {
            rem = mag_sub(rem, b);
            if (bit / 32 < W) q.w[bit / 32] |= 1u << (bit % 32);
        }
    }
    q.neg = a.neg != b.neg && !is_zero(q);
    return q;
}
Big from_double(double d) {  // exact for 2^-320 <= |d| < 2^64
    Big r;
    if (d == 0) return r;
    int e;
    double f = frexp(fabs(d), &e);
    uint64_t m = (uint64_t)ldexp(f, 53);  // d = m 2^(e - 53)
    int sh = e - 53 + FRAC;
    for (int bit = 0; bit < 53; bit++)
        if (m >> bit & 1) {
            int at = bit + sh;
            if (at >= 0 && at < W * 32) r.w[at / 32] |= 1u << (at % 32);
        }
    r.neg = d < 0;
    return r;
}
Big from_int(int64_t v) {
    Big r;
    uint64_t a = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    r.w[FRAC / 32] = (uint32_t)a, r.w[FRAC / 32 + 1] = (uint32_t)(a >> 32);
    r.neg = v < 0;
    return r;
}
double to_double(const Big &a) {  // (approximate)
    double v = 0;
    for (int i = W - 1; i >= 0; i--) v = v * 4294967296.0 + a.w[i];
    v = ldexp(v, -FRAC);
    return a.neg ? -v : v;
}
Big shifted(const Big &a, int sh) {  // a 2^sh (truncated)
    Big r;
    for (int bit = 0; bit < W * 32; bit++)
        if (a.w[bit / 32] >> (bit % 32) & 1) {
            int at = bit + sh;
            if (at >= 0 && at < W * 32) r.w[at / 32] |= 1u << (at % 32);
        }
    r.neg = a.neg;
    return r;
}

Big big_ln2() {  // 2 atanh(1/3)
    static const Big v = [] {
        Big u = div_u(from_int(1), 3), u2 = mul(u, u), term = u, sum;
        for (uint32_t k = 0; !is_zero(term); k++) {
            sum = add(sum, div_u(term, 2 * k + 1));
            term = mul(term, u2);
        }
        return mul_u(sum, 2);
    }();
    return v;
}

}  // namespace

double cr_pow_slow(double x, double y) {
    const Big LN2B = big_ln2();
    int e;
    double f = frexp(x, &e), m = 2 * f;
    int k = e - 1;
    if (m > SQRT2) m = f, k = e;
    Big s = div(add(from_double(m), from_int(-1)), add(from_double(m), from_int(1))), s2 = mul(s, s), term = s, sum;
    for (uint32_t i = 0; !is_zero(term); i++) {
        sum = add(sum, div_u(term, 2 * i + 1));
        term = mul(term, s2);
    }
    Big L = add(mul(from_int(k), LN2B), mul_u(sum, 2));
    Big z = mul(L, from_double(y));
    double zd = to_double(z);
    if (zd < -745.14) return 0.0;
    int64_t kk = (int64_t)nearbyint(zd / 0.6931471805599453);
    Big r = add(z, negate(mul(from_int(kk), LN2B)));
    Big E, t = from_int(1);
    for (uint32_t n = 1; !is_zero(t); n++) {
        E = add(E, t);
        t = div_u(mul(t, r), n);
    }
    // E 2^kk scaled to an integer U at the result's precision, as in cr_pow_fast
    int top = W * 32 - 1;
    while (top >= 0 && !(E.w[top / 32] >> (top % 32) & 1)) top--;
    int b = top - FRAC;
    int vb = b + (int)kk;
    int sc = vb >= -1022 ? 52 - vb : 1074;
    int sh = (int)kk + sc;
    Big U = shifted(E, sh);
    uint64_t n = (uint64_t)U.w[FRAC / 32] | (uint64_t)U.w[FRAC / 32 + 1] << 32;
    Big frac = U;
    frac.w[FRAC / 32] = frac.w[FRAC / 32 + 1] = 0;
    Big half;
    half.w[(FRAC - 1) / 32] = 1u << ((FRAC - 1) % 32);
    Big tol;  // (the error is below 2^-227 on U: within 2^-220 of a midpoint, it is one - an exact case)
    tol.w[(FRAC - 220) / 32] = 1u << ((FRAC - 220) % 32);
    Big d = add(frac, negate(half));
    if (mag_cmp(d, tol) <= 0) n += n & 1;  // (a tie: to even)
    else if (!d.neg) n += 1;
    return ldexp((double)n, -sc);
}

double cr_pow(double x, double y) {
    if (!(x > 0 && x < 1 && y > 0 && isfinite(y))) return pow(x, y);
    if (y == 1) return x;
    bool ok;
    double r = cr_pow_fast(x, y, &ok);
    return ok ? r : cr_pow_slow(x, y);
}

// float_pow (Objects/floatobject.c) for x >= 0 and y > 0: 0 ** y is 0, 1 ** y is 1, then the C library's pow - here
// the correctly rounded one
double py_pow(double x, double y) {
    if (x == 0) return 0.0;
    if (x == 1) return 1.0;
    return cr_pow(x, y);
}

}  // namespace solve
}  // namespace rando
