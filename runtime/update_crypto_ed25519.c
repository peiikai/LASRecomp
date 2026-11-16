// Ed25519 (SHA-512 based, as Sparkle and OpenSSL use it) for the self-updater: see update_crypto.c.
#include "update.h"
#if LARECOMP_SELF_UPDATE
#if defined(__clang__)
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wall"
#endif
#include "../third_party/monocypher/monocypher-ed25519.c"
#endif
