// Update check tags (runtime/ui/update_tags.h): their order and labels; the macOS build numbers
// (tools/bundle_version.sh) go up in the same order. The update dialog's changelog (runtime/ui/update_notes.h). No game,
// no network.
// Build + run (from the repo root): clang++ -std=c++17 -O1 tools/test_update_tags.cpp -o build/test-update-tags &&
// build/test-update-tags
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "../runtime/ui/update_notes.h"  // (and update_tags.h)

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// tools/bundle_version.sh <tag> [<test-of>] -> CFBundleVersion ("" when it fails)
static std::string bundle_build(const char *tag, const char *test_of = "") {
    std::string cmd = std::string("sh tools/bundle_version.sh '") + tag + "' '" + test_of + "' 2>/dev/null";
    FILE *f = popen(cmd.c_str(), "r");
    char shortv[64] = "", build[64] = "";
    int n = f ? fscanf(f, "%63s %63s", shortv, build) : 0;
    int rc = f ? pclose(f) : -1;
    return n == 2 && rc == 0 ? build : "";
}

// macOS compares bundle versions part by part, numerically (a missing part is 0): <0, 0, >0
static int bundle_cmp(const std::string &a, const std::string &b) {
    std::vector<long> x, y;
    for (const char *p = a.c_str(); *p; p += *p == '.') x.push_back(strtol(p, (char **)&p, 10));
    for (const char *p = b.c_str(); *p; p += *p == '.') y.push_back(strtol(p, (char **)&p, 10));
    for (size_t i = 0; i < x.size() || i < y.size(); i++) {
        long u = i < x.size() ? x[i] : 0, v = i < y.size() ? y[i] : 0;
        if (u != v) return u < v ? -1 : 1;
    }
    return 0;
}

int main(void) {
    // oldest to newest; equal ranks share a group
    const char *order[][2] = {
        {"beta_1", "v0.1.0"}, {"beta_4", NULL}, {"beta_5", "v0.5.0"}, {"beta_5.1", "v0.5.1"}, {"beta_5.2", NULL},
        {"beta_5.3", "v0.5.3"}, {"beta_5.4", NULL}, {"v0.6.0", "beta_6"}, {"v0.6.1", "beta_6.1"}, {"v0.6.12", NULL},
        {"v0.7.0", NULL}, {"v0.99.0", NULL}, {"v1.0.0", NULL}, {"v1.0.1", NULL}, {"v1.1.0", NULL}, {"v2.0.0", NULL},
    };
    int n = sizeof order / sizeof *order;
    for (int i = 0; i < n; i++) {
        long long r = tag_rank(order[i][0]);
        CHECK(r > 0, "%s ranks (%lld)", order[i][0], r);
        if (order[i][1]) CHECK(tag_rank(order[i][1]) == r, "%s == %s", order[i][1], order[i][0]);
        for (int j = i + 1; j < n; j++)
            CHECK(tag_rank(order[j][0]) > r, "%s > %s", order[j][0], order[i][0]);
    }
    // macOS build numbers: one number that goes up with every release (equal ranks: the same number), above what
    // Beta 5.2 shipped (0.9.5.2); a test build sits between the release before the one it previews and that release
    for (int i = 0; i < n; i++) {
        std::string b = bundle_build(order[i][0]);
        CHECK(!b.empty() && b.find('.') == std::string::npos, "%s: build number '%s'", order[i][0], b.c_str());
        CHECK(bundle_cmp(b, "0.9.5.2") > 0, "%s: build %s above Beta 5.2's 0.9.5.2", order[i][0], b.c_str());
        if (order[i][1]) CHECK(bundle_build(order[i][1]) == b, "%s build == %s build", order[i][1], order[i][0]);
        if (i + 1 < n) {
            std::string c = bundle_build(order[i + 1][0]);
            CHECK(bundle_cmp(c, b) > 0, "%s build %s > %s build %s", order[i + 1][0], c.c_str(), order[i][0], b.c_str());
        }
    }
    CHECK(bundle_build("beta_5.3") == "503" && bundle_build("v0.6.0") == "600" && bundle_build("v1.0.0") == "10000",
          "build numbers 503 / 600 / 10000");
    {
        std::string t = bundle_build("test-foo", "beta_5.4");
        CHECK(bundle_cmp(t, bundle_build("beta_5.3")) > 0 && bundle_cmp(t, bundle_build("beta_5.4")) < 0,
              "test build of beta_5.4: %s between 5.3 and 5.4", t.c_str());
        CHECK(bundle_build("test-foo").empty() && bundle_build("alpha_3").empty(), "no build number without a release");
        // nightlies: each round above the last (Sparkle's feed), all between the release before and the one previewed
        std::string n1 = bundle_build("nightly-202611161000-abc1234", "v0.7.0"), n2 = bundle_build("nightly-202611171000-def5678", "v0.7.0");
        CHECK(n1 == "699.1.202611161000", "nightly build number: %s", n1.c_str());
        CHECK(bundle_cmp(n2, n1) > 0 && bundle_cmp(n1, bundle_build("v0.6.9")) > 0 && bundle_cmp(n2, bundle_build("v0.7.0")) < 0,
              "nightlies in order: %s %s", n1.c_str(), n2.c_str());
        CHECK(bundle_build("nightly-2026-abc", "v0.7.0").empty(), "bad nightly stamp");
    }
    const char *never[] = {"alpha_3", "test-388-grass", "beta_0", "beta_5.0", "v0.0.1", "v1.0", "v0.6.0-rc1", "beta_5.3x",
                           "ci-test-1", "", "v", "beta_", "experimental", "experimental-202610302315-abc1234", "nightly",
                           "nightly-202611160400-abc1234"};
    for (const char *t : never) CHECK(tag_rank(t) == -1, "'%s' is not a release (%lld)", t, tag_rank(t));

    const char *labels[][3] = {  // tag, label, with the word
        {"beta_5", "Beta 5", "Beta 5"},
        {"beta_5.3", "Beta 5.3", "Beta 5.3"},
        {"v0.6.0", "Beta 6", "Beta 6"},
        {"v0.6.1", "Beta 6.1", "Beta 6.1"},
        {"v0.10.2", "Beta 10.2", "Beta 10.2"},
        {"v1.0.0", "1.0.0", "Version 1.0.0"},
        {"v1.2.3", "1.2.3", "Version 1.2.3"},
        {"test-foo", "Test build foo", "Test build foo"},
        {"experimental", "Experimental build", "Experimental build"},
        {"experimental-202610302315-abc1234", "Experimental build 2026-10-30", "Experimental build 2026-10-30"},
        {"nightly", "Nightly build", "Nightly build"},
        {"nightly-202611160400-abc1234", "Nightly build 2026-11-16", "Nightly build 2026-11-16"},
    };
    for (auto &l : labels) {
        std::string a = tag_label(l[0], false), b = tag_label(l[0], true);
        CHECK(a == l[1], "%s -> '%s', want '%s'", l[0], a.c_str(), l[1]);
        CHECK(b == l[2], "%s (with word) -> '%s', want '%s'", l[0], b.c_str(), l[2]);
    }
    {  // the newest experimental build of one platform, from asset names
        std::string body = "\"name\":\"LAS.Recomp-experimental-202610302315-abc1234-linux-aarch64.AppImage\"\n"
                           "\"name\":\"LAS.Recomp-experimental-202611011200-def5678-linux-aarch64.AppImage\"\n"
                           "\"name\":\"LAS.Recomp-experimental-202612010000-0123456-windows-arm64.zip\"\n"
                           "\"name\":\"LAS.Recomp-experimental-2026120100-0123456-linux-aarch64.AppImage\"\n"
                           "\"name\":\"LAS.Recomp-experimental-202612020000-0123456-linux-aarch64x.AppImage\"\n"
                           "\"name\":\"LAS.Recomp-beta_5.4-linux-aarch64.AppImage\"\n";
        CHECK(newest_experimental(body, "linux-aarch64") == 202611011200LL, "linux-aarch64: %lld",
              newest_experimental(body, "linux-aarch64"));
        CHECK(newest_experimental(body, "windows-arm64") == 202612010000LL, "windows-arm64");
        CHECK(newest_experimental(body, "linux-x86_64") == 0 && newest_experimental("", "linux-aarch64") == 0, "none");
    }
    {  // the newest nightly of one platform; experimental files don't count, nor the other way round
        std::string body = "\"name\":\"LAS.Recomp-nightly-202611160400-abc1234-macOS.dmg\"\n"
                           "\"name\":\"LAS.Recomp-nightly-202611170400-def5678-macOS.dmg\"\n"
                           "\"name\":\"LAS.Recomp-nightly-202611170400-def5678-android.apk\"\n"
                           "\"name\":\"LAS.Recomp-experimental-202612010000-0123456-macOS.dmg\"\n"
                           "\"name\":\"LAS.Recomp-nightly-202611180400-0123456-windows-arm64.zip\"\n";
        CHECK(newest_nightly(body, "macOS") == 202611170400LL, "nightly macOS: %lld", newest_nightly(body, "macOS"));
        CHECK(newest_nightly(body, "android") == 202611170400LL, "nightly android");
        CHECK(newest_nightly(body, "windows") == 0, "nightly windows: none (windows-arm64 is another platform)");
        CHECK(newest_experimental(body, "macOS") == 202612010000LL, "experimental macOS ignores nightlies");
    }
    {  // Windows x64's zip renamed (2026-11): builds from before look for "-windows.", newer ones for "-windows-x64.";
       // the handover rounds carry both names, the later ones only the new one
        std::string handover = "\"name\":\"LAS.Recomp-nightly-202611200400-abc1234-windows-x64.zip\"\n"
                               "\"name\":\"LAS.Recomp-nightly-202611200400-abc1234-windows.zip\"\n"
                               "\"name\":\"LAS.Recomp-nightly-202611210400-def5678-windows-arm64.zip\"\n";
        std::string later = "\"name\":\"LAS.Recomp-nightly-202612200400-abc1234-windows-x64.zip\"\n";
        CHECK(newest_nightly(handover, "windows") == 202611200400LL, "old builds see the handover round");
        CHECK(newest_nightly(handover, "windows-x64") == 202611200400LL, "new builds see it too");
        CHECK(newest_nightly(handover, "windows-arm64") == 202611210400LL, "Windows on ARM: its own file");
        CHECK(newest_nightly(later, "windows-x64") == 202612200400LL, "new builds: the new name alone");
        CHECK(newest_nightly(later, "windows") == 0, "old builds don't take windows-x64 for theirs");
    }
    {  // the file name bridge: ReAwakening-<kind>-... counts like LAS.Recomp-<kind>-..., the newest of either wins
        std::string both = "\"name\":\"LAS.Recomp-nightly-202612010400-abc1234-macOS.dmg\"\n"
                           "\"name\":\"ReAwakening-nightly-202612010400-abc1234-macOS.dmg\"\n";
        std::string renamed = "\"name\":\"LAS.Recomp-nightly-202612010400-abc1234-macOS.dmg\"\n"
                              "\"name\":\"ReAwakening-nightly-202612020400-def5678-macOS.dmg\"\n"
                              "\"name\":\"ReAwakening-nightly-202612030400-0123456-android.apk\"\n"
                              "\"name\":\"ReAwakening-experimental-202612040400-0123456-linux-aarch64.AppImage\"\n"
                              "\"name\":\"ReAwakening-v0.7.0-macOS.dmg\"\n";
        CHECK(newest_nightly(both, "macOS") == 202612010400LL, "both names, one round");
        CHECK(newest_nightly(renamed, "macOS") == 202612020400LL, "the new name only: %lld", newest_nightly(renamed, "macOS"));
        CHECK(newest_nightly(renamed, "android") == 202612030400LL, "android under the new name");
        CHECK(newest_experimental(renamed, "linux-aarch64") == 202612040400LL, "experimental under the new name");
        CHECK(newest_nightly(renamed, "linux-aarch64") == 0, "experimental files aren't nightlies");
        CHECK(newest_nightly("\"name\":\"ReAwakening-nightly-2026120204-def5678-macOS.dmg\"\n", "macOS") == 0,
              "a short stamp doesn't count");
    }
    {  // the self-updater's manifests on the nightly page are never taken for a nightly build
        std::string body = "\"name\":\"update-macOS.txt\"\n\"name\":\"update-windows.txt\"\n"
                           "\"browser_download_url\":\"https://github.com/x/releases/download/nightly/update-windows.txt\"\n";
        CHECK(newest_nightly(body, "windows") == 0 && newest_nightly(body, "macOS") == 0, "manifests aren't nightlies");
    }
    {  // the releases JSON: tag and notes of each, nested objects and escapes on the way
        std::string json = "[{\"url\":\"x\",\"author\":{\"login\":\"a\",\"body\":\"not this\"},\"tag_name\":\"v0.7.0\","
                           "\"assets\":[{\"name\":\"a\",\"size\":3}],\"prerelease\":false,"
                           "\"body\":\"# LAS Recomp Beta 7\\r\\n\\r\\n- \\\"quoted\\\" caf\\u00e9 \\ud83d\\ude00\"},"
                           " {\"tag_name\":\"nightly\",\"body\":null}, {\"tag_name\":\"beta_6\",\"body\":\"B6\"}]";
        std::vector<ReleaseInfo> r = json_releases(json);
        CHECK(r.size() == 3, "3 releases: %zu", r.size());
        if (r.size() == 3) {
            CHECK(r[0].tag == "v0.7.0" && r[0].body == "# LAS Recomp Beta 7\r\n\r\n- \"quoted\" caf\xc3\xa9 \xf0\x9f\x98\x80",
                  "first: '%s' '%s'", r[0].tag.c_str(), r[0].body.c_str());
            CHECK(r[1].tag == "nightly" && r[1].body.empty() && r[2].tag == "beta_6" && r[2].body == "B6", "others");
        }
        CHECK(json_releases("").empty() && json_releases("[{\"tag_name\":\"x").empty() && json_releases("{}").empty(), "broken JSON");
    }
    {  // a release's notes: banner, "Built from", sha256 table and comments out
        std::string b = "> [!WARNING]\r\n> **Test build.** Not for general use.\r\n\r\n# LAS Recomp Beta 7 Changelog\r\n\r\n"
                        "## New\r\n\r\n- One <!-- hidden -->thing\r\n- Two\r\n\r\n---\r\n\r\nBuilt from abc.\r\n\r\n"
                        "| File | sha256 |\r\n|---|---|\r\n| `a` | `b` |\r\n";
        std::string c = clean_release_notes(b);
        CHECK(c == "# LAS Recomp Beta 7 Changelog\n\n## New\n\n- One thing\n- Two", "clean: [%s]", c.c_str());
        CHECK(clean_release_notes("Release notes: (to be written)\n\nBuilt from x.\n\n| File | sha256 |") ==
              "Release notes: (to be written)", "draft notes");
        CHECK(clean_release_notes("a\n<!-- one\ntwo -->\nb") == "a\nb", "comment over lines: [%s]",
              clean_release_notes("a\n<!-- one\ntwo -->\nb").c_str());
    }
    {  // every release skipped, newest first; nothing older than this build, nothing newer than the offer
        std::vector<ReleaseInfo> r = {{"v0.6.1", "- fix one"}, {"beta_6", "# LAS Recomp Beta 6 Changelog\n\n- six"},
                                      {"v0.7.0", "# LAS Recomp Beta 7 Changelog\n\n- seven"}, {"v0.6.2", ""},
                                      {"test-x", "no"}, {"nightly", "no"}, {"v0.8.0", "not offered"}};
        std::string c = release_changelog(r, tag_rank("v0.6.0"), "v0.7.0");
        CHECK(c == "# LAS Recomp Beta 7 Changelog\n\n- seven\n\n# Beta 6.2\n\n# Beta 6.1\n\n- fix one", "changelog: [%s]", c.c_str());
        CHECK(release_changelog(r, tag_rank("v0.7.0"), "v0.7.0").empty(), "nothing newer");
    }
    {  // nightly rounds newer than this build
        std::string body = "> [!WARNING]\n> x\n\n## Changes\n\n<!-- nightly-changes: 202611171000 -->\n**2026-11-17**\n\n"
                           "- Third (#3)\n\n<!-- nightly-changes: 202611161000 -->\n**2026-11-16**\n\n- Second (#2)\n- First (#1)\n\n"
                           "To report a problem, open an issue.\n\n<!-- nightly-round: 202611171000 abc -->\n";
        std::vector<ReleaseInfo> r = {{"beta_6", "x"}, {"nightly", body}};
        std::string c = nightly_changelog(r, 202611161000LL);
        CHECK(c == "**2026-11-17**\n\n- Third (#3)", "one round: [%s]", c.c_str());
        c = nightly_changelog(r, 202611151000LL);
        CHECK(c == "**2026-11-17**\n\n- Third (#3)\n\n**2026-11-16**\n\n- Second (#2)\n- First (#1)", "two rounds: [%s]", c.c_str());
        CHECK(nightly_changelog(r, 202611171000LL).empty() && nightly_changelog({}, 0).empty(), "none newer");
    }
    {  // inline Markdown out
        CHECK(md_inline("**Bold** and `code`, [a link](https://x.y/z) <https://a.b> \\*star\\*") ==
              "Bold and code, a link https://a.b *star*", "inline: [%s]",
              md_inline("**Bold** and `code`, [a link](https://x.y/z) <https://a.b> \\*star\\*").c_str());
    }
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("update tags: all passed\n");
    return 0;
}
