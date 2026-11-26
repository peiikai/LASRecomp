// Release tags for the update check (runtime/ui/launcher.mm): their order and the name players see.
// Tags: beta_<n> and its fixes releases beta_<n>.<m> (to Beta 5), v0.<n>.<m> from Beta 6, v1.0.0 and up after the beta.
// Under the hood every release is a semantic version: beta_<n> = 0.<n>.0, beta_<n>.<m> = 0.<n>.<m> (the macOS
// bundle version, tools/bundle_macos.sh). Test (tools/test_update_tags.cpp): no game, no network.
#pragma once
#include <stdio.h>
#include <initializer_list>
#include <string>

// Higher = newer; -1: not a release tag (alpha_*, test-*, anything else). v0.<n>.<m> ranks with beta_<n>.<m>
// (so Beta 5 builds offer v0.6.0 as a newer beta), v1.0.0+ above every beta.
static inline long long tag_rank(const std::string &t) {
    int n, m, a, b, c;
    char end;
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1 && n > 0 && n < (1 << 20)) return (long long)n << 12;
    if (sscanf(t.c_str(), "beta_%d.%d%c", &n, &m, &end) == 2 && n > 0 && n < (1 << 20) && m > 0 && m < 4096)
        return ((long long)n << 12) + m;
    if (sscanf(t.c_str(), "v%d.%d.%d%c", &a, &b, &c, &end) == 3 && a == 0 && b > 0 && b < (1 << 20) && c >= 0 && c < 4096)
        return ((long long)b << 12) + c;
    if (sscanf(t.c_str(), "v%d.%d.%d%c", &a, &b, &c, &end) == 3 && a >= 1 && a < (1 << 20) && b >= 0 && c >= 0 &&
        b < 4096 && c < 4096)
        return (1LL << 48) + ((long long)a << 24) + ((long long)b << 12) + c;
    return -1;
}

// "Beta 2" / "Beta 5.1" / "Beta 6" (v0.6.0) / "Beta 6.1" (v0.6.1) / "1.0.0" (with_word: "Version 1.0.0")
static inline std::string tag_label(const std::string &t, bool with_word) {
    int n, m, a;
    char end;
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1) return "Beta " + std::to_string(n);
    if (sscanf(t.c_str(), "beta_%d.%d%c", &n, &m, &end) == 2 && m > 0)
        return "Beta " + std::to_string(n) + "." + std::to_string(m);
    if (sscanf(t.c_str(), "v%d.%d.%d%c", &a, &n, &m, &end) == 3 && a == 0 && n > 0)
        return "Beta " + std::to_string(n) + (m > 0 ? "." + std::to_string(m) : std::string());
    if (!t.compare(0, 5, "test-")) return "Test build " + t.substr(5);
    if (t == "experimental") return "Experimental build";
    if (!t.compare(0, 13, "experimental-") && t.size() >= 21)  // experimental-<YYYYMMDDHHMM>-<commit>
        return "Experimental build " + t.substr(13, 4) + "-" + t.substr(17, 2) + "-" + t.substr(19, 2);
    if (t == "nightly") return "Nightly build";
    if (!t.compare(0, 8, "nightly-") && t.size() >= 16)  // nightly-<YYYYMMDDHHMM>-<commit>
        return "Nightly build " + t.substr(8, 4) + "-" + t.substr(12, 2) + "-" + t.substr(14, 2);
    std::string v = t.size() > 1 && t[0] == 'v' ? t.substr(1) : t;
    return with_word ? "Version " + v : v;
}

// Rolling builds are never on the normal releases: each kind sits on one rolling pre-release named after it, its files
// named LAS.Recomp-<kind>-<YYYYMMDDHHMM>-<commit>-<platform>.<ext>. Kinds: "experimental" (experimental platforms,
// from the retired experimental.yml) and "nightly" (every platform from develop, .github/workflows/nightly.yml).
// ReAwakening-<kind>-... counts too: the bridge for renaming those files later (the player-facing name), so builds
// from now on find either name. Builds from before only know LAS.Recomp-, so the files keep that name until players
// have moved on (tools/nightly_publish.sh can carry both names for a while).
// The newest stamp of that kind for this platform (e.g. "linux-aarch64"), found in the releases JSON; 0: none.
static inline long long newest_rolling(const std::string &body, const std::string &kind, const std::string &platform) {
    long long best = 0;
    for (const char *prefix : {"LAS.Recomp-", "ReAwakening-"}) {
        const std::string key = prefix + kind + "-";
        for (size_t at = 0; (at = body.find(key, at)) != std::string::npos;) {
            at += key.size();
            long long stamp = 0;
            size_t i = at;
            for (; i < body.size() && i - at < 12 && body[i] >= '0' && body[i] <= '9'; i++) stamp = stamp * 10 + (body[i] - '0');
            if (i - at != 12 || i >= body.size() || body[i] != '-') continue;
            size_t dash = body.find('-', i + 1);  // (after the commit)
            if (dash == std::string::npos || body.compare(dash + 1, platform.size(), platform) ||
                dash + 1 + platform.size() >= body.size() || body[dash + 1 + platform.size()] != '.')
                continue;
            if (stamp > best) best = stamp;
        }
    }
    return best;
}
static inline long long newest_experimental(const std::string &body, const std::string &platform) {
    return newest_rolling(body, "experimental", platform);
}
static inline long long newest_nightly(const std::string &body, const std::string &platform) {
    return newest_rolling(body, "nightly", platform);
}
