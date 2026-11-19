// lasrando texture-check (texture_check.h): a file whose textures the seed build compresses itself, against the
// Python generator's.
#include "texture_check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <iterator>

#include "bntx.h"
#include "sarc.h"
#include "texture.h"

namespace rando {

namespace {

std::string read_all(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool same_fields(const BntxTexture &a, const BntxTexture &b) {
    return a.name == b.name && a.flags == b.flags && a.dim == b.dim && a.tile_mode == b.tile_mode &&
           a.swizzle == b.swizzle && a.num_mips == b.num_mips && a.num_samples == b.num_samples &&
           a.format == b.format && a.access_flags == b.access_flags && a.width == b.width && a.height == b.height &&
           a.depth == b.depth && a.array_length == b.array_length && a.texture_layout == b.texture_layout &&
           a.texture_layout2 == b.texture_layout2 && a.image_size == b.image_size && a.alignment == b.alignment &&
           a.comp_sel == b.comp_sel && a.img_dim == b.img_dim && a.mip_offsets == b.mip_offsets;
}

// one texture's pixels, level by level: false when a level is too far off
bool pixels(const char *file, const BntxTexture &want, const BntxTexture &got) {
    std::vector<tex::Image> w = tex::decode_levels(want), g = tex::decode_levels(got);
    double worst = 0;
    int maxd = 0;
    for (size_t m = 0; m < w.size(); m++) {
        double sum = 0;
        for (size_t i = 0; i < w[m].px.size(); i++) {
            int d = w[m].px[i] - g[m].px[i];
            sum += d * d;
            if (abs(d) > maxd) maxd = abs(d);
        }
        double rms = sqrt(sum / (double)w[m].px.size());
        if (rms > worst) worst = rms;
    }
    bool ok = worst <= TEXTURE_RMS_LIMIT;
    printf("  %s %s: %zu levels, pixels as the Python's within RMS %.2f (largest difference %d)%s\n", file,
           want.name.c_str(), w.size(), worst, maxd, ok ? "" : " - TOO FAR");
    return ok;
}

// two banks: the same apart from texture pixel data; that compared by its pixels
bool banks(const char *file, const std::string &want, const std::string &got) {
    if (want == got) return true;
    Bntx bw = Bntx::parse(want), bg = Bntx::parse(got);
    if (bw.textures.size() != bg.textures.size()) {
        printf("  %s: %zu textures, the Python's has %zu\n", file, bg.textures.size(), bw.textures.size());
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < bw.textures.size(); i++) {
        BntxTexture &a = bw.textures[i], &b = bg.textures[i];
        if (!same_fields(a, b)) {
            printf("  %s %s: its description differs from the Python's\n", file, a.name.c_str());
            ok = false;
            continue;
        }
        if (*a.data == *b.data) continue;
        ok = pixels(file, a, b) && ok;
        b.data = a.data;  // (the rest must then be the same)
    }
    if (ok && bw.save() != bg.save()) {
        printf("  %s: the bank differs from the Python's beyond its pixels\n", file);
        ok = false;
    }
    return ok;
}

}  // namespace

const double TEXTURE_RMS_LIMIT = 4.0;

int texture_check(const std::string &want_path, const std::string &got_path, const std::string &rel) {
    std::string want = read_all(want_path), got = read_all(got_path);
    if (want == got) return 0;
    const char *name = rel.c_str();
    try {
        if (rel.size() > 4 && !rel.compare(rel.size() - 4, 4, ".arc")) {
            Sarc sw = Sarc::parse(want), sg = Sarc::parse(got);
            if (sw.files.size() != sg.files.size()) { printf("  %s: file count differs\n", name); return 1; }
            bool ok = true;
            for (size_t i = 0; i < sw.files.size(); i++) {
                if (sw.files[i].first != sg.files[i].first) { printf("  %s: file names differ\n", name); return 1; }
                if (sw.files[i].second == sg.files[i].second) continue;
                if (sw.files[i].first.size() < 5 || sw.files[i].first.compare(sw.files[i].first.size() - 5, 5, ".bntx")) {
                    printf("  %s: %s differs\n", name, sw.files[i].first.c_str());
                    ok = false;
                    continue;
                }
                if (!banks((rel + ":" + sw.files[i].first).c_str(), sw.files[i].second, sg.files[i].second)) ok = false;
                sg.files[i].second = sw.files[i].second;
            }
            if (ok && sw.write() != sg.write()) { printf("  %s: the archive differs beyond its textures\n", name); ok = false; }
            return ok ? 0 : 1;
        }
        // a model (.bfres) holding one bank: the bank compared as above, the rest byte for byte
        std::string magic("BNTX\0\0\0\0", 8);
        size_t aw = want.find(magic), ag = got.find(magic);
        if (aw == std::string::npos || aw != ag || want.size() != got.size()) {
            printf("  %s: differs outside its textures\n", name);
            return 1;
        }
        uint32_t n;
        memcpy(&n, &want[aw + 0x1C], 4);
        if (want.compare(0, aw, got, 0, aw) || want.compare(aw + n, std::string::npos, got, aw + n, std::string::npos)) {
            printf("  %s: differs outside its textures\n", name);
            return 1;
        }
        // (the bank is patched in place, not saved again: compare it with its pixel data swapped back)
        Bntx bw = Bntx::parse(want.substr(aw, n)), bg = Bntx::parse(got.substr(aw, n));
        std::string patched = got;
        bool ok = bw.textures.size() == bg.textures.size();
        for (size_t i = 0; ok && i < bw.textures.size(); i++) {
            if (!same_fields(bw.textures[i], bg.textures[i])) { ok = false; break; }
            if (*bw.textures[i].data == *bg.textures[i].data) continue;
            ok = pixels(name, bw.textures[i], bg.textures[i]) && ok;
            uint64_t at;
            memcpy(&at, &want[aw + bw.textures[i].ptrs_addr], 8);
            patched.replace(aw + at, bw.textures[i].data->size(), *bw.textures[i].data);
        }
        if (ok && patched != want) { printf("  %s: differs beyond its pixels\n", name); ok = false; }
        return ok ? 0 : 1;
    } catch (const std::exception &e) {
        printf("  %s: %s\n", name, e.what());
        return 1;
    }
}

}  // namespace rando
