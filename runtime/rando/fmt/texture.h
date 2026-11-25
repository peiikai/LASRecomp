// Texture edits for the seed build (docs/randomizer.md, stage 4): the recoloured chests (chest looks) and the title
// logo. Images are decoded from the player's own texture banks, edited with the same image operations the Python
// generator used (Pillow's: colour conversion, point tables, blur, paste, bilinear resize - written for LAS Recomp
// from how they behave, no code copied) and compressed again with rgbcx (third_party/rgbcx, MIT).
//
// The Python generator compressed with quicktex, which can't be ported (LGPL): the compressed bytes differ from its
// files, the decoded pixels are as close or closer (tools/rando/test_apply.py compares these files by their pixels).
#pragma once
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace rando {
struct BntxTexture;

namespace tex {

struct Image {
    int w = 0, h = 0, c = 0;  // c: 1 (L), 2 (LA), 3 (RGB), 4 (RGBA / RGBX)
    std::vector<uint8_t> px;  // rows top to bottom, c bytes per pixel
    Image() = default;
    Image(int w_, int h_, int c_, uint8_t fill = 0) : w(w_), h(h_), c(c_), px((size_t)w_ * h_ * c_, fill) {}
    uint8_t *at(int x, int y) { return &px[((size_t)y * w + x) * c]; }
    const uint8_t *at(int x, int y) const { return &px[((size_t)y * w + x) * c]; }
};

// ---- Pillow's operations (each named after the call it stands for)
Image rgb_to_hsv(const Image &rgb);                          // im.convert('HSV') (RGB in, 3 bands out)
Image hsv_to_rgb(const Image &hsv);                          // Image.merge('HSV', ...).convert('RGB')
Image band(const Image &im, int b);                          // im.split()[b] / getchannel
Image merge(const std::vector<const Image *> &bands);        // Image.merge
Image point(const Image &l, const uint8_t lut[256]);         // l.point(table)
Image multiply(const Image &a, const Image &b);              // ImageChops.multiply (L)
Image lighter(const Image &a, const Image &b);               // ImageChops.lighter (L)
Image gaussian_blur(const Image &l, float radius);           // l.filter(ImageFilter.GaussianBlur(radius))
Image composite(const Image &a, const Image &b, const Image &mask);  // Image.composite(a, b, mask): mask L
void paste_masked(Image &dst, const Image &src, int x, int y);  // dst.paste(src, (x, y), src): src LA / RGBA
Image resize_bilinear(const Image &im, int w, int h);        // im.resize((w, h), Image.BILINEAR), any band count
Image convert(const Image &im, int c);                       // L / LA / RGB -> RGB / RGBA (gray copied, alpha 255)

// quicktex's mip chain: halve (rounding down, never below 1) until 1x1, each level resized from the full image
std::vector<std::pair<int, int>> mip_sizes(int w, int h);

// ---- BNTX texture data
// decode level 0 of a BC1 (format 0x1a..) texture to RGB / of an R8G8 (0x09..) one to LA, as Pillow reads the DDS the
// LAS BNTX editor exports
Image decode_level0(const BntxTexture &t);
// every level of a BC1 / BC3 / R8G8 texture (BC1 / BC3 as RGBA, R8G8 as LA)
std::vector<Image> decode_levels(const BntxTexture &t);
// compress a mip chain to BC1 (RGB(X) levels, rgbcx level 10, 4-colour blocks) or BC3 (RGBA, level 18) and lay each
// level out block-linear as the texture's layout puts it (its block height, the levels one after another)
std::string encode_bc1(const std::vector<Image> &levels, uint32_t block_height_log2);
std::string encode_bc3(const std::vector<Image> &levels, uint32_t block_height_log2);
// level offsets of that layout (BntxTexture::mip_offsets) and its total size
std::vector<int64_t> level_offsets(int w, int h, int levels, int bytes_per_block, uint32_t block_height_log2,
                                   int64_t *total);

}  // namespace tex
}  // namespace rando
