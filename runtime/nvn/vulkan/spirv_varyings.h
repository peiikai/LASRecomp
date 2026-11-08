// Varying Location renumbering for a vertex + fragment SPIR-V pair (spirv_varyings.c). No Vulkan dependency, so the
// host test (tools/test_spirv_varyings.py) builds it alone.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Renumbers the generic varyings of a vertex shader (Output variables) and the fragment shader it feeds (Input
// variables) to dense Locations, in place: the fragment shader's inputs first (from 0, in their old order), then the
// vertex outputs it doesn't read. Only Location literals change: builtins, Component, interpolation decorations,
// vertex inputs and fragment outputs are left alone. Variables spanning several Locations (arrays, matrices, 64-bit
// vectors) keep their extent, and Locations shared or overlapped between variables stay shared.
// Does nothing unless some fragment input reaches Location `limit` or above.
// Returns 1 if renumbered, 0 if not needed, -1 if a module has something this doesn't handle (block members with
// their own Locations, decoration groups, several entry points, an unknown type): both modules are then unchanged.
// If info is given, it gets a short description (old -> new Locations of the fragment inputs, or the reason for -1).
int spirv_compact_varyings(uint32_t *vs, size_t nvs, uint32_t *fs, size_t nfs, uint32_t limit, char *info, size_t ninfo);
