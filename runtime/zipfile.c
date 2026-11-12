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

struct ZipReader { mz_zip_archive z; mz_zip_archive_file_stat st; char *path; };  // (st: ZipEntry.name points into it)
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
    r->path = strdup(path);
    return r;
}

const char *zip_path(const ZipReader *r) { return r->path; }

void zip_close(ZipReader *r) {
    if (!r) return;
    mz_zip_reader_end(&r->z);
    free(r->path);
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
        // (a link's target isn't checked like a name: "x -> /elsewhere", then "x/file", would write outside dir)
        if (e.is_symlink) { snprintf(why, why_size, "%s: a link (none is expected)", e.name); return false; }
        if (e.is_dir) { mkdirs(out); continue; }
        char *slash = strrchr(out, SEP[0]);
        if (slash) { *slash = 0; mkdirs(out); *slash = SEP[0]; }
        if (!mz_zip_reader_extract_to_file(&r->z, (mz_uint)i, out, 0)) { zip_why(&r->z, e.name, why, why_size); return false; }
#ifndef _WIN32
        if (e.mode & 0111) chmod(out, e.mode & 0777);
#endif
    }
    return true;
}

// ------------------------------------------------------------------ writing
ZipWriter *zip_create(const char *path, char *why, size_t why_size) {
    ZipWriter *w = calloc(1, sizeof *w);
    if (!mz_zip_writer_init_file_v2(&w->z, path, 0, 0)) {
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
