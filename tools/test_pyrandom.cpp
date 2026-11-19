// runtime/rando/pyrandom.h against CPython: replays tools/rando/pyrandom_golden.txt (values recorded from CPython 3.12)
// and checks every result. No game files.
// Build + run (from the repo root):
//   c++ -std=c++17 -O1 -Iruntime tools/test_pyrandom.cpp -o build/test-pyrandom && build/test-pyrandom
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// (Monocypher's C compiled into this file as C++, so one command builds it on every platform)
#include "../third_party/monocypher/monocypher.c"
#undef FOR
#include "../third_party/monocypher/monocypher-ed25519.c"
#define PYRANDOM_SHA512 crypto_sha512
#include "rando/pyrandom.h"

static double from_bits(const std::string &h) {
    uint64_t u = strtoull(h.c_str(), NULL, 16);
    double d;
    memcpy(&d, &u, 8);
    return d;
}
static std::string bits(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)u);
    return b;
}
static std::vector<long long> ints(const std::string &s) {
    std::vector<long long> v;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ',')) v.push_back(atoll(t.c_str()));
    return v;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tools/rando/pyrandom_golden.txt";
    std::ifstream f(path);
    if (!f) { printf("can't open %s\n", path); return 2; }
    PyRandom r;
    std::string line, seed_desc;
    int n = 0, fails = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // (a Windows checkout may add CRs)
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string op;
        in >> op;
        if (op == "seed") {
            std::string kind, arg;
            in >> kind;
            std::getline(in, arg);
            if (!arg.empty() && arg[0] == ' ') arg.erase(0, 1);
            seed_desc = kind + " " + arg;
            if (kind == "str") {
                std::string s;
                for (size_t i = 0; i + 1 < arg.size(); i += 2) s += (char)strtol(arg.substr(i, 2).c_str(), NULL, 16);
                r.seed(s);
            } else {
                r.seed_int(atoll(arg.c_str()));
            }
            continue;
        }
        size_t eq = line.find(" = ");
        std::string args = line.substr(op.size(), eq - op.size()), want = line.substr(eq + 3), got;
        std::istringstream a(args);
        if (op == "random") got = bits(r.random());
        else if (op == "getrandbits") { int k; a >> k; got = std::to_string((unsigned long long)r.getrandbits(k)); }
        else if (op == "randbelow") { unsigned long long m; a >> m; got = std::to_string((unsigned long long)r.randbelow(m)); }
        else if (op == "randrange1") { long long m; a >> m; got = std::to_string((long long)r.randrange(m)); }
        else if (op == "randrange") { long long x, y, s; a >> x >> y >> s; got = std::to_string((long long)r.randrange(x, y, s)); }
        else if (op == "randint") { long long x, y; a >> x >> y; got = std::to_string((long long)r.randint(x, y)); }
        else if (op == "choice") {
            long long m; a >> m;
            std::vector<long long> v;
            for (long long i = 0; i < m; i++) v.push_back(i);
            got = std::to_string(r.choice(v));
        } else if (op == "shuffle") {
            long long m; a >> m;
            std::vector<long long> v;
            for (long long i = 0; i < m; i++) v.push_back(i);
            r.shuffle(v);
            for (size_t i = 0; i < v.size(); i++) got += (i ? "," : "") + std::to_string(v[i]);
        } else if (op == "choices") {
            std::string w; a >> w;
            std::vector<long long> wl = ints(w);
            got = std::to_string(r.choices_index(std::vector<int64_t>(wl.begin(), wl.end())));
        } else if (op == "choicesu") { long long m; a >> m; got = std::to_string(r.choices_index((size_t)m)); }
        else if (op == "uniform") { std::string x, y; a >> x >> y; got = bits(r.uniform(from_bits(x), from_bits(y))); }
        else { printf("unknown op %s\n", op.c_str()); return 2; }
        n++;
        if (got != want && fails++ < 20)
            printf("FAIL seed %s: %s -> %s, CPython %s\n", seed_desc.c_str(), line.substr(0, eq).c_str(), got.c_str(),
                   want.c_str());
    }
    printf("%d checks, %d failed\n", n, fails);
    return fails ? 1 : 0;
}
