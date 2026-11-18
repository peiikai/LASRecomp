// Seed records (runtime/rando/seed.c) against the way the runtime read settings.json before: each reader searched the
// file's text ("\"escape-bottle\": true", atoi after "\"seashell-total\":", ...). For every key the runtime reads, the
// record must answer the same as that search over the whole file, for older seed folders and for their seed.json
// (`lasrando convert`).
//
// With no arguments: generated seed folders (no game files). With folders: those (e.g. rando/Dev* seeds).
// Build + run (from the repo root):
//   cc -std=gnu11 -O1 tools/test_seed_record.c runtime/rando/seed.c runtime/rando/json.c -o build/test-seed-record && build/test-seed-record
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../runtime/rando/seed.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// every settings.json key the runtime reads as true / false
static const char *const kBools[] = {"no-buff-drops", "tunic-upgrades", "underwater-items", "escape-bottle",
                                     "shuffle-bombs", "shuffle-powder", "hookshot-anything", "hint-sheet",
                                     "weather-cycle", "randomize-enemies", "marin-stays", "ghost-item", "bad-pets",
                                     "include-trade-gifts", "random-cosmetics"};

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    b[fread(b, 1, (size_t)n, f)] = 0;
    fclose(f);
    return b;
}

// the old readers, as they were (runtime/hle/qol.c, escape.c, ..., ui/inventory_overlay.mm, cosmetics_seed.c)
static int old_bool(const char *t, const char *key) {
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\": true", key);
    return strstr(t, pat) != NULL;
}
static int old_shell_total(const char *t) {  // silent_pickups.c: >0 counts, else the game's 50
    const char *k = strstr(t, "\"seashell-total\":");
    return k && atoi(k + 17) > 0 ? atoi(k + 17) : 50;
}
static int old_marker(const char *t) {  // miniboss.c: -1 when absent
    const char *k = strstr(t, "\"miniboss-marker-flag\":");
    return k ? atoi(k + strlen("\"miniboss-marker-flag\":")) : -1;
}
static int old_keys_shown(const char *t) {  // inventory_overlay.mm
    const char *k = strstr(t, "\"keysanity\":");
    static const char off[] = "\"keysanity\": \"off\"";
    return k && strncmp(k, off, sizeof off - 1) != 0;
}
static void old_seed(const char *t, const char *dir, char *out, size_t cap) {  // cosmetics_seed.c
    const char *name = dir, *end = dir + strlen(dir), *k = strstr(t, "\"seed\": \"");
    if (k && strchr(k + 9, '"')) name = k + 9, end = strchr(name, '"');
    snprintf(out, cap, "%.*s", (int)(end - name), name);
}

// the same answers from a record
static int new_shell_total(const RandoSeed *s) {
    int64_t v = rando_seed_int(s, "seashell-total", 0);
    return v > 0 ? (int)v : 50;
}
static int new_marker(const RandoSeed *s) { return (int)rando_seed_int(s, "miniboss-marker-flag", -1); }
static int new_keys_shown(const RandoSeed *s) {
    const char *k = rando_seed_str(s, "keysanity");
    return rando_seed_has(s, "keysanity") && !(k && !strcmp(k, "off"));
}
static void new_seed(const RandoSeed *s, const char *dir, char *out, size_t cap) {
    const char *v = rando_seed_str(s, "seed");
    snprintf(out, cap, "%s", v ? v : dir);
}

static void compare(const char *dir, const char *text, const RandoSeed *s, const char *what) {
    for (size_t i = 0; i < sizeof kBools / sizeof *kBools; i++)
        CHECK(old_bool(text, kBools[i]) == rando_seed_bool(s, kBools[i]), "%s (%s): %s", dir, what, kBools[i]);
    CHECK(old_shell_total(text) == new_shell_total(s), "%s (%s): seashell-total %d vs %d", dir, what,
          old_shell_total(text), new_shell_total(s));
    CHECK(old_marker(text) == new_marker(s), "%s (%s): miniboss-marker-flag", dir, what);
    CHECK(old_keys_shown(text) == new_keys_shown(s), "%s (%s): keysanity shown", dir, what);
    char a[256], b[256];
    old_seed(text, dir, a, sizeof a);
    new_seed(s, dir, b, sizeof b);
    CHECK(!strcmp(a, b), "%s (%s): seed \"%s\" vs \"%s\"", dir, what, a, b);
}

static void check_folder(const char *dir, const char *tmp) {
    char p[1200];
    snprintf(p, sizeof p, "%s/settings.json", dir);
    char *text = slurp(p);
    if (!text) { printf("skip %s (no settings.json)\n", dir); return; }
    const RandoSeed *s = rando_seed_load(dir);
    CHECK(s != NULL, "%s: no record", dir);
    if (!s) { free(text); return; }
    compare(dir, text, s, "folder");
    // the same seed as seed.json alone (lasrando convert): every answer unchanged, and it reads as format 1
    char conv[1200];
    snprintf(conv, sizeof conv, "%s/conv", tmp);
    mkdir(conv, 0755);
    size_t n;
    char *j = rando_seed_json(s, &n);
    snprintf(p, sizeof p, "%s/seed.json", conv);
    FILE *f = fopen(p, "wb");
    fwrite(j, 1, n, f);
    fclose(f);
    free(j);
    const RandoSeed *c = rando_seed_load(conv);
    CHECK(c && rando_seed_format(c) == 1, "%s: converted record didn't load as seed.json", dir);
    if (c) compare(dir, text, c, "seed.json");
    CHECK(c && !strcmp(rando_seed_id(c), rando_seed_id(s)), "%s: seed ID", dir);
    remove(p);
    rmdir(conv);
    free(text);
}

// generated folders: what the generator writes (json.dump: ", " / ": "), edge values
static void make(const char *tmp, const char *name, const char *settings, const char *id) {
    char d[1200], p[1300];
    snprintf(d, sizeof d, "%s/%s", tmp, name);
    mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/settings.json", d);
    FILE *f = fopen(p, "w");
    fputs(settings, f);
    fclose(f);
    if (id) {
        snprintf(p, sizeof p, "%s/seed_id.txt", d);
        f = fopen(p, "w");
        fprintf(f, "%s\n", id);
        fclose(f);
    }
}

int main(int argc, char **argv) {
    char tmp[] = "/tmp/seedrecXXXXXX";
    if (!mkdtemp(tmp)) { perror("mkdtemp"); return 2; }
    if (argc > 1) {
        for (int i = 1; i < argc; i++) check_folder(argv[i], tmp);
    } else {
        make(tmp, "all-on", "{\"seed\": \"MBHMM-13FR6-2000W-40GAZ-X700Z\", \"no-buff-drops\": true, \"tunic-upgrades\": true, "
             "\"underwater-items\": true, \"escape-bottle\": true, \"shuffle-bombs\": true, \"shuffle-powder\": true, "
             "\"hookshot-anything\": true, \"hint-sheet\": true, \"weather-cycle\": true, \"randomize-enemies\": true, "
             "\"marin-stays\": true, \"ghost-item\": true, \"bad-pets\": true, \"include-trade-gifts\": true, "
             "\"random-cosmetics\": true, \"seashell-total\": 37, \"miniboss-marker-flag\": 1771, \"keysanity\": \"all\"}",
             "MBHMM-13FR6-2000W-40GAZ-X700Z");
        make(tmp, "all-off", "{\"seed\": \"5F5HF-AB703-5RKZW-E4VTZ-9D100-0035Z\", \"no-buff-drops\": false, "
             "\"escape-bottle\": false, \"seashell-total\": 0, \"keysanity\": \"off\", \"bad-pets\": false}", NULL);
        make(tmp, "old-seed", "{\"seed\": \"OldName\", \"logic\": \"basic\"}", NULL);  // (no newer keys at all)
        make(tmp, "no-seed-key", "{\"keysanity\": \"keys\", \"seashell-total\": -3}", "");
        make(tmp, "damaged", "{\"escape-bottle\": tru", NULL);  // (not JSON: everything off, as the text search too)
        // a big settings.json (room shuffle's dungeon-of grows with rooms): the keys late in the file still count
        {
            size_t cap = 1 << 20, n = 0;
            char *b = malloc(cap);
            n += (size_t)snprintf(b + n, cap - n, "{\"seed\": \"BIG\", \"dungeon-of\": {");
            for (int i = 0; i < 4000; i++) n += (size_t)snprintf(b + n, cap - n, "%s\"pot-%04d\": \"Lv0%d\"", i ? ", " : "", i, i % 9);
            snprintf(b + n, cap - n, "}, \"escape-bottle\": true, \"hint-sheet\": true, \"ghost-item\": true, \"marin-stays\": true, "
                     "\"bad-pets\": true, \"seashell-total\": 49}");
            make(tmp, "big", b, NULL);
            free(b);
        }
        const char *names[] = {"all-on", "all-off", "old-seed", "no-seed-key", "damaged", "big"};
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            char d[1200];
            snprintf(d, sizeof d, "%s/%s", tmp, names[i]);
            check_folder(d, tmp);
        }
        // the damaged folder still loads (as the old readers' text search: everything off)
        char d[1200];
        snprintf(d, sizeof d, "%s/damaged", tmp);
        const RandoSeed *s = rando_seed_load(d);
        CHECK(s && !rando_seed_bool(s, "escape-bottle"), "damaged settings.json: record with everything off");
    }
    char cmd[1300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    if (system(cmd)) {}
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
