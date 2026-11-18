// lasrando: the randomizer's command-line tool, built from the same code the game runs (runtime/rando/).
//
//   lasrando convert <seed folder> [<out.json>]   the folder's record as seed.json (an older folder: read from its
//                                                  settings.json + seed_id.txt), to <out.json> or stdout
//   lasrando get <seed folder> <key>...            settings as the runtime reads them: one line per key,
//                                                  "<key> bool=<0|1> int=<n|-> str=<s|->"
//
// Build: make lasrando (build/lasrando).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/rando/seed.h"

static int usage(void) {
    fprintf(stderr, "usage: lasrando convert <seed folder> [<out.json>]\n"
                    "       lasrando get <seed folder> <key>...\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    const RandoSeed *s = rando_seed_load(argv[2]);
    if (!s) { fprintf(stderr, "%s: no seed there\n", argv[2]); return 1; }
    if (!strcmp(argv[1], "convert")) {
        size_t n;
        char *j = rando_seed_json(s, &n);
        if (!j) return 1;
        FILE *f = argc > 3 ? fopen(argv[3], "wb") : stdout;
        if (!f) { perror(argv[3]); return 1; }
        fwrite(j, 1, n, f);
        if (f != stdout) fclose(f);
        free(j);
        return 0;
    }
    if (!strcmp(argv[1], "get")) {
        for (int i = 3; i < argc; i++) {
            const char *k = argv[i], *str = rando_seed_str(s, k);
            int64_t v = rando_seed_int(s, k, INT64_MIN);
            printf("%s bool=%d int=", k, rando_seed_bool(s, k));
            if (v == INT64_MIN) printf("-"); else printf("%lld", (long long)v);
            printf(" str=%s\n", str ? str : "-");
        }
        return 0;
    }
    return usage();
}
