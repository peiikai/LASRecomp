// Seed records (runtime/rando/seed.h): seed.json, or an older seed folder's settings.json + seed_id.txt read into the
// same record.
#include "seed.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../third_party/yyjson/yyjson.h"

#define SEED_FORMAT "lasrecomp-seed"
#define SEED_VERSION 1

struct RandoSeed {
    yyjson_doc *doc;
    yyjson_val *settings;  // (an object; never NULL)
    const char *id;
    int format;
};

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap);
    for (size_t r; buf && (r = fread(buf + n, 1, cap - n, f)) > 0;) {
        n += r;
        if (n == cap) {
            char *b = realloc(buf, cap *= 2);
            if (!b) { free(buf); buf = NULL; }
            buf = b;
        }
    }
    fclose(f);
    if (buf) *len = n;
    return buf;
}

static RandoSeed *wrap(yyjson_doc *doc, int format) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) { yyjson_doc_free(doc); return NULL; }
    RandoSeed *s = calloc(1, sizeof *s);
    if (!s) { yyjson_doc_free(doc); return NULL; }
    s->doc = doc;
    s->format = format;
    s->settings = yyjson_obj_get(root, "settings");
    if (!yyjson_is_obj(s->settings)) {  // (keep readers simple: an empty object)
        static yyjson_doc *empty;
        if (!empty) empty = yyjson_read("{}", 2, 0);
        s->settings = yyjson_doc_get_root(empty);
    }
    s->id = yyjson_get_str(yyjson_obj_get(root, "seed_id"));
    if (!s->id) s->id = "";
    return s;
}

// seed.json: the record as written
static RandoSeed *load_json(const char *dir) {
    char p[1100];
    snprintf(p, sizeof p, "%s/seed.json", dir);
    size_t n;
    char *raw = read_all(p, &n);
    if (!raw) return NULL;
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts(raw, n, 0, NULL, &err);
    free(raw);
    if (!doc) { fprintf(stderr, "[rando] %s: not a seed record (%s at %zu)\n", p, err.msg, err.pos); return NULL; }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *fmt = yyjson_get_str(yyjson_obj_get(root, "format"));
    int64_t ver = yyjson_get_sint(yyjson_obj_get(root, "version"));
    if (!fmt || strcmp(fmt, SEED_FORMAT) || ver < 1) {
        fprintf(stderr, "[rando] %s: not a seed record\n", p);
        yyjson_doc_free(doc);
        return NULL;
    }
    if (ver > SEED_VERSION)  // (a newer app's seed: read what this one knows)
        fprintf(stderr, "[rando] %s: record version %lld, this build reads %d\n", p, (long long)ver, SEED_VERSION);
    return wrap(doc, (int)ver);
}

// An older seed folder: settings.json (the generator's settings, json.dump) and seed_id.txt
static RandoSeed *load_folder(const char *dir) {
    char p[1100];
    snprintf(p, sizeof p, "%s/settings.json", dir);
    size_t n;
    char *raw = read_all(p, &n);
    if (!raw) return NULL;
    yyjson_doc *settings = yyjson_read(raw, n, 0);
    free(raw);
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(m);
    yyjson_mut_doc_set_root(m, root);
    yyjson_mut_obj_add_str(m, root, "format", SEED_FORMAT);
    yyjson_mut_obj_add_int(m, root, "version", SEED_VERSION);
    char id[128] = "";
    snprintf(p, sizeof p, "%s/seed_id.txt", dir);
    FILE *f = fopen(p, "r");
    if (f) {
        if (!fgets(id, sizeof id, f)) id[0] = 0;
        fclose(f);
        id[strcspn(id, "\r\n")] = 0;
    }
    yyjson_mut_obj_add_strcpy(m, root, "seed_id", id);
    yyjson_mut_val *sv = settings && yyjson_is_obj(yyjson_doc_get_root(settings))
                             ? yyjson_val_mut_copy(m, yyjson_doc_get_root(settings))
                             : yyjson_mut_obj(m);  // (a damaged settings.json: no settings, as before)
    if (!settings) fprintf(stderr, "[rando] %s/settings.json: not JSON; the seed's settings read as off\n", dir);
    yyjson_mut_obj_add_val(m, root, "settings", sv);
    yyjson_doc_free(settings);
    yyjson_doc *doc = yyjson_mut_doc_imut_copy(m, NULL);
    yyjson_mut_doc_free(m);
    return doc ? wrap(doc, 0) : NULL;
}

const RandoSeed *rando_seed_load(const char *dir) {
    if (!dir || !dir[0]) return NULL;
    RandoSeed *s = load_json(dir);
    return s ? s : load_folder(dir);
}

void rando_seed_free(const RandoSeed *s) {
    if (!s || s == rando_seed()) return;  // (never the active one: seed.h)
    yyjson_doc_free(s->doc);
    free((void *)s);
}

bool rando_seed_dir_bool(const char *dir, const char *key) {
    const RandoSeed *s = rando_seed_load(dir);
    bool v = rando_seed_bool(s, key);
    rando_seed_free(s);
    return v;
}

bool rando_seed_has(const RandoSeed *s, const char *key) { return s && yyjson_obj_get(s->settings, key); }

bool rando_seed_bool(const RandoSeed *s, const char *key) {
    return s && yyjson_is_true(yyjson_obj_get(s->settings, key));
}

int64_t rando_seed_int(const RandoSeed *s, const char *key, int64_t dflt) {
    yyjson_val *v = s ? yyjson_obj_get(s->settings, key) : NULL;
    if (yyjson_is_sint(v)) return yyjson_get_sint(v);
    if (yyjson_is_uint(v)) return (int64_t)yyjson_get_uint(v);
    return dflt;
}

const char *rando_seed_str(const RandoSeed *s, const char *key) {
    return s ? yyjson_get_str(yyjson_obj_get(s->settings, key)) : NULL;
}

const char *rando_seed_id(const RandoSeed *s) { return s ? s->id : ""; }
int rando_seed_format(const RandoSeed *s) { return s ? s->format : 0; }

char *rando_seed_json(const RandoSeed *s, size_t *len) {
    if (!s) return NULL;
    return yyjson_write(s->doc, YYJSON_WRITE_PRETTY_TWO_SPACES | YYJSON_WRITE_NEWLINE_AT_END, len);
}

// ---- the active seed
static const RandoSeed *g_active;  // (__atomic: read from any thread)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void rando_seed_activate(const char *dir) {
    pthread_mutex_lock(&g_lock);
    // (the previous record stays allocated: readers may still hold its strings - seed.h)
    __atomic_store_n(&g_active, dir ? rando_seed_load(dir) : NULL, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lock);
}

const RandoSeed *rando_seed(void) { return __atomic_load_n(&g_active, __ATOMIC_ACQUIRE); }
bool rando_setting(const char *key) { return rando_seed_bool(rando_seed(), key); }
int64_t rando_setting_int(const char *key, int64_t dflt) { return rando_seed_int(rando_seed(), key, dflt); }
const char *rando_setting_str(const char *key) { return rando_seed_str(rando_seed(), key); }
