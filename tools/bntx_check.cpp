// Driver for tools/test_bntx_formats.py: runs the texture bank code (runtime/rando/fmt/bntx.h) on files and writes
// the result, so the script can compare it with what tools/rando/bntx_merge.py makes.
// Build (from the repo root):
//   c++ -std=c++17 -O2 -Iruntime tools/bntx_check.cpp runtime/rando/fmt/bntx.cpp runtime/rando/fmt/sarc.cpp \
//       -o build/bntx-check
// A bank is a path, or <archive>::<file> for one inside an archive (SARC).
//   bntx-check save <bank> <out>
//   bntx-check merge <out> <only list | -> <base> <other>...
//   bntx-check subset <out> <only list> <source>...
//   bntx-check copy <bank> <pairs> <out>                (pairs: "source\tnew" per line)
//   bntx-check patch <bank> <spec> <out>                (spec: "set\ttexture\tsource" or "ren\told\tnew" per line)
// Lists hold one name per line.
#include <stdio.h>

#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "rando/fmt/bntx.h"
#include "rando/fmt/sarc.h"

static std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("can't read " + path);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

static std::string read_bank(const std::string &spec) {
    size_t sep = spec.find("::");
    if (sep == std::string::npos) return read_file(spec);
    rando::Sarc arc = rando::Sarc::parse(read_file(spec.substr(0, sep)));
    const std::string *f = arc.get(spec.substr(sep + 2));
    if (!f) throw std::runtime_error("no " + spec);
    return *f;
}

static void write_file(const std::string &path, const std::string &data) {
    std::ofstream f(path, std::ios::binary);
    f.write(data.data(), (std::streamsize)data.size());
    if (!f) throw std::runtime_error("can't write " + path);
}

static std::vector<std::vector<std::string>> read_rows(const std::string &path) {
    std::vector<std::vector<std::string>> rows;
    std::istringstream in(read_file(path));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> row;
        size_t at = 0, tab;
        while ((tab = line.find('\t', at)) != std::string::npos) row.push_back(line.substr(at, tab - at)), at = tab + 1;
        row.push_back(line.substr(at));
        rows.push_back(row);
    }
    return rows;
}

static std::unordered_set<std::string> read_set(const std::string &path) {
    std::unordered_set<std::string> s;
    for (auto &r : read_rows(path)) s.insert(r[0]);
    return s;
}

static int run(int argc, char **argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "save" && argc == 4) {
        write_file(argv[3], rando::Bntx::parse(read_bank(argv[2])).save());
    } else if ((cmd == "merge" && argc >= 5) || (cmd == "subset" && argc >= 5)) {
        std::vector<std::string> banks;
        for (int i = 4; i < argc; i++) banks.push_back(read_bank(argv[i]));
        std::vector<const std::string *> ptrs;
        for (auto &b : banks) ptrs.push_back(&b);
        std::string out;
        if (cmd == "subset") {
            out = rando::bntx_subset(ptrs, read_set(argv[3]));
        } else {
            std::unordered_set<std::string> only;
            bool use = std::string(argv[3]) != "-";
            if (use) only = read_set(argv[3]);
            std::vector<const std::string *> others(ptrs.begin() + 1, ptrs.end());
            out = rando::bntx_merge(*ptrs[0], others, use ? &only : nullptr);
        }
        write_file(argv[2], out);
    } else if (cmd == "copy" && argc == 5) {
        std::vector<std::pair<std::string, std::string>> pairs;
        for (auto &r : read_rows(argv[3])) pairs.push_back({r.at(0), r.at(1)});
        write_file(argv[4], rando::bntx_copy_renamed(read_bank(argv[2]), pairs));
    } else if (cmd == "patch" && argc == 5) {
        std::vector<std::pair<std::string, std::string>> set_from, rename;
        for (auto &r : read_rows(argv[3])) (r.at(0) == "ren" ? rename : set_from).push_back({r.at(1), r.at(2)});
        write_file(argv[4], rando::bntx_patch_in_place(read_bank(argv[2]), set_from, rename));
    } else {
        fprintf(stderr, "usage: see the top of tools/bntx_check.cpp\n");
        return 2;
    }
    return 0;
}

int main(int argc, char **argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        fprintf(stderr, "bntx-check: %s\n", e.what());
        return 1;
    }
}
