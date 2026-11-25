// SHA-512 (FIPS 180-4) for the randomizer: Python's random.seed(str) hashes the seed string with it (pyrandom.h), so
// every PyRandom seeded from a string, in the game and in lasrando, goes through this one function.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rando_sha512(uint8_t hash[64], const uint8_t *message, size_t message_size);

#ifdef __cplusplus
}
#endif
