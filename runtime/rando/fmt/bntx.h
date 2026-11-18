// Texture banks (region_common/map/<Level>.bntx, timg/__Combined.bntx inside ui/*.arc): BNTX, little endian. A
// 0x20 header, the texture container (0x38) and a 0x140-byte memory pool, one 64-bit pointer per texture to its
// info block, the string table (_STR: count, then per string a 16-bit length, the bytes, a NUL, padded to 2), the
// name dictionary (_DIC: a Patricia trie over the texture names, entry 0 the root), one info block per texture
// (BRTI: the texture's description, 0x200 bytes of GPU descriptor space, then a pointer per mip level), the image
// data (BRTD, each texture aligned to its own alignment) and the relocation table (_RLT).
//
// Written for LAS Recomp from the format (no code copied). Bntx::save lays a bank out exactly as the BNTX editor
// vendored in LAS-Randomizer (Tools/bntx_editor, opened then saved) does, since the Python generator writes banks
// with it:
//   - strings first, then the dictionary (aligned to 8), straight after the texture pointers at 0x198;
//   - every info block is 0x2A0 + 8 * mips bytes; the last one also covers the padding up to the data block;
//   - the data block (its header included) ends on 1 << alignment shift (0xC for "NX  ", 3 otherwise), its contents
//     start right after the header at that alignment, each texture's data at its own alignment; the relocation
//     table follows, at that alignment too;
//   - header fields are rewritten: version 0x40100, address size 0x40, flag 0; texture layout keeps only its block
//     height (the low 3 bits); user data and user dictionaries are dropped (pointers written as 0); the pads in the
//     header and the info block are written as 0;
//   - the relocation table has two sections (everything before the data, then the data), each entry list built
//     the same way, in the same order (see save()).
// Names are kept as their UTF-8 bytes. Only little-endian banks are read (all the game's are).
#pragma once
#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rando {

struct BntxTexture {  // one BRTI block
    std::string name;
    int name_idx = 0;  // (its entry in Bntx::strings)
    uint8_t flags = 0, dim = 0;
    uint16_t tile_mode = 0, swizzle = 0, num_mips = 0, num_samples = 0;
    uint32_t format = 0, access_flags = 0;
    int32_t width = 0, height = 0, depth = 0;
    uint32_t array_length = 0, texture_layout = 0, texture_layout2 = 0, image_size = 0, alignment = 0, comp_sel = 0;
    uint8_t img_dim = 0;
    std::vector<int64_t> mip_offsets;           // (from the first mip's start; one entry even for 0 mips)
    std::shared_ptr<const std::string> data;    // image_size bytes (fewer if the file ended first), shared by copies
    // where parse found it (patch_in_place edits the original bytes; save recomputes all of these)
    int64_t ptrs_addr = 0, user_data_addr = 0, user_dict_addr = 0;
};

struct BntxDictEntry {
    int64_t ref_bit = 0;  // (written as 32 bits)
    uint16_t left = 0, right = 0;
    int str_idx = -1;     // (-1: the root, pointing at the empty string at the table's start)
};

struct Bntx {
    std::string magic = std::string("BNTX\0\0\0\0", 8);
    std::string target = "NX  ";       // (platform: "NX  " or "Gen ")
    std::vector<std::string> strings;  // the string table, in order
    std::vector<int64_t> string_pos;   // (where parse found each: its length field)
    int name_idx = 0;                  // the bank's own name, in strings
    std::string dict_magic = "_DIC";
    std::vector<BntxDictEntry> dict;   // root + one per texture
    int64_t dict_pos = 0;              // (where parse found it)
    std::vector<BntxTexture> textures;

    static Bntx parse(const std::string &data);  // throws on bad data
    std::string save() const;                    // throws on values the layout can't hold

    const std::string &name() const { return strings.at(name_idx); }
    void rebuild_dict();  // the dictionary for the current texture list, in texture order (the root kept)
};

// nn::util ResDic: (bit, left, right) for entries 0..n (entry i + 1 is keys[i]). Ported from tools/rando/resdic.py.
std::vector<std::pair<int64_t, std::pair<uint16_t, uint16_t>>> resdic_build(const std::vector<std::string> &keys);

// tools/rando/bntx_merge.py, byte for byte:
//   merge: base plus every texture of the others whose name it lacks (only those in *only, if given)
//   subset: exactly the textures named in only, each from the first source that has it
//   copy_renamed: each (source, new name) remade as a copy under the new name (one with that name replaced)
//   patch_in_place: renames (same length) and pixel copies between same-shaped textures, without re-saving
std::string bntx_merge(const std::string &base, const std::vector<const std::string *> &others,
                       const std::unordered_set<std::string> *only = nullptr);
std::string bntx_subset(const std::vector<const std::string *> &sources, const std::unordered_set<std::string> &only);
std::string bntx_copy_renamed(const std::string &base, const std::vector<std::pair<std::string, std::string>> &pairs);
std::string bntx_patch_in_place(const std::string &base,
                                const std::vector<std::pair<std::string, std::string>> &set_from,  // (texture, source)
                                const std::vector<std::pair<std::string, std::string>> &rename = {});  // (old, new)

}  // namespace rando
