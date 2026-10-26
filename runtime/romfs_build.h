// The game's RomFS image, laid out from a folder of game files (runtime/romfs_build.c). The game reads its data through
// one raw RomFS image; runtime/services/fs.c serves that image from this layout - the header and tables in memory, each
// file's data straight from its file in the player's romfs/ folder - so no second copy of the game data is kept.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROMFS_DATA_START 0x200u
typedef struct { uint64_t start, size; char *path; } RomfsLayoutFile;  // start: offset in the image
typedef struct {
    uint8_t header[ROMFS_DATA_START];  // the image's first bytes (the 0x50-byte header, then zeros)
    uint64_t tables_off, tables_size;  // dir hash, dir entries, file hash and file entries, one blob
    uint8_t *tables;
    RomfsLayoutFile *files;  // sorted by start; the bytes between files read as zeros
    int nfiles;
    uint64_t size;  // the whole image
} RomfsLayout;

// Lays out the RomFS image of the folder `dir`. false = failed (why: a short message).
bool romfs_layout(const char *dir, RomfsLayout *out, char *why, size_t why_size);
void romfs_layout_free(RomfsLayout *l);

// Bytes of file data under dir (the image is about this plus its tables).
uint64_t romfs_dir_bytes(const char *dir);

// The game's files in the player's folder: exefs is the folder holding main and main.npdm, romfs the one holding
// region_common (the game's data root) - the picked folder itself, its exefs/ and romfs/, or a few levels down (some
// layouts nest them; the two may be one and the same folder). Either comes back "" when not found. unusable (optional)
// is set when game packages (.xci / .nsp / .nca...) were seen instead: the player has the game, not its files.
void romfs_find_game(const char *dir, char *exefs, size_t exefs_size, char *romfs, size_t romfs_size, bool *unusable);

// Copies the folder tree src to dst (hard links where the file system allows: no second copy of 6 GB). progress as
// above. false = failed.
bool romfs_copy_tree(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total));
// One file the same way (an existing dst is replaced).
bool romfs_copy_file(const char *src, const char *dst);

#ifdef __cplusplus
}
#endif
