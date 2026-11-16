// Signs the self-updater's manifests (runtime/update_core.h) and the macOS update's Sparkle signature, with one
// Ed25519 key (Monocypher, CC0). Used by release.yml and nightly.yml; the private key is the UPDATE_SIGNING_KEY secret
// (base64 of the 32-byte seed, the format Sparkle's own tools use), never on disk in the repo.
// Build: c++ -std=c++17 -O2 -Iruntime tools/update_sign.cpp -o build/update_sign
//   update_sign keygen <private key file>          a new key: the seed (base64) into the file, the public key printed
//   update_sign public                              the public key of $UPDATE_SIGNING_KEY (hex, then base64)
//   update_sign manifest <version> <platform> <file> [<folder>] > update-<platform>.txt
//                                                   the signed manifest for a release file; <folder>: what a zip holds
//                                                   (its top folder), every file in it listed
//   update_sign sparkle <file>                      the file's signature for a Sparkle appcast (base64)
//   update_sign verify <manifest> <public key hex>  checks a manifest (exit 0: good)
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <algorithm>
#include <filesystem>
#include <string>

#include "update_core.h"
// (Monocypher's C compiled into this file as C++, so one command builds it on every platform)
#include "../third_party/monocypher/monocypher.c"
#undef FOR  // (each file defines its own)
#include "../third_party/monocypher/monocypher-ed25519.c"

namespace fs = std::filesystem;

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64(const uint8_t *p, size_t n) {
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o += B64[v >> 18 & 63], o += B64[v >> 12 & 63];
        o += i + 1 < n ? B64[v >> 6 & 63] : '=';
        o += i + 2 < n ? B64[v & 63] : '=';
    }
    return o;
}
static bool unb64(const std::string &s, std::vector<uint8_t> *out) {
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const char *k = strchr(B64, c);
        if (!k || !c) return false;
        v = v << 6 | (uint32_t)(k - B64), bits += 6;
        if (bits >= 8) bits -= 8, out->push_back((uint8_t)(v >> bits));
    }
    return true;
}
static std::string hex(const uint8_t *p, size_t n) {
    std::string o;
    char b[3];
    for (size_t i = 0; i < n; i++) snprintf(b, 3, "%02x", p[i]), o += b;
    return o;
}
static void die(const char *m) {
    fprintf(stderr, "update_sign: %s\n", m);
    exit(1);
}

// $UPDATE_SIGNING_KEY -> the 64-byte Monocypher secret key and the public key
static void key(uint8_t sk[64], uint8_t pk[32]) {
    const char *e = getenv("UPDATE_SIGNING_KEY");
    std::vector<uint8_t> seed;
    if (!e || !*e) die("UPDATE_SIGNING_KEY is not set");
    if (!unb64(e, &seed) || seed.size() != 32) die("UPDATE_SIGNING_KEY: not a base64 32-byte seed");
    crypto_ed25519_key_pair(sk, pk, seed.data());  // (wipes the seed)
}
static bool read_file(const char *path, std::string *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char b[1 << 16];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out->append(b, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);  // (the manifest goes out as signed: LF lines, never CR LF)
#endif
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "keygen" && argc == 3) {
        uint8_t seed[32], sk[64], pk[32];
        FILE *r = fopen("/dev/urandom", "rb");
        if (!r || fread(seed, 1, 32, r) != 32) die("no /dev/urandom");
        fclose(r);
        if (upd_exists(argv[2])) die("the key file exists already: never replace a published key");
        std::string s = b64(seed, 32);
        crypto_ed25519_key_pair(sk, pk, seed);
        FILE *f = fopen(argv[2], "wx");
        if (!f || fprintf(f, "%s\n", s.c_str()) < 0 || fclose(f) != 0) die("can't write the key file");
        printf("public %s\npublic-base64 %s\n", hex(pk, 32).c_str(), b64(pk, 32).c_str());
        return 0;
    }
    if (cmd == "public" && argc == 2) {
        uint8_t sk[64], pk[32];
        key(sk, pk);
        printf("public %s\npublic-base64 %s\n", hex(pk, 32).c_str(), b64(pk, 32).c_str());
        return 0;
    }
    if (cmd == "sparkle" && argc == 3) {
        uint8_t sk[64], pk[32], sig[64];
        key(sk, pk);
        std::string data;
        if (!read_file(argv[2], &data)) die("can't read the file");
        crypto_ed25519_sign(sig, sk, (const uint8_t *)data.data(), data.size());
        printf("%s\n", b64(sig, 64).c_str());
        return 0;
    }
    if (cmd == "manifest" && (argc == 5 || argc == 6)) {
        uint8_t sk[64], pk[32], sig[64];
        key(sk, pk);
        UpdManifest m;
        m.version = argv[2], m.platform = argv[3];
        m.asset = fs::path(argv[4]).filename().string();
        std::replace(m.asset.begin(), m.asset.end(), ' ', '.');  // (GitHub stores "LAS Recomp-..." as "LAS.Recomp-...")
        m.asset_sha256 = upd_file_sha256(argv[4], &m.asset_size);
        if (m.asset_sha256.empty()) die("can't read the file");
        if (argc == 6) {
            fs::path top(argv[5]);
            for (auto &e : fs::recursive_directory_iterator(top)) {
                if (e.is_symlink()) die("a link inside the folder (zips can't hold them)");
                if (!e.is_regular_file()) continue;
                UpdFile f;
                f.path = fs::relative(e.path(), top).generic_string();
                if (!upd_safe_path(f.path)) { fprintf(stderr, "%s\n", f.path.c_str()); die("a file the updater may not write"); }
                f.sha256 = upd_file_sha256(e.path().string(), &f.size);
                if (f.sha256.empty()) die("can't read a file in the folder");
                m.files.push_back(f);
            }
            std::sort(m.files.begin(), m.files.end(), [](const UpdFile &a, const UpdFile &b) { return a.path < b.path; });
            if (m.files.empty()) die("an empty folder");
        }
        std::string t = upd_manifest_text(m);
        crypto_ed25519_sign(sig, sk, (const uint8_t *)t.data(), t.size());
        t += "signature " + hex(sig, 64) + "\n";
        UpdManifest back;  // (it must read back as the app will read it)
        std::string why;
        if (!upd_parse_manifest(t, pk, &back, &why)) die(why.c_str());
        fwrite(t.data(), 1, t.size(), stdout);
        return 0;
    }
    if (cmd == "verify" && argc == 4) {
        std::string t, why;
        uint8_t pk[32];
        if (!read_file(argv[2], &t)) die("can't read the manifest");
        if (!upd_hex(argv[3], pk, 32)) die("not a public key (64 hex)");
        UpdManifest m;
        if (!upd_parse_manifest(t, pk, &m, &why)) die(why.c_str());
        printf("%s %s: %s, %zu files\n", m.version.c_str(), m.platform.c_str(), m.asset.c_str(), m.files.size());
        return 0;
    }
    fprintf(stderr, "usage: see the top of tools/update_sign.cpp\n");
    return 2;
}
