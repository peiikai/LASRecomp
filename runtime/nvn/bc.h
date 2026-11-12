// BC1-BC7 decoder, for Vulkan drivers that don't list a BC format (vulkan/textures.c). Samsung's Xclipse 940 driver
// (25.3.3) lists BC1-BC3 but not BC4 / BC5, which the game's button glyphs and menu frames use, nor BC7, which house
// walls use (#693).
#pragma once
#include <stdint.h>

enum { BC_NONE, BC1_RGB, BC1_RGBA, BC2, BC3, BC4_UNORM, BC4_SNORM, BC5_UNORM, BC5_SNORM, BC6H_UFLOAT, BC6H_SFLOAT, BC7 };

// blocks: row-major blocks (8 bytes for BC1 / BC4, 16 for the others) covering ceil(width/4) x ceil(height/4).
// rgba_out: width*height texels of bc_texel_bytes, tightly packed rows. BC4 / BC5 decode as their native views read
// them: R (BC4), R G (BC5), the other channels 0 and alpha 1. The SNORM kinds write signed bytes (R8G8B8A8_SNORM:
// alpha 127). BC6H writes half floats (R16G16B16A16_SFLOAT, alpha 1.0); the others RGBA8.
void bc_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *rgba_out);
static inline uint32_t bc_block_bytes(int kind) { return kind == BC1_RGB || kind == BC1_RGBA || kind == BC4_UNORM || kind == BC4_SNORM ? 8 : 16; }
static inline uint32_t bc_texel_bytes(int kind) { return kind == BC6H_UFLOAT || kind == BC6H_SFLOAT ? 8 : 4; }
