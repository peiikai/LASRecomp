// The active seed's game files, built in memory when the seed is loaded (runtime/rando/apply/, docs/randomizer.md):
// runtime/services/fs.c serves them in place of the game's, and adds the ones the game doesn't have.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// how the build reads the game's own files: a file by its romfs path ("region_common/event/Tarin.bfevfl"; malloc'd
// bytes, NULL if none) and a folder's file names (each malloc'd, in an malloc'd array; NULL if no such folder)
typedef uint8_t *(*RandoGameRead)(void *ctx, const char *path, size_t *size);
typedef char **(*RandoGameList)(void *ctx, const char *dir, size_t *count);

// Build the seed from its decisions (seed.json "decisions"). Replaces what an earlier build made. false: the seed
// can't be built here (logged: no decisions, a step this build doesn't have yet, a bad file) - its files then come
// from its folder, as before. `read` (with ctx) must keep working until rando_build_clear or the next build: files
// built when they're read (rando_built_deferred) read the game's files again then.
bool rando_build_seed(const char *decisions_json, size_t len, RandoGameRead read, RandoGameList list, void *ctx);
void rando_build_clear(void);

// the built file at a romfs path (NULL: not one the seed changes, or one built when read); the bytes stay valid
// until the next build / clear
const uint8_t *rando_built_file(const char *path, size_t *size);
// a file built when it's read rather than kept (room shuffle's level texture banks): true, and its size
bool rando_built_deferred(const char *path, uint64_t *size);
bool rando_built_has(const char *path);  // (either kind)
// copies bytes [off, off + len) of a built file of either kind to dst: the number copied (0 past its end, or for a
// path the seed doesn't build). Any thread; a deferred file is built on its first read and kept until another is.
size_t rando_built_read(const char *path, uint64_t off, uint8_t *dst, size_t len);
size_t rando_built_count(void);
const char *rando_built_path(size_t i);  // (sorted; both kinds)

#ifdef __cplusplus
}
#endif
