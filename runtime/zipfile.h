// Zip archives (miniz, tools/fetch_miniz.sh): the iOS kit the player downloads (.laskit: the app, the linkers, the headers) is
// read here, the toolchain of this host taken out of it, and the .ipa written from its Payload/ entries (cloned as
// they are) plus the game code library. Entries keep their Unix mode in the external attributes (Info-ZIP's
// convention): the linker comes out executable on every host, which a folder on Windows can't give.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZipReader ZipReader;
typedef struct ZipWriter ZipWriter;

// ---- reading
ZipReader *zip_open(const char *path, char *why, size_t why_size);
void zip_close(ZipReader *z);
const char *zip_path(const ZipReader *z);  // the file it was opened from
int zip_count(const ZipReader *z);
// An entry by index (name as stored, Unix mode from the external attributes - 0 when the archive doesn't say).
typedef struct { const char *name; uint64_t size; uint32_t mode; bool is_dir, is_symlink; time_t mtime; } ZipEntry;
bool zip_entry_at(ZipReader *z, int i, ZipEntry *e);
int zip_find(ZipReader *z, const char *name);  // -1 when absent
// The entry's bytes, malloc'ed and NUL-terminated (text), *size without the NUL. NULL on failure.
void *zip_read(ZipReader *z, int i, size_t *size);
// Every entry under prefix/ (or all, prefix NULL) into dir, with modes; false (why) on the first failure. A symlink
// entry is refused.
bool zip_extract(ZipReader *z, const char *prefix, const char *dir, char *why, size_t why_size,
                 bool (*cancelled)(void));

// ---- writing (zip64 only when an entry or the archive needs it). Entries cloned from another archive keep everything,
// their Unix mode included; a file added here gets no mode at all (miniz can't write one; the library doesn't need one).
ZipWriter *zip_create(const char *path, char *why, size_t why_size);
bool zip_add_file(ZipWriter *w, const char *name, const char *path, bool compress);
// Entry i of src copied as it is: its name, data (no recompression) and central-directory record.
bool zip_add_from(ZipWriter *w, ZipReader *src, int i);
// Finish and close; false when anything failed to write (the file is left for the caller to remove).
bool zip_finish(ZipWriter *w);

#ifdef __cplusplus
}
#endif
