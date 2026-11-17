// Varying Location renumbering for a last pre-rasterization stage + fragment shader SPIR-V pair (spirv_varyings.c).
// No Vulkan dependency, so the host tests (tools/test_spirv_varyings.py, tools/test_spirv_varyings_render.py) build
// it alone.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Renumbers the generic varyings between the last pre-rasterization stage `pre` (a Vertex, TessellationEvaluation or
// Geometry shader; the stage is read from its OpEntryPoint) and the fragment shader `fs` it feeds, in place: pre's
// Output variables and fs's Input variables get dense Locations, the fragment shader's inputs first (from 0, in their
// old order), then the outputs it doesn't read. Only those Location literals change: builtins, Component,
// interpolation decorations, pre's inputs (the interfaces between earlier stages, which never reach the fragment
// shader) and fragment outputs are left alone. Variables spanning several Locations (arrays, matrices, 64-bit vectors)
// keep their extent, and Locations shared or overlapped between variables stay shared.
// Does nothing unless some fragment input reaches Location `limit` or above.
// Returns 1 if renumbered, 0 if not needed, -1 if a module has something this doesn't handle (any other producer
// stage, such as TessellationControl or a mesh shader; several entry points; a Stream other than 0; per-primitive or
// per-vertex fragment inputs; block member Locations; decoration groups; an unknown type): both modules are then
// unchanged. If info is given, it gets a short description (old -> new Locations of the fragment inputs, or the reason
// for -1).
int spirv_compact_varyings(uint32_t *pre, size_t npre, uint32_t *fs, size_t nfs, uint32_t limit, char *info, size_t ninfo);
