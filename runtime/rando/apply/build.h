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
// from its folder, as before.
bool rando_build_seed(const char *decisions_json, size_t len, RandoGameRead read, RandoGameList list, void *ctx);
void rando_build_clear(void);

// the built file at a romfs path (NULL: not one the seed changes); the bytes stay valid until the next build / clear
const uint8_t *rando_built_file(const char *path, size_t *size);
size_t rando_built_count(void);
const char *rando_built_path(size_t i);  // (sorted)

#ifdef __cplusplus
}
#endif
