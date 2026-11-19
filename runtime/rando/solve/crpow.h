// x ** y as Python computes it for the shuffler's item pacing (random.random() ** (top / weight)), the same on every
// platform.
//
// Python's float ** hands finite positive cases to the C library's pow(), and C libraries differ in the last bit
// (macOS, glibc, Android's bionic, Windows): a different bit can reorder two locations and change the seed. cr_pow
// gives the correctly rounded result instead (the exact x^y rounded to nearest, ties to even), which is what every
// accurate pow returns except on the rare inputs where x^y lies extremely close to halfway between two doubles.
// The seeds recorded with macOS's libm match it bit for bit (lasrando solve-check).
//
// How: x^y = exp(y ln x) in double-double arithmetic (about 104 bits), then Ziv's test: if the result's error bound
// can't straddle a rounding boundary, that rounding is the correct one; otherwise the same is done again with
// 320-bit fixed-point numbers. A result still within 2^-250 of a boundary is an exact case (x^y equal to a
// double or a midpoint: pow's hardest binary64 cases are far further away than that) and is rounded as exact.
#pragma once

namespace rando {
namespace solve {

double cr_pow(double x, double y);   // x > 0, x != 1, y > 0, both finite (other inputs: std::pow)
double py_pow(double x, double y);   // Python's float.__pow__ (x >= 0, y > 0): its special cases, then cr_pow

// test hooks: the double-double step alone (ok = false where it can't decide) and the fixed-point step alone
double cr_pow_fast(double x, double y, bool *ok);
double cr_pow_slow(double x, double y);

}  // namespace solve
}  // namespace rando
