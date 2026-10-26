// Zip archives over miniz (runtime/zipfile.h).
#include "zipfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir(p, m) _mkdir(p)
#else
#include <unistd.h>
#endif

#include "miniz.h"  // build/miniz (tools/fetch_miniz.sh)

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

struct ZipReader { mz_zip_archive z; mz_zip_archive_file_stat st; };  // (st: ZipEntry.name points into it)
struct ZipWriter { mz_zip_archive z; bool failed; };

static void zip_why(mz_zip_archive *z, const char *what, char *why, size_t n) {
    if (why && n) snprintf(why, n, "%s: %s", what, mz_zip_get_error_string(mz_zip_get_last_error(z)));
}

// ------------------------------------------------------------------ reading
ZipReader *zip_open(const char *path, char *why, size_t why_size) {
    ZipReader *r = calloc(1, sizeof *r);
    if (!mz_zip_reader_init_file_v2(&r->z, path, 0, 0, 0)) {
        zip_why(&r->z, path, why, why_size);
        free(r);
        return NULL;
    }
    return r;
}

void zip_close(ZipReader *r) {
    if (!r) return;
    mz_zip_reader_end(&r->z);
    free(r);
}

int zip_count(const ZipReader *r) { return (int)mz_zip_reader_get_num_files((mz_zip_archive *)&r->z); }

bool zip_entry_at(ZipReader *r, int i, ZipEntry *e) {
    mz_zip_archive_file_stat *st = &r->st;
    if (!mz_zip_reader_file_stat(&r->z, (mz_uint)i, st)) return false;
    e->name = st->m_filename;
    e->size = st->m_uncomp_size;
    e->mtime = st->m_time;
    // the Unix mode in the high half when the archive was made by Unix (Info-ZIP / zip / our writer)
    uint32_t made_by = st->m_version_made_by >> 8;
    e->mode = made_by == 3 || made_by == 19 ? st->m_external_attr >> 16 : 0;
    e->is_dir = st->m_is_directory || (e->mode & 0170000) == 040000;
    e->is_symlink = (e->mode & 0170000) == 0120000;
    return true;
}

int zip_find(ZipReader *r, const char *name) { return mz_zip_reader_locate_file(&r->z, name, NULL, 0); }

void *zip_read(ZipReader *r, int i, size_t *size) {
    size_t n = 0;
    void *p = mz_zip_reader_extract_to_heap(&r->z, (mz_uint)i, &n, 0);
    if (!p) return NULL;
    void *q = realloc(p, n + 1);  // (NUL-terminated for text)
    if (!q) { mz_free(p); return NULL; }
    ((char *)q)[n] = 0;
    if (size) *size = n;
    return q;
}

static void mkdirs(const char *path) {
    char p[2048];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') { char s = *c; *c = 0; mkdir(p, 0755); *c = s; }
    mkdir(p, 0755);
}

bool zip_extract(ZipReader *r, const char *prefix, const char *dir, char *why, size_t why_size, bool (*cancelled)(void)) {
    size_t plen = prefix ? strlen(prefix) : 0;
    int n = zip_count(r);
    for (int i = 0; i < n; i++) {
        if (cancelled && cancelled()) { snprintf(why, why_size, "cancelled"); return false; }
        ZipEntry e;
        if (!zip_entry_at(r, i, &e)) { zip_why(&r->z, "entry", why, why_size); return false; }
        if (plen && (strncmp(e.name, prefix, plen) != 0 || (e.name[plen] != '/' && e.name[plen] != 0))) continue;
        const char *rel = e.name + plen;
        while (*rel == '/') rel++;
        if (!*rel) continue;
        if (strstr(rel, "..")) { snprintf(why, why_size, "%s: unsafe name", e.name); return false; }
        char out[2048];
        snprintf(out, sizeof out, "%s" SEP "%s", dir, rel);
        for (char *c = out + strlen(dir) + 1; *c; c++) if (*c == '/') *c = SEP[0];
        if (e.is_dir) { mkdirs(out); continue; }
        char *slash = strrchr(out, SEP[0]);
        if (slash) { *slash = 0; mkdirs(out); *slash = SEP[0]; }
        if (e.is_symlink) {
            size_t tlen;
            char *target = zip_read(r, i, &tlen);
            if (!target) { zip_why(&r->z, e.name, why, why_size); return false; }
#ifdef _WIN32
            // (no symlinks for an unprivileged Windows user: a copy of the target, when it's in the archive)
            char tname[2048];
            snprintf(tname, sizeof tname, "%.*s/%s", (int)(strrchr(e.name, '/') ? strrchr(e.name, '/') - e.name : 0), e.name, target);
            int ti = zip_find(r, tname);
            free(target);
            if (ti < 0 || !mz_zip_reader_extract_to_file(&r->z, (mz_uint)ti, out, 0)) {
                snprintf(why, why_size, "%s: a link whose target isn't in the archive", e.name);
                return false;
            }
#else
            remove(out);
            int rc = symlink(target, out);
            free(target);
            if (rc != 0) { snprintf(why, why_size, "%s: can't make the link", out); return false; }
#endif
            continue;
        }
        if (!mz_zip_reader_extract_to_file(&r->z, (mz_uint)i, out, 0)) { zip_why(&r->z, e.name, why, why_size); return false; }
#ifndef _WIN32
        if (e.mode & 0111) chmod(out, e.mode & 0777);
#endif
    }
    return true;
}

// SHA-256 of the file (runtime/cache_sha256.h is C++; a small C one here)
static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void sha256_block(uint32_t h[8], const uint8_t b[64]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

bool zip_file_id(const char *path, char out[17]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    static uint8_t buf[1 << 16];
    uint8_t tail[128];
    size_t used = 0, got;
    uint64_t total = 0;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        total += got;
        size_t off = 0;
        if (used) {  // (a partial block from before)
            size_t take = 64 - used < got ? 64 - used : got;
            memcpy(tail + used, buf, take); used += take; off = take;
            if (used == 64) { sha256_block(h, tail); used = 0; }
        }
        for (; off + 64 <= got; off += 64) sha256_block(h, buf + off);
        if (off < got) { memcpy(tail, buf + off, got - off); used = got - off; }
    }
    fclose(f);
    tail[used++] = 0x80;
    if (used > 56) { memset(tail + used, 0, 64 - used); sha256_block(h, tail); used = 0; }
    memset(tail + used, 0, 56 - used);
    uint64_t bits = total * 8;
    for (int i = 0; i < 8; i++) tail[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_block(h, tail);
    snprintf(out, 17, "%08x%08x", h[0], h[1]);
    return true;
}

// ------------------------------------------------------------------ writing
ZipWriter *zip_create(const char *path, char *why, size_t why_size) {
    ZipWriter *w = calloc(1, sizeof *w);
    if (!mz_zip_writer_init_file_v2(&w->z, path, 0, MZ_ZIP_FLAG_WRITE_ZIP64)) {
        zip_why(&w->z, path, why, why_size);
        free(w);
        return NULL;
    }
    return w;
}

bool zip_add_file(ZipWriter *w, const char *name, const char *path, bool compress) {
    if (w->failed) return false;
    if (!mz_zip_writer_add_file(&w->z, name, path, NULL, 0, compress ? MZ_DEFAULT_LEVEL : MZ_NO_COMPRESSION)) w->failed = true;
    return !w->failed;
}

bool zip_add_from(ZipWriter *w, ZipReader *src, int i) {
    if (w->failed) return false;
    if (!mz_zip_writer_add_from_zip_reader(&w->z, &src->z, (mz_uint)i)) w->failed = true;
    return !w->failed;
}

bool zip_finish(ZipWriter *w) {
    bool ok = !w->failed;
    if (ok && !mz_zip_writer_finalize_archive(&w->z)) ok = false;
    mz_zip_writer_end(&w->z);
    free(w);
    return ok;
}
