// The seed build's texture steps: tools/rando/chest_looks.py write_models (LAS Recomp) and LAS-Randomizer's
// makeGeneralARCChanges (RandomizerCore/mod_generator.py + Tools/bntx_tools.py, MIT, Owen-Splat and contributors),
// ported. Each writes the same file as its Python; the textures in it are compressed with rgbcx instead of quicktex,
// so those bytes differ while the pixels match (runtime/rando/fmt/texture.h).
#include "textures.h"

#include <stdio.h>
#include <string.h>

#include <stdexcept>

#include "../fmt/bntx.h"
#include "../fmt/sarc.h"

namespace rando {
namespace textures {

using tex::Image;

static const char *const CHEST_TEXTURE = "MI_dungeonTreasureBox_01_alb";

static int clamp_int(double x) { return x < 0 ? 0 : x > 255 ? 255 : (int)x; }

Image chest_recolour(const Image &src, const char *look) {
    // look: (body (hue 0-255 or -1 = keep, saturation factor, value factor), gold trim (the same))
    struct Part { int hue; double sf, vf; };
    struct Look { Part body, trim; };
    static const Look key = {{150, 1.0, 1.25}, {-1, 0.08, 0.85}}, plain = {{16, 0.6, 0.62}, {22, 0.45, 0.48}};
    const Look &lk = !strcmp(look, "key") ? key : plain;
    Image hsv = tex::rgb_to_hsv(src);
    Image h = tex::band(hsv, 0), s = tex::band(hsv, 1), v = tex::band(hsv, 2);
    uint8_t t_gold_h[256], t_gold_s[256], t_speck_h[256], t_speck_v[256];
    for (int x = 0; x < 256; x++) {
        t_gold_h[x] = 16 <= x && x <= 52 ? 255 : 0;
        t_gold_s[x] = x > 70 ? 255 : 0;
        t_speck_h[x] = x <= 16 || x >= 250 ? 255 : 0;
        t_speck_v[x] = x > 175 ? 255 : 0;
    }
    // the gold trim: saturated yellow / orange, and its bright red specks (the wood is darker)
    Image gold = tex::multiply(tex::point(h, t_gold_h), tex::point(s, t_gold_s));
    Image specks = tex::multiply(tex::point(h, t_speck_h), tex::point(v, t_speck_v));
    Image mask = tex::gaussian_blur(tex::lighter(gold, specks), 0.8f);
    auto part = [&](const Part &p) {
        uint8_t th[256], ts[256], tv[256];
        for (int x = 0; x < 256; x++)
            th[x] = (uint8_t)(p.hue >= 0 ? p.hue : x), ts[x] = (uint8_t)clamp_int(x * p.sf), tv[x] = (uint8_t)clamp_int(x * p.vf);
        Image hh = tex::point(h, th), ss = tex::point(s, ts), vv = tex::point(v, tv);
        return tex::hsv_to_rgb(tex::merge({&hh, &ss, &vv}));
    };
    return tex::composite(part(lk.trim), part(lk.body), mask);
}

std::vector<Image> mip_chain(const Image &im) {
    // (quicktex: RGB as RGBX, resized as is; an image with alpha has its colour and its alpha resized apart, never
    // premultiplied)
    Image full = tex::convert(im, 4);
    bool alpha = im.c == 2 || im.c == 4;
    Image colour = alpha ? tex::convert(im, 3) : Image(), a = alpha ? tex::band(full, 3) : Image();
    std::vector<Image> out;
    for (auto &sz : tex::mip_sizes(im.w, im.h)) {
        if (out.empty()) { out.push_back(full); continue; }
        if (!alpha) { out.push_back(tex::resize_bilinear(full, sz.first, sz.second)); continue; }
        Image cr = tex::resize_bilinear(colour, sz.first, sz.second), ar = tex::resize_bilinear(a, sz.first, sz.second);
        Image lvl(sz.first, sz.second, 4);
        for (size_t i = 0, n = (size_t)sz.first * sz.second; i < n; i++)
            memcpy(&lvl.px[i * 4], &cr.px[i * 3], 3), lvl.px[i * 4 + 3] = ar.px[i];
        out.push_back(lvl);
    }
    return out;
}

Image logo_banner() {
    Image out(LOGO_W, LOGO_H, 2);
    size_t at = 0;
    for (size_t i = 0; i + 2 < LOGO_RLE_SIZE; i += 3)
        for (int n = 0; n <= LOGO_RLE[i]; n++) {
            if (at >= out.px.size() / 2) throw std::runtime_error("logo banner: too many pixels");
            out.px[at * 2] = LOGO_RLE[i + 1], out.px[at * 2 + 1] = LOGO_RLE[i + 2], at++;
        }
    if (at != out.px.size() / 2) throw std::runtime_error("logo banner: too few pixels");
    return out;
}

// the BNTX bank inside a .bfres: where it starts and how long it is
static bool bfres_bntx(const std::string &d, size_t &at, size_t &len) {
    static const char magic[8] = {'B', 'N', 'T', 'X', 0, 0, 0, 0};
    for (size_t i = 0; i + 0x20 <= d.size(); i += 4)  // (the bank sits aligned)
        if (!memcmp(&d[i], magic, 8)) {
            uint32_t n;
            memcpy(&n, &d[i + 0x1C], 4);
            if (i + n > d.size()) return false;
            at = i, len = n;
            return true;
        }
    return false;
}

// chest_looks.write_models: actor/ObjTreasureBox{Key,Plain}.bfres, the game's chest with only its texture's pixel
// bytes changed
static void chest_models(SeedBuild &b) {
    std::string data;
    if (!b.read_game("region_common/actor/ObjTreasureBox.bfres", data))
        throw std::runtime_error("no actor/ObjTreasureBox.bfres");
    size_t at, len;
    if (!bfres_bntx(data, at, len)) throw std::runtime_error("no BNTX in ObjTreasureBox.bfres");
    Bntx bank = Bntx::parse(data.substr(at, len));
    const BntxTexture *t = nullptr;
    for (const BntxTexture &x : bank.textures)
        if (x.name == CHEST_TEXTURE) t = &x;
    if (!t) throw std::runtime_error("no chest texture in ObjTreasureBox.bfres");
    if ((t->format >> 8) != 0x1a || t->tile_mode != 0) throw std::runtime_error("chest texture: unexpected format");
    // where its pixels are in the file: the texture's mip pointer (relative to the bank)
    uint64_t data_at;
    memcpy(&data_at, &data[at + t->ptrs_addr], 8);
    Image src = tex::decode_level0(*t);
    static const struct { const char *look, *file; } kFiles[] = {
        {"key", "region_common/actor/ObjTreasureBoxKey.bfres"}, {"plain", "region_common/actor/ObjTreasureBoxPlain.bfres"}};
    for (auto &f : kFiles) {
        std::vector<Image> levels = mip_chain(chest_recolour(src, f.look));
        if ((int)levels.size() != t->num_mips) throw std::runtime_error("recoloured chest: mip count");
        std::string px = tex::encode_bc1(levels, t->texture_layout & 7);
        if (px.size() != t->image_size) throw std::runtime_error("recoloured chest does not match the original layout");
        std::string out = data;
        memcpy(&out[at + data_at], px.data(), px.size());
        b.write_bytes(f.file, out);
    }
}

// makeGeneralARCChanges (bntx_tools.createRandomizerTitleScreenArchive): the title logo with the Randomizer banner
// pasted on, as a BC3 texture with a full mip chain (the LAS BNTX editor's replace), the bank saved again
static void title_logo(SeedBuild &b) {
    const char *path = "region_common/ui/StartUp.arc";
    try {
        std::string raw;
        if (!b.read_bytes(path, raw)) throw std::runtime_error("no ui/StartUp.arc");
        Sarc arc = Sarc::parse(raw);
        const std::string *bntx = arc.get("timg/__Combined.bntx");
        if (!bntx) throw std::runtime_error("no timg/__Combined.bntx");
        Bntx bank = Bntx::parse(*bntx);
        BntxTexture *t = nullptr;
        for (BntxTexture &x : bank.textures)
            if (x.name == "Logo_00^f") t = &x;
        if (!t) throw std::runtime_error("Texture Logo_00^f not found");
        Image logo = tex::decode_level0(*t);  // (LA)
        tex::paste_masked(logo, logo_banner(), 0, 0);
        std::vector<Image> levels = mip_chain(logo);
        uint32_t bh = t->texture_layout & 7;
        int64_t total = 0;
        t->mip_offsets = tex::level_offsets(logo.w, logo.h, (int)levels.size(), 16, bh, &total);
        t->data = std::make_shared<const std::string>(tex::encode_bc3(levels, bh));
        if ((int64_t)t->data->size() != total) throw std::runtime_error("logo: layout size");
        t->format = 0x1c01;  // BC3, unorm
        t->num_mips = (uint16_t)levels.size();
        t->image_size = (uint32_t)total;
        t->comp_sel = 0x05040302;  // (r, g, b, a)
        t->flags = 1;
        arc.put("timg/__Combined.bntx", bank.save());
        b.write_bytes(path, arc.write());
    } catch (const std::exception &e) {  // (as the Python: the logo isn't needed to play)
        fprintf(stderr, "[rando] build: title logo skipped: %s\n", e.what());
    }
}

}  // namespace textures

std::vector<BuildStep> steps_textures() {
    return {{"mod_generator.makeGeneralARCChanges", textures::title_logo},
            {"chest_looks.write_models", textures::chest_models}};
}

}  // namespace rando
