// The world steps' tables (las_world.cpp): LAS-Randomizer's code patches already assembled, and the game's level
// folders. las_world_data.cpp was generated from the Python (the patches assembled by LAS's own assembler); it is edited
// directly now.
#pragma once
#include <stdint.h>

#include <utility>
#include <vector>

namespace rando {
namespace las {
namespace world {

// one patch of ASM/Patches/*.asm, as makeExefsPatches writes it into a .pchtxt (the files and patches in its order)
struct AsmPatch {
    const char *file;     // (the .asm it is from)
    const char *setting;  // the setting it needs (";settings name" / "!name"), nullptr: always
    bool state;           // (true: the setting on, false: "!name")
    const char *comment;  // its titles ("// ..." lines), "" for none
    uint32_t address;
    int kind;             // 0 code (text: the assembled bytes in hex), 1 a string (text as written), 2 a short (hex)
    const char *text;     // (nullptr: by the data pick)
    const char *data;     // the randomizer data it takes (";data CHEST_ENEMY"), nullptr: none
    const std::vector<std::pair<int, const char *>> *variants;  // data value -> text
};
extern const std::vector<AsmPatch> ASM_PATCHES;

// region_common/level's folders, sorted (os.listdir without the .ldb file)
extern const std::vector<const char *> LEVEL_FOLDERS;

}  // namespace world
}  // namespace las
}  // namespace rando
