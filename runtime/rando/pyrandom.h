// PyRandom: Python's `random.Random`, bit for bit, for the randomizer's native solver. Seed IDs made by the Python
// generator must keep giving the same seeds, so every draw it makes is reproduced exactly: the same Mersenne Twister
// state from the same seed (strings: version-2 seeding, the UTF-8 bytes followed by their SHA-512), and the same
// methods on top (random, getrandbits, _randbelow, randrange, choice, shuffle, choices, uniform).
//
// Written from the algorithms in CPython's Lib/random.py and Modules/_randommodule.c (MT19937: Matsumoto and
// Nishimura's reference init_genrand / init_by_array / genrand_int32); no code copied. Proven against CPython 3.12 by
// tools/test_pyrandom.cpp with the vectors tools/rando/pyrandom_golden.py writes.
//
// SHA-512: Monocypher (third_party/monocypher, CC0 / BSD-2): the including file compiles monocypher-ed25519.c, or
// defines PYRANDOM_SHA512 to its own.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef PYRANDOM_SHA512
extern "C" void crypto_sha512(uint8_t hash[64], const uint8_t *message, size_t message_size);
#define PYRANDOM_SHA512 crypto_sha512
#endif

class PyRandom {
public:
    PyRandom() { init_genrand(19650218u); }
    explicit PyRandom(const std::string &s) { seed(s); }

    // random.seed(str) (version 2): int.from_bytes(bytes + sha512(bytes), 'big') as the key
    void seed(const std::string &s) {
        std::vector<uint8_t> b(s.begin(), s.end());
        uint8_t h[64];
        PYRANDOM_SHA512(h, b.data(), b.size());
        b.insert(b.end(), h, h + 64);
        seed_bytes_be(b);
    }
    // random.seed(int): the key is abs(n)
    void seed_int(int64_t n) {
        uint64_t a = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
        std::vector<uint8_t> b(8);
        for (int i = 0; i < 8; i++) b[i] = (uint8_t)(a >> (56 - 8 * i));
        seed_bytes_be(b);
    }

    // genrand_res53: a double in [0, 1) from two draws
    double random() {
        uint32_t a = genrand() >> 5, b = genrand() >> 6;
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }

    // getrandbits(k), k <= 64: 32-bit words least significant first, the last one shifted down
    uint64_t getrandbits(int k) {
        if (k < 0 || k > 64) throw std::invalid_argument("getrandbits: k out of range");
        if (k == 0) return 0;
        if (k <= 32) return genrand() >> (32 - k);
        uint64_t lo = genrand(), hi = genrand();
        return lo | ((k < 64 ? hi >> (64 - k) : hi) << 32);
    }

    // _randbelow_with_getrandbits: n.bit_length() bits, drawn again while >= n
    uint64_t randbelow(uint64_t n) {
        int k = 0;
        for (uint64_t v = n; v; v >>= 1) k++;
        uint64_t r = getrandbits(k);
        while (r >= n) r = getrandbits(k);
        return r;
    }

    // randrange(stop) / randrange(start, stop[, step])
    int64_t randrange(int64_t stop) {
        if (stop <= 0) throw std::invalid_argument("empty range for randrange()");
        return (int64_t)randbelow((uint64_t)stop);
    }
    int64_t randrange(int64_t start, int64_t stop, int64_t step = 1) {
        int64_t width = stop - start;
        if (step == 1) {
            if (width <= 0) throw std::invalid_argument("empty range in randrange()");
            return start + (int64_t)randbelow((uint64_t)width);
        }
        int64_t n;
        if (step > 0) n = floordiv(width + step - 1, step);
        else if (step < 0) n = floordiv(width + step + 1, step);
        else throw std::invalid_argument("zero step for randrange()");
        if (n <= 0) throw std::invalid_argument("empty range in randrange()");
        return start + step * (int64_t)randbelow((uint64_t)n);
    }
    int64_t randint(int64_t a, int64_t b) { return randrange(a, b + 1); }

    template <class T> const T &choice(const std::vector<T> &seq) {
        if (seq.empty()) throw std::out_of_range("Cannot choose from an empty sequence");
        return seq[randbelow(seq.size())];
    }
    size_t choice_index(size_t n) {
        if (!n) throw std::out_of_range("Cannot choose from an empty sequence");
        return (size_t)randbelow(n);
    }

    template <class T> void shuffle(std::vector<T> &x) {
        for (size_t i = x.size(); i-- > 1;) {
            size_t j = (size_t)randbelow(i + 1);
            std::swap(x[i], x[j]);
        }
    }

    // choices(population, weights)[0] with integer weights: bisect_right(cum_weights, random() * total, 0, n - 1)
    size_t choices_index(const std::vector<int64_t> &weights) {
        if (weights.empty()) throw std::out_of_range("Cannot choose from an empty population");
        std::vector<int64_t> cum(weights.size());
        int64_t t = 0;
        for (size_t i = 0; i < weights.size(); i++) cum[i] = t += weights[i];
        double total = (double)cum.back();
        if (!(total > 0.0)) throw std::invalid_argument("Total of weights must be greater than zero");
        double x = random() * total;
        size_t lo = 0, hi = weights.size() - 1;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (x < (double)cum[mid]) hi = mid; else lo = mid + 1;
        }
        return lo;
    }
    // choices(population, k=1)[0] without weights: floor(random() * n)
    size_t choices_index(size_t n) { return (size_t)(random() * (double)n); }

    // (the product kept apart from the add: a fused multiply-add rounds once where CPython rounds twice, which moved
    // results by one bit in tools/test_pyrandom.cpp on arm64)
    double uniform(double a, double b) {
        volatile double p = (b - a) * random();
        return a + p;
    }

    // getstate() / setstate(): the object itself is the state
    struct State { uint32_t mt[624]; int mti; };
    State getstate() const { State s; memcpy(s.mt, mt_, sizeof mt_); s.mti = mti_; return s; }
    void setstate(const State &s) { memcpy(mt_, s.mt, sizeof mt_); mti_ = s.mti; }

private:
    uint32_t mt_[624];
    int mti_ = 625;

    static int64_t floordiv(int64_t a, int64_t b) {
        int64_t q = a / b;
        return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
    }

    // init_by_array with the 32-bit words of a big-endian number, least significant word first, at least one word
    void seed_bytes_be(const std::vector<uint8_t> &be) {
        size_t start = 0;
        while (start < be.size() && !be[start]) start++;
        size_t nbytes = be.size() - start;  // (leading zero bytes don't count: CPython keys on the number)
        std::vector<uint32_t> words((nbytes + 3) / 4);
        if (words.empty()) words.push_back(0);
        for (size_t i = 0; i < nbytes; i++) words[i / 4] |= (uint32_t)be[be.size() - 1 - i] << (8 * (i % 4));
        init_by_array(words.data(), words.size());
    }

    void init_genrand(uint32_t s) {
        mt_[0] = s;
        for (int i = 1; i < 624; i++) mt_[i] = 1812433253u * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + (uint32_t)i;
        mti_ = 624;
    }

    void init_by_array(const uint32_t *key, size_t len) {
        init_genrand(19650218u);
        size_t i = 1, j = 0;
        for (size_t k = 624 > len ? 624 : len; k; k--) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525u)) + key[j] + (uint32_t)j;
            i++, j++;
            if (i >= 624) { mt_[0] = mt_[623]; i = 1; }
            if (j >= len) j = 0;
        }
        for (size_t k = 623; k; k--) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941u)) - (uint32_t)i;
            i++;
            if (i >= 624) { mt_[0] = mt_[623]; i = 1; }
        }
        mt_[0] = 0x80000000u;
        mti_ = 624;
    }

    uint32_t genrand() {
        static const uint32_t mag01[2] = {0u, 0x9908b0dfu};
        if (mti_ >= 624) {
            int kk = 0;
            for (; kk < 624 - 397; kk++) {
                uint32_t y = (mt_[kk] & 0x80000000u) | (mt_[kk + 1] & 0x7fffffffu);
                mt_[kk] = mt_[kk + 397] ^ (y >> 1) ^ mag01[y & 1];
            }
            for (; kk < 623; kk++) {
                uint32_t y = (mt_[kk] & 0x80000000u) | (mt_[kk + 1] & 0x7fffffffu);
                mt_[kk] = mt_[kk + (397 - 624)] ^ (y >> 1) ^ mag01[y & 1];
            }
            uint32_t y = (mt_[623] & 0x80000000u) | (mt_[0] & 0x7fffffffu);
            mt_[623] = mt_[396] ^ (y >> 1) ^ mag01[y & 1];
            mti_ = 0;
        }
        uint32_t y = mt_[mti_++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= y >> 18;
        return y;
    }
};
