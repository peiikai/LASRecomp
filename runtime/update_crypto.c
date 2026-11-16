// Ed25519 for the self-updater's signed manifests (runtime/update_core.h): Monocypher 4.0.2, used under its CC0
// licence (third_party/monocypher/LICENCE.md). Only the desktops that update themselves need it. Two files, as
// Monocypher's two (each has static helpers of the same name): this one and update_crypto_ed25519.c.
#include "update.h"
#if LARECOMP_SELF_UPDATE
#if defined(__clang__)
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wall"
#endif
#include "../third_party/monocypher/monocypher.c"
#endif
