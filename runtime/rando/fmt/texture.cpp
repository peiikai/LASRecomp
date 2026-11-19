// Texture edits for the seed build (texture.h).
#include "texture.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>

#include "bntx.h"

extern "C" {
#include "../../nvn/tegra.h"
}

#define RGBCX_IMPLEMENTATION
#include "../../../third_party/rgbcx/rgbcx.h"

namespace rando {
namespace tex {

static inline uint8_t clip8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// ---- colour conversion (Pillow's RGB <-> HSV: floats, hue and saturation scaled to 0-255 and truncated)
Image rgb_to_hsv(const Image &rgb) {
    Image out(rgb.w, rgb.h, 3);
    for (size_t i = 0, n = (size_t)rgb.w * rgb.h; i < n; i++) {
        const uint8_t *p = &rgb.px[i * rgb.c];
        int r = p[0], g = p[1], b = p[2];
        int maxc = std::max(r, std::max(g, b)), minc = std::min(r, std::min(g, b));
        uint8_t h = 0, s = 0;
        if (minc != maxc) {
            float cr = (float)(maxc - minc);
            float ss = cr / maxc;
            float rc = (float)(maxc - r) / cr, gc = (float)(maxc - g) / cr, bc = (float)(maxc - b) / cr;
            float hh;
            if (r == maxc) hh = bc - gc;
            else if (g == maxc) hh = 2.0f + rc - bc;
            else hh = 4.0f + gc - rc;
            hh = (float)fmod(hh / 6.0 + 1.0, 1.0);
            h = clip8((int)(hh * 255.0));
            s = clip8((int)(ss * 255.0));
        }
        out.px[i * 3] = h, out.px[i * 3 + 1] = s, out.px[i * 3 + 2] = (uint8_t)maxc;
    }
    return out;
}

Image hsv_to_rgb(const Image &hsv) {
    Image out(hsv.w, hsv.h, 3);
    for (size_t i = 0, n = (size_t)hsv.w * hsv.h; i < n; i++) {
        const uint8_t *p = &hsv.px[i * 3];
        int h = p[0], s = p[1], v = p[2], r, g, b;
        if (s == 0) {
            r = g = b = v;
        } else {
            int k = (int)floor((float)h * 6.0 / 255.0);
            float f = (float)((float)h * 6.0 / 255.0 - (float)k);
            float fs = (float)(s / 255.0);
            int pp = clip8((int)round((float)v * (1.0 - fs)));
            int q = clip8((int)round((float)v * (1.0 - fs * f)));
            int t = clip8((int)round((float)v * (1.0 - fs * (1.0 - f))));
            switch (k % 6) {
            case 0: r = v, g = t, b = pp; break;
            case 1: r = q, g = v, b = pp; break;
            case 2: r = pp, g = v, b = t; break;
            case 3: r = pp, g = q, b = v; break;
            case 4: r = t, g = pp, b = v; break;
            default: r = v, g = pp, b = q; break;
            }
        }
        out.px[i * 3] = (uint8_t)r, out.px[i * 3 + 1] = (uint8_t)g, out.px[i * 3 + 2] = (uint8_t)b;
    }
    return out;
}

// ---- bands
Image band(const Image &im, int b) {
    Image out(im.w, im.h, 1);
    for (size_t i = 0, n = (size_t)im.w * im.h; i < n; i++) out.px[i] = im.px[i * im.c + b];
    return out;
}

Image merge(const std::vector<const Image *> &bands) {
    Image out(bands[0]->w, bands[0]->h, (int)bands.size());
    for (size_t i = 0, n = (size_t)out.w * out.h; i < n; i++)
        for (int b = 0; b < out.c; b++) out.px[i * out.c + b] = bands[b]->px[i];
    return out;
}

Image point(const Image &l, const uint8_t lut[256]) {
    Image out(l.w, l.h, l.c);
    for (size_t i = 0; i < l.px.size(); i++) out.px[i] = lut[l.px[i]];
    return out;
}

Image multiply(const Image &a, const Image &b) {
    Image out(a.w, a.h, a.c);
    for (size_t i = 0; i < a.px.size(); i++) out.px[i] = (uint8_t)((int)a.px[i] * (int)b.px[i] / 255);
    return out;
}

Image lighter(const Image &a, const Image &b) {
    Image out(a.w, a.h, a.c);
    for (size_t i = 0; i < a.px.size(); i++) out.px[i] = std::max(a.px[i], b.px[i]);
    return out;
}

// ---- Gaussian blur: Pillow approximates it with 3 box blurs of a fractional radius (each pass rounded to 8 bits;
// 3 passes along rows, then 3 along columns)
static void box_line(uint8_t *out, const uint8_t *in, int lastx, int radius, int edge_a, int edge_b, uint32_t ww,
                     uint32_t fw) {
    uint32_t acc = in[0] * (radius + 1), bulk;
    for (int x = 0; x < edge_a - 1; x++) acc += in[x];
    acc += in[lastx] * (radius - edge_a + 1);
    auto save = [&](int x) { out[x] = (uint8_t)((bulk + (1 << 23)) >> 24); };
    if (edge_a <= edge_b) {
        for (int x = 0; x < edge_a; x++) {
            acc += in[x + radius] - in[0];
            bulk = acc * ww + (in[0] + in[x + radius + 1]) * fw;
            save(x);
        }
        for (int x = edge_a; x < edge_b; x++) {
            acc += in[x + radius] - in[x - radius - 1];
            bulk = acc * ww + (in[x - radius - 1] + in[x + radius + 1]) * fw;
            save(x);
        }
        for (int x = edge_b; x <= lastx; x++) {
            acc += in[lastx] - in[x - radius - 1];
            bulk = acc * ww + (in[x - radius - 1] + in[lastx]) * fw;
            save(x);
        }
    } else {
        for (int x = 0; x < edge_b; x++) {
            acc += in[x + radius] - in[0];
            bulk = acc * ww + (in[0] + in[x + radius + 1]) * fw;
            save(x);
        }
        for (int x = edge_b; x < edge_a; x++) {
            acc += in[lastx] - in[0];
            bulk = acc * ww + (in[0] + in[lastx]) * fw;
            save(x);
        }
        for (int x = edge_a; x <= lastx; x++) {
            acc += in[lastx] - in[x - radius - 1];
            bulk = acc * ww + (in[x - radius - 1] + in[lastx]) * fw;
            save(x);
        }
    }
}

static void box_rows(Image &im, float fradius) {  // (L images)
    int radius = (int)fradius;
    uint32_t ww = (uint32_t)((float)(1 << 24) / (fradius * 2 + 1));
    uint32_t fw = ((1 << 24) - (radius * 2 + 1) * ww) / 2;
    int edge_a = std::min(radius + 1, im.w), edge_b = std::max(im.w - radius - 1, 0);
    std::vector<uint8_t> line(im.w);
    for (int y = 0; y < im.h; y++) {
        memcpy(line.data(), im.at(0, y), im.w);
        box_line(im.at(0, y), line.data(), im.w - 1, radius, edge_a, edge_b, ww, fw);
    }
}

static Image transpose(const Image &im) {
    Image out(im.h, im.w, im.c);
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) memcpy(out.at(y, x), im.at(x, y), im.c);
    return out;
}

Image gaussian_blur(const Image &l, float radius) {
    if (l.c != 1) throw std::runtime_error("gaussian_blur: L images only");
    const int passes = 3;
    float sigma2 = radius * radius / passes;
    float big_l = (float)sqrt(12.0 * sigma2 + 1.0);
    float il = (float)floor((big_l - 1.0) / 2.0);
    float a = (2 * il + 1) * (il * (il + 1) - 3 * sigma2);
    a /= 6 * (sigma2 - (il + 1) * (il + 1));
    float r = il + a;
    Image out = l;
    if (r == 0) return out;
    for (int i = 0; i < passes; i++) box_rows(out, r);
    Image t = transpose(out);
    for (int i = 0; i < passes; i++) box_rows(t, r);
    return transpose(t);
}

// ---- paste with a mask: out = (dst * (255 - m) + src * m) / 255, rounded the way Pillow does
static inline uint8_t blend(int m, int dst, int src) {
    uint32_t t = (uint32_t)(dst * (255 - m) + src * m) + 128;
    return (uint8_t)(((t >> 8) + t) >> 8);
}

Image composite(const Image &a, const Image &b, const Image &mask) {
    Image out = b;
    for (size_t i = 0, n = (size_t)b.w * b.h; i < n; i++)
        for (int k = 0; k < b.c; k++) out.px[i * b.c + k] = blend(mask.px[i], b.px[i * b.c + k], a.px[i * a.c + k]);
    return out;
}

void paste_masked(Image &dst, const Image &src, int x0, int y0) {
    if (src.c != 2 && src.c != 4) throw std::runtime_error("paste_masked: LA / RGBA sources only");
    if (dst.c != src.c) throw std::runtime_error("paste_masked: same modes only");
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < src.w; x++) {
            int dx = x + x0, dy = y + y0;
            if (dx < 0 || dy < 0 || dx >= dst.w || dy >= dst.h) continue;
            const uint8_t *s = src.at(x, y);
            uint8_t *d = dst.at(dx, dy);
            int m = s[src.c - 1];
            for (int k = 0; k < src.c; k++) d[k] = blend(m, d[k], s[k]);
        }
}

Image convert(const Image &im, int c) {
    if (im.c == c) return im;
    Image out(im.w, im.h, c);
    for (size_t i = 0, n = (size_t)im.w * im.h; i < n; i++) {
        const uint8_t *p = &im.px[i * im.c];
        uint8_t *o = &out.px[i * c];
        uint8_t r = p[0], g = im.c >= 3 ? p[1] : p[0], b = im.c >= 3 ? p[2] : p[0];
        uint8_t a = im.c == 2 ? p[1] : im.c == 4 ? p[3] : 255;
        if (c == 1) throw std::runtime_error("convert: to L not needed");
        if (c == 2) o[0] = r, o[1] = a;
        else {
            o[0] = r, o[1] = g, o[2] = b;
            if (c == 4) o[3] = a;
        }
    }
    return out;
}

// ---- bilinear resize (Pillow's: a triangle filter widened by the scale when shrinking, 22-bit fixed point weights,
// rows first, then columns; each pass rounded to 8 bits)
static const int PRECISION_BITS = 32 - 8 - 2;

static int coeffs(int in_size, int out_size, std::vector<int> &bounds, std::vector<int> &kk) {
    double scale = (double)in_size / out_size, filterscale = scale < 1.0 ? 1.0 : scale;
    double support = 1.0 * filterscale;
    int ksize = (int)ceil(support) * 2 + 1;
    std::vector<double> pre((size_t)out_size * ksize, 0.0);
    bounds.assign((size_t)out_size * 2, 0);
    for (int xx = 0; xx < out_size; xx++) {
        double center = (xx + 0.5) * scale, ww = 0.0, ss = 1.0 / filterscale;
        int xmin = (int)(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = (int)(center + support + 0.5);
        if (xmax > in_size) xmax = in_size;
        xmax -= xmin;
        double *k = &pre[(size_t)xx * ksize];
        for (int x = 0; x < xmax; x++) {
            double v = (x + xmin - center + 0.5) * ss;
            if (v < 0) v = -v;
            double w = v < 1.0 ? 1.0 - v : 0.0;
            k[x] = w;
            ww += w;
        }
        for (int x = 0; x < xmax; x++)
            if (ww != 0.0) k[x] /= ww;
        bounds[xx * 2] = xmin, bounds[xx * 2 + 1] = xmax;
    }
    kk.resize(pre.size());
    for (size_t i = 0; i < pre.size(); i++)
        kk[i] = pre[i] < 0 ? (int)(-0.5 + pre[i] * (1 << PRECISION_BITS)) : (int)(0.5 + pre[i] * (1 << PRECISION_BITS));
    return ksize;
}

static inline uint8_t clip_fixed(int v) {
    v >>= PRECISION_BITS;
    return clip8(v);
}

Image resize_bilinear(const Image &im, int w, int h) {
    if (w == im.w && h == im.h) return im;
    std::vector<int> bh, kh, bv, kv;
    int kx = coeffs(im.w, w, bh, kh), ky = coeffs(im.h, h, bv, kv);
    bool horiz = w != im.w, vert = h != im.h;
    int first = bv[0], last = bv[(size_t)h * 2 - 2] + bv[(size_t)h * 2 - 1];
    Image tmp;
    if (horiz) {
        tmp = Image(w, last - first, im.c);
        for (int y = 0; y < tmp.h; y++)
            for (int x = 0; x < w; x++) {
                int xmin = bh[x * 2], xmax = bh[x * 2 + 1];
                const int *k = &kh[(size_t)x * kx];
                for (int c = 0; c < im.c; c++) {
                    int ss = 1 << (PRECISION_BITS - 1);
                    for (int i = 0; i < xmax; i++) ss += im.at(xmin + i, y + first)[c] * k[i];
                    tmp.at(x, y)[c] = clip_fixed(ss);
                }
            }
        for (int y = 0; y < h; y++) bv[y * 2] -= first;
    } else {
        tmp = im;
    }
    if (!vert) return tmp;
    Image out(w, h, im.c);
    for (int y = 0; y < h; y++) {
        int ymin = bv[y * 2], ymax = bv[y * 2 + 1];
        const int *k = &kv[(size_t)y * ky];
        for (int x = 0; x < w; x++)
            for (int c = 0; c < im.c; c++) {
                int ss = 1 << (PRECISION_BITS - 1);
                for (int i = 0; i < ymax; i++) ss += tmp.at(x, ymin + i)[c] * k[i];
                out.at(x, y)[c] = clip_fixed(ss);
            }
    }
    return out;
}

std::vector<std::pair<int, int>> mip_sizes(int w, int h) {
    int count = (int)ceil(log2((double)std::max(w, h)) + 1);
    std::vector<std::pair<int, int>> out;
    for (int i = 0; i < count; i++) {
        out.push_back({w, h});
        if (w == 1 && h == 1) break;
        w = std::max(w / 2, 1), h = std::max(h / 2, 1);
    }
    return out;
}

// ---- block-linear layout
static TexLayout layout(int w, int h, int levels, const FormatInfo *f, uint32_t bh_log2) {
    TexLayout t;
    memset(&t, 0, sizeof t);
    t.width = (uint32_t)w, t.height = (uint32_t)h, t.depth = 1, t.layers = 1, t.levels = (uint32_t)levels, t.fmt = f;
    t.block_height_log2 = bh_log2;
    return t;
}

std::vector<int64_t> level_offsets(int w, int h, int levels, int bpb, uint32_t bh_log2, int64_t *total) {
    FormatInfo f = {(uint8_t)bpb, 4, 4, FMT_COMPRESSED, "bc"};
    TexLayout t = layout(w, h, levels, &f, bh_log2);
    std::vector<int64_t> out;
    for (int i = 0; i < levels; i++) out.push_back((int64_t)tegra_level(&t, (uint32_t)i).offset);
    if (total) *total = (int64_t)tegra_layer_size(&t);
    return out;
}

// ---- decode
static void rgb565(uint16_t c, int o[3]) {  // (Pillow's expansion: the top bits copied into the low ones)
    int r = (c & 0xf800) >> 8, g = (c & 0x07e0) >> 3, b = (c & 0x001f) << 3;
    o[0] = r | (r >> 5), o[1] = g | (g >> 6), o[2] = b | (b >> 5);
}

static void bc1_block(const uint8_t *s, uint8_t out[16][4]) {
    uint16_t c0 = (uint16_t)(s[0] | s[1] << 8), c1 = (uint16_t)(s[2] | s[3] << 8);
    int p[4][4];
    rgb565(c0, p[0]), rgb565(c1, p[1]);
    p[0][3] = p[1][3] = 255;
    if (c0 > c1) {
        for (int k = 0; k < 3; k++) p[2][k] = (2 * p[0][k] + p[1][k]) / 3, p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
        p[2][3] = p[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) p[2][k] = (p[0][k] + p[1][k]) / 2, p[3][k] = 0;
        p[2][3] = 255, p[3][3] = 0;
    }
    uint32_t sel = (uint32_t)(s[4] | s[5] << 8 | s[6] << 16 | (uint32_t)s[7] << 24);
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) out[i][k] = (uint8_t)p[(sel >> (2 * i)) & 3][k];
}

Image decode_level0(const BntxTexture &t) {
    uint32_t type = t.format >> 8;
    bool bc1 = type == 0x1a, rg8 = type == 0x09;
    if (!bc1 && !rg8) throw std::runtime_error("decode_level0: format not handled");
    FormatInfo f = bc1 ? FormatInfo{8, 4, 4, FMT_COMPRESSED, "bc1"} : FormatInfo{2, 1, 1, FMT_COLOR, "rg8"};
    TexLayout l = layout(t.width, t.height, t.num_mips, &f, t.texture_layout & 7);
    LevelLayout lv = tegra_level(&l, 0);
    std::vector<uint8_t> lin((size_t)lv.wb * lv.hb * f.bpb);
    if (!t.data || t.data->size() < tegra_layer_size(&l)) throw std::runtime_error("decode_level0: short data");
    tegra_deswizzle((const uint8_t *)t.data->data() + lv.offset, lin.data(), lv.wb, lv.hb, 1, f.bpb,
                    lv.block_height_log2, 0);
    if (rg8) {
        Image out(t.width, t.height, 2);
        for (int y = 0; y < t.height; y++) memcpy(out.at(0, y), &lin[(size_t)y * lv.wb * 2], (size_t)t.width * 2);
        return out;
    }
    Image out(t.width, t.height, 3);
    uint8_t blk[16][4];
    for (uint32_t by = 0; by < lv.hb; by++)
        for (uint32_t bx = 0; bx < lv.wb; bx++) {
            bc1_block(&lin[((size_t)by * lv.wb + bx) * 8], blk);
            for (int i = 0; i < 16; i++) {
                int x = (int)bx * 4 + i % 4, y = (int)by * 4 + i / 4;
                if (x < t.width && y < t.height) memcpy(out.at(x, y), blk[i], 3);
            }
        }
    return out;
}

static void bc4_block(const uint8_t *s, uint8_t out[16]) {  // (BC3's alpha)
    int a0 = s[0], a1 = s[1], p[8] = {a0, a1};
    if (a0 > a1) for (int i = 1; i < 7; i++) p[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else {
        for (int i = 1; i < 5; i++) p[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        p[6] = 0, p[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)s[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)p[(bits >> (3 * i)) & 7];
}

std::vector<Image> decode_levels(const BntxTexture &t) {
    uint32_t type = t.format >> 8;
    int bpb = type == 0x1a ? 8 : type == 0x1c ? 16 : type == 0x09 ? 2 : 0;
    if (!bpb) throw std::runtime_error("decode_levels: format not handled");
    bool rg8 = type == 0x09;
    FormatInfo f = rg8 ? FormatInfo{2, 1, 1, FMT_COLOR, "rg8"} : FormatInfo{(uint8_t)bpb, 4, 4, FMT_COMPRESSED, "bc"};
    TexLayout l = layout(t.width, t.height, t.num_mips, &f, t.texture_layout & 7);
    if (!t.data || t.data->size() < tegra_layer_size(&l)) throw std::runtime_error("decode_levels: short data");
    std::vector<Image> out;
    for (int m = 0; m < t.num_mips; m++) {
        LevelLayout lv = tegra_level(&l, (uint32_t)m);
        std::vector<uint8_t> lin((size_t)lv.wb * lv.hb * bpb);
        tegra_deswizzle((const uint8_t *)t.data->data() + lv.offset, lin.data(), lv.wb, lv.hb, 1, (uint32_t)bpb,
                        lv.block_height_log2, 0);
        if (rg8) {
            Image im((int)lv.w, (int)lv.h, 2);
            for (uint32_t y = 0; y < lv.h; y++) memcpy(im.at(0, (int)y), &lin[(size_t)y * lv.wb * 2], (size_t)lv.w * 2);
            out.push_back(im);
            continue;
        }
        Image im((int)lv.w, (int)lv.h, 4);
        uint8_t blk[16][4], a[16];
        for (uint32_t by = 0; by < lv.hb; by++)
            for (uint32_t bx = 0; bx < lv.wb; bx++) {
                const uint8_t *s = &lin[((size_t)by * lv.wb + bx) * bpb];
                bc1_block(bpb == 16 ? s + 8 : s, blk);
                if (bpb == 16) {  // (BC3 colour: always the 4-colour palette)
                    uint16_t c0 = (uint16_t)(s[8] | s[9] << 8), c1 = (uint16_t)(s[10] | s[11] << 8);
                    if (c0 <= c1) {
                        uint8_t fix[8];
                        memcpy(fix, s + 8, 8);
                        int p0[3], p1[3];
                        rgb565(c0, p0), rgb565(c1, p1);
                        uint32_t sel = (uint32_t)(fix[4] | fix[5] << 8 | fix[6] << 16 | (uint32_t)fix[7] << 24);
                        for (int i = 0; i < 16; i++) {
                            int k = (sel >> (2 * i)) & 3;
                            for (int c = 0; c < 3; c++)
                                blk[i][c] = (uint8_t)(k == 0 ? p0[c] : k == 1 ? p1[c] : k == 2 ? (2 * p0[c] + p1[c]) / 3 : (p0[c] + 2 * p1[c]) / 3);
                        }
                    }
                    bc4_block(s, a);
                    for (int i = 0; i < 16; i++) blk[i][3] = a[i];
                }
                for (int i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    if (x < lv.w && y < lv.h) memcpy(im.at((int)x, (int)y), blk[i], 4);
                }
            }
        out.push_back(im);
    }
    return out;
}

// ---- encode
static void rgbcx_init() {
    static std::once_flag once;
    std::call_once(once, [] { rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal); });
}

// one level's blocks, linear (a block past the image's edge repeats its last row / column)
static std::vector<uint8_t> blocks(const Image &im, bool bc3) {
    int wb = (im.w + 3) / 4, hb = (im.h + 3) / 4, bpb = bc3 ? 16 : 8;
    std::vector<uint8_t> out((size_t)wb * hb * bpb);
    for (int by = 0; by < hb; by++)
        for (int bx = 0; bx < wb; bx++) {
            uint8_t px[64];
            for (int i = 0; i < 16; i++) {
                int x = std::min(bx * 4 + i % 4, im.w - 1), y = std::min(by * 4 + i / 4, im.h - 1);
                const uint8_t *p = im.at(x, y);
                px[i * 4] = p[0], px[i * 4 + 1] = p[1], px[i * 4 + 2] = p[2];
                px[i * 4 + 3] = bc3 ? p[3] : 255;
            }
            uint8_t *o = &out[((size_t)by * wb + bx) * bpb];
            if (bc3) rgbcx::encode_bc3(18, o, px);
            else rgbcx::encode_bc1(10, o, px, false, false);
        }
    return out;
}

static std::string encode(const std::vector<Image> &levels, uint32_t bh_log2, bool bc3) {
    rgbcx_init();
    FormatInfo f = {(uint8_t)(bc3 ? 16 : 8), 4, 4, FMT_COMPRESSED, "bc"};
    TexLayout t = layout(levels[0].w, levels[0].h, (int)levels.size(), &f, bh_log2);
    std::string out((size_t)tegra_layer_size(&t), '\0');
    for (size_t i = 0; i < levels.size(); i++) {
        LevelLayout lv = tegra_level(&t, (uint32_t)i);
        if ((int)lv.w != levels[i].w || (int)lv.h != levels[i].h) throw std::runtime_error("encode: level size");
        std::vector<uint8_t> lin = blocks(levels[i], bc3);
        tegra_swizzle(lin.data(), (uint8_t *)&out[lv.offset], lv.wb, lv.hb, 1, f.bpb, lv.block_height_log2, 0);
    }
    return out;
}

std::string encode_bc1(const std::vector<Image> &levels, uint32_t bh) { return encode(levels, bh, false); }
std::string encode_bc3(const std::vector<Image> &levels, uint32_t bh) { return encode(levels, bh, true); }

}  // namespace tex
}  // namespace rando
