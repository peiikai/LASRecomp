// The game-address tables the generator reads (port of tools/game_addresses.py): the same headers the runtime
// includes (runtime/game_addresses/), compiled in. Only patches_<key>.h, as the Python reads: the runtime_<key>.h
// tables do not affect generated code.
#include "gen_internal.hpp"

#include <cctype>

namespace lasgen {

const GameVersion GAME_VERSIONS[] = {
#define GA_VERSION(key, label, build_id) {#key, label, build_id},
#define GA_RELEASE(key, label, npdm_sha256)
#include "../game_addresses/versions.h"
#undef GA_VERSION
#undef GA_RELEASE
};
const int GAME_VERSION_COUNT = (int)(sizeof GAME_VERSIONS / sizeof GAME_VERSIONS[0]);

#define GA_ADDR(name, value) {#name, UINT64_C(value)},
static const std::pair<const char *, uint64_t> ga_v1_0_0[] = {
#include "../game_addresses/patches_v1_0_0.h"
};
static const std::pair<const char *, uint64_t> ga_v1_0_1[] = {
#include "../game_addresses/patches_v1_0_1.h"
};
#undef GA_ADDR

template <size_t N> static AddressTable make_table(const std::pair<const char *, uint64_t> (&t)[N]) {
    AddressTable m;
    for (auto &e : t)
        if (!m.emplace(e.first, e.second).second)
            throw GenError(std::string("Invalid or duplicate game-address definition: ") + e.first);
    return m;
}

const AddressTable &address_table(const std::string &key) {
    static const AddressTable v1_0_0 = make_table(ga_v1_0_0), v1_0_1 = make_table(ga_v1_0_1);
    if (key == "v1_0_0") return v1_0_0;
    if (key == "v1_0_1") return v1_0_1;
    throw GenError("Unknown game version " + key);
}

// The version key for a main build ID (hex, any case; Nso.build_id strips trailing zero bytes).
std::string version_for_build_id(const std::string &build_id) {
    std::string want = build_id;
    for (auto &ch : want) ch = (char)toupper((unsigned char)ch);
    std::string padded = want;
    if (padded.size() < 64) padded.append(64 - padded.size(), '0');  // (the header's 32-byte field, zero-filled)
    bool tail_zero = padded.find_first_not_of('0', 40) == std::string::npos;
    for (int i = 0; i < GAME_VERSION_COUNT; i++)
        if (padded.compare(0, 40, GAME_VERSIONS[i].build_id) == 0 && tail_zero) return GAME_VERSIONS[i].key;
    std::string supported;
    for (int i = 0; i < GAME_VERSION_COUNT; i++) {
        if (i) supported += ", ";
        supported += std::string(GAME_VERSIONS[i].label) + " (" + std::string(GAME_VERSIONS[i].build_id).substr(0, 16) + ")";
    }
    throw GenError("These game files are an unsupported version of the game (main build ID " +
                   (want.empty() ? std::string("missing") : want.substr(0, 16)) + "). Supported: " + supported + ".");
}

// Expand @NAME@ as hex digits inside generated-C addresses, symbols or labels.
std::string addr_text(const AddressTable &a, const std::string &code) {
    std::string out;
    size_t i = 0;
    while (i < code.size()) {
        if (code[i] == '@' && i + 1 < code.size() && isupper((unsigned char)code[i + 1])) {
            size_t j = i + 1;
            while (j < code.size() && (isupper((unsigned char)code[j]) || isdigit((unsigned char)code[j]) || code[j] == '_')) j++;
            if (j < code.size() && code[j] == '@') {
                std::string name = code.substr(i + 1, j - i - 1);
                auto it = a.find(name);
                if (it == a.end()) throw GenError("AttributeError: " + name);
                out += hexs(it->second);
                i = j + 1;
                continue;
            }
        }
        out += code[i++];
    }
    return out;
}

}  // namespace lasgen
