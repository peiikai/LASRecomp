// Make every folder of a file's path ("a/b/c.txt" makes a/ and a/b/), on every platform.
#pragma once
#include <string>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

namespace rando {
inline void make_dir(const std::string &dir) {
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
}
inline void make_parent_dirs(const std::string &path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') make_dir(path.substr(0, i));
}
}  // namespace rando
