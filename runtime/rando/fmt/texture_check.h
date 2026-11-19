// lasrando texture-check: a game file whose textures the seed build compresses itself (the recoloured chests, the
// title logo: texture.h), against the same file made by the Python generator. Everything but the textures' pixel
// data must be byte-identical (the archive, the bank, every texture's description); the pixels are decoded level by
// level and must be within TEXTURE_RMS_LIMIT (root mean square, 0-255 per channel) of the Python's.
#pragma once
#include <string>

namespace rando {

extern const double TEXTURE_RMS_LIMIT;
// prints one line per texture compared; 0: within the limit, 1: not (or the files differ elsewhere)
int texture_check(const std::string &want_path, const std::string &got_path, const std::string &rel);

}  // namespace rando
