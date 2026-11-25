// The seed build's texture steps (textures.cpp): chest looks' recoloured chests and the title logo.
#pragma once
#include <stddef.h>

#include <vector>

#include "../fmt/texture.h"
#include "seed_build.h"

namespace rando {

// LAS-Randomizer's title banner, run-length encoded (logo_data.cpp, tools/rando/export_logo.py)
extern const int LOGO_W, LOGO_H;
extern const size_t LOGO_RLE_SIZE;
extern const unsigned char LOGO_RLE[];

namespace textures {
// chest_looks.recolour: the chest texture (RGB) in a look's colours ("key" / "plain")
tex::Image chest_recolour(const tex::Image &src, const char *look);
// the levels the build compresses: quicktex's mip chain of an image (RGB levels as RGBX)
std::vector<tex::Image> mip_chain(const tex::Image &im);
tex::Image logo_banner();  // (LA)
}  // namespace textures

std::vector<BuildStep> steps_textures();  // chest_looks.write_models, mod_generator.makeGeneralARCChanges

}  // namespace rando
