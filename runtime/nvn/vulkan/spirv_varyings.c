// Varying Location renumbering for a last pre-rasterization stage + fragment shader SPIR-V pair (spirv_varyings.h).
//
// Why (exec.c compile_program, device.c compact_varyings): PanVK before Mesa 26.1 on Valhall v9 / v10 (Mali-G57,
// G610, G710) compiles a fragment shader with at most 16 input slots to read its varyings with LD_VAR_BUF_IMM, whose
// byte offset is 16 x (Location) in an 8-bit field. A varying at Location 16 or above (offset 256 and up) overflows
// into the instruction's source format bits and reads garbage: the translated shaders keep the guest's attribute
// numbers as Locations, and some Field shaders read 24-27 (white triangles over the grass, fence and houses on
// Mali-G610, Mesa 25.2.7 / 25.3.6 / 26.0.3). Fixed in Mesa 26.1 (0f0a3225a599, which falls back to LD_VAR when the
// layout doesn't fit). One renumbering for every shader can't work (the Field alone uses 18 different Locations), so
// each vertex + fragment pair is renumbered when its modules are made.
//
// The producer can be any last pre-rasterization stage (Vertex, TessellationEvaluation, Geometry): only its outputs
// and the fragment inputs reach the fragment shader, so the earlier interfaces (vertex -> tessellation control ->
// evaluation, vertex / evaluation -> geometry) keep their Locations. LA only has vertex + fragment programs; the other
// stages are for runtimes that share this file (tools/test_spirv_varyings*.py test all of them).
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spirv_varyings.h"

enum { OP_ENTRY_POINT = 15, OP_TYPE_BOOL = 20, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
       OP_TYPE_MATRIX = 24, OP_TYPE_ARRAY = 28, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_CONSTANT = 43,
       OP_VARIABLE = 59, OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_GROUP_DECORATE = 74,
       OP_GROUP_MEMBER_DECORATE = 75 };
enum { DEC_BUILTIN = 11, DEC_PATCH = 15, DEC_STREAM = 29, DEC_LOCATION = 30, DEC_PER_PRIMITIVE = 5271,
       DEC_PER_VERTEX = 5285 };  // (PerPrimitiveEXT = PerPrimitiveNV, PerVertexKHR = PerVertexNV)
enum { SC_INPUT = 1, SC_OUTPUT = 3 };
enum { EM_VERTEX = 0, EM_TESS_CONTROL = 1, EM_TESS_EVAL = 2, EM_GEOMETRY = 3, EM_FRAGMENT = 4 };
enum { F_BUILTIN = 1, F_PATCH = 2, F_PER_PRIMITIVE = 4, F_PER_VERTEX = 8 };
enum { MAX_LOC = 64, MAX_RANGES = 64 };

typedef struct { uint32_t start, count; uint32_t *word; bool fs; } Range;

typedef struct {
    const uint32_t *w;
    size_t n;
    uint32_t bound;
    uint32_t *def;  // id -> word index of the instruction defining it (types, constants, variables), 0 if none
} Mod;

// Locations a value of type id takes in an output of the last pre-rasterization stage / a fragment input (neither is
// arrayed per vertex), or -1 if unknown.
static int loc_count(const Mod *m, uint32_t id, int depth) {
    if (id >= m->bound || !m->def[id] || depth > 16) return -1;
    const uint32_t *w = m->w + m->def[id];
    uint32_t wc = w[0] >> 16, op = w[0] & 0xFFFF;
    switch (op) {
    case OP_TYPE_BOOL: case OP_TYPE_INT: case OP_TYPE_FLOAT: return 1;
    case OP_TYPE_VECTOR: {
        if (wc < 4 || w[2] >= m->bound || !m->def[w[2]]) return -1;
        const uint32_t *c = m->w + m->def[w[2]];
        bool wide = ((c[0] & 0xFFFF) == OP_TYPE_INT || (c[0] & 0xFFFF) == OP_TYPE_FLOAT) && c[2] == 64;
        return wide && w[3] > 2 ? 2 : 1;
    }
    case OP_TYPE_MATRIX: {
        if (wc < 4) return -1;
        int c = loc_count(m, w[2], depth + 1);
        return c < 0 ? -1 : c * (int)w[3];
    }
    case OP_TYPE_ARRAY: {
        if (wc < 4 || w[3] >= m->bound || !m->def[w[3]]) return -1;
        const uint32_t *len = m->w + m->def[w[3]];
        if ((len[0] & 0xFFFF) != OP_CONSTANT || (len[0] >> 16) != 4 || len[3] > MAX_LOC) return -1;
        int c = loc_count(m, w[2], depth + 1);
        return c < 0 ? -1 : c * (int)len[3];
    }
    case OP_TYPE_STRUCT: {
        int s = 0;
        for (uint32_t i = 2; i < wc; i++) {
            int c = loc_count(m, w[i], depth + 1);
            if (c < 0) return -1;
            s += c;
        }
        return s;
    }
    default: return -1;
    }
}

static const char *model_name(uint32_t em) {
    switch (em) {
    case EM_VERTEX: return "Vertex";
    case EM_TESS_CONTROL: return "TessellationControl";
    case EM_TESS_EVAL: return "TessellationEvaluation";
    case EM_GEOMETRY: return "Geometry";
    case EM_FRAGMENT: return "Fragment";
    case 5267: case 5364: return "a task shader";
    case 5268: case 5365: return "a mesh shader";
    default: return "an unknown stage";
    }
}

// Appends the module's generic varyings to r (true), or false with why set: the Output variables of the last
// pre-rasterization stage (fs false) or the fragment shader's Input variables (fs true).
static bool collect(uint32_t *spv, size_t n, bool fs, Range *r, int *nr, char *why, size_t nwhy) {
    if (n < 5 || spv[0] != 0x07230203) { snprintf(why, nwhy, "not SPIR-V"); return false; }
    const uint32_t sc = fs ? SC_INPUT : SC_OUTPUT;
    Mod m = {spv, n, spv[3], calloc(spv[3] ? spv[3] : 1, sizeof(uint32_t))};
    uint32_t **loc = calloc(m.bound ? m.bound : 1, sizeof *loc);  // id -> its Location literal
    uint8_t *flags = calloc(m.bound ? m.bound : 1, 1);             // id -> F_*
    int entries = 0;
    uint32_t model = ~0u;
    bool ok = true;
    for (size_t i = 5; i < n && ok;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc || i + wc > n) { snprintf(why, nwhy, "truncated"); ok = false; break; }
        uint32_t *w = spv + i;
        switch (op) {
        case OP_ENTRY_POINT:
            entries++;
            if (wc >= 3) model = w[1];
            break;
        case OP_TYPE_BOOL: case OP_TYPE_INT: case OP_TYPE_FLOAT: case OP_TYPE_VECTOR: case OP_TYPE_MATRIX:
        case OP_TYPE_ARRAY: case OP_TYPE_STRUCT: case OP_TYPE_POINTER:
            if (wc >= 2 && w[1] < m.bound) m.def[w[1]] = (uint32_t)i;
            break;
        case OP_CONSTANT: case OP_VARIABLE:
            if (wc >= 3 && w[2] < m.bound) m.def[w[2]] = (uint32_t)i;
            break;
        case OP_DECORATE:
            if (wc >= 3 && w[1] < m.bound) {
                if (w[2] == DEC_LOCATION && wc >= 4) loc[w[1]] = &w[3];
                else if (w[2] == DEC_BUILTIN) flags[w[1]] |= F_BUILTIN;
                else if (w[2] == DEC_PATCH) flags[w[1]] |= F_PATCH;
                else if (w[2] == DEC_PER_PRIMITIVE) flags[w[1]] |= F_PER_PRIMITIVE;
                else if (w[2] == DEC_PER_VERTEX) flags[w[1]] |= F_PER_VERTEX;
                else if (w[2] == DEC_STREAM && wc >= 4 && w[3] != 0) { snprintf(why, nwhy, "Stream %u", w[3]); ok = false; }
            }
            break;
        case OP_MEMBER_DECORATE:
            if (wc >= 4 && w[3] == DEC_LOCATION) { snprintf(why, nwhy, "block member Locations"); ok = false; }
            else if (wc >= 5 && w[3] == DEC_STREAM && w[4] != 0) { snprintf(why, nwhy, "Stream %u", w[4]); ok = false; }
            else if (fs && wc >= 4 && (w[3] == DEC_PER_PRIMITIVE || w[3] == DEC_PER_VERTEX)) {
                snprintf(why, nwhy, "per-%s fragment inputs", w[3] == DEC_PER_PRIMITIVE ? "primitive" : "vertex");
                ok = false;
            }
            break;
        case OP_GROUP_DECORATE: case OP_GROUP_MEMBER_DECORATE: snprintf(why, nwhy, "decoration groups"); ok = false; break;
        default: break;
        }
        i += wc;
    }
    if (ok && entries != 1) { snprintf(why, nwhy, "%d entry points", entries); ok = false; }
    if (ok && fs && model != EM_FRAGMENT) { snprintf(why, nwhy, "consumer is %s, not Fragment", model_name(model)); ok = false; }
    if (ok && !fs && model != EM_VERTEX && model != EM_TESS_EVAL && model != EM_GEOMETRY) {
        snprintf(why, nwhy, "producer is %s, not a last pre-rasterization stage", model_name(model));
        ok = false;
    }
    for (uint32_t id = 0; ok && id < m.bound; id++) {
        if (!m.def[id]) continue;
        const uint32_t *v = spv + m.def[id];
        if ((v[0] & 0xFFFF) != OP_VARIABLE || v[3] != sc) continue;
        uint32_t pt = v[1];
        if (pt >= m.bound || !m.def[pt] || (spv[m.def[pt]] & 0xFFFF) != OP_TYPE_POINTER) { snprintf(why, nwhy, "variable type"); ok = false; break; }
        uint32_t ty = spv[m.def[pt] + 3];
        uint8_t f = flags[id] | (ty < m.bound ? flags[ty] : 0);  // (on the variable or its block type)
        if (fs && (f & (F_PER_PRIMITIVE | F_PER_VERTEX))) {
            snprintf(why, nwhy, "per-%s fragment inputs", f & F_PER_PRIMITIVE ? "primitive" : "vertex");
            ok = false;
            break;
        }
        if (!loc[id] || (f & F_BUILTIN)) continue;
        if (f & F_PATCH) { snprintf(why, nwhy, "Patch varyings"); ok = false; break; }
        int c = loc_count(&m, ty, 0);
        if (c <= 0 || *loc[id] + (uint32_t)c > MAX_LOC) { snprintf(why, nwhy, "unknown varying type or Location"); ok = false; break; }
        if (*nr >= MAX_RANGES) { snprintf(why, nwhy, "too many varyings"); ok = false; break; }
        r[(*nr)++] = (Range){*loc[id], (uint32_t)c, loc[id], fs};
    }
    free(m.def);
    free(loc);
    free(flags);
    return ok;
}

static int by_start(const void *a, const void *b) {
    const Range *x = a, *y = b;
    return x->start != y->start ? (x->start < y->start ? -1 : 1) : (x->fs != y->fs ? (x->fs ? -1 : 1) : 0);
}

int spirv_compact_varyings(uint32_t *pre, size_t npre, uint32_t *fs, size_t nfs, uint32_t limit, char *info, size_t ninfo) {
    Range r[MAX_RANGES];
    int nr = 0;
    char why[96] = "";
    if (info && ninfo) info[0] = 0;
    // (nothing is written until both modules are known to be fine)
    if (!collect(pre, npre, false, r, &nr, why, sizeof why) || !collect(fs, nfs, true, r, &nr, why, sizeof why)) {
        if (info && ninfo) snprintf(info, ninfo, "%s", why);
        return -1;
    }
    bool need = false;
    for (int i = 0; i < nr; i++) need |= r[i].fs && r[i].start + r[i].count > limit;
    if (!need) return 0;
    // units: the old Locations covered by overlapping ranges (of either module), each moved as one block
    qsort(r, (size_t)nr, sizeof *r, by_start);
    uint32_t ustart[MAX_RANGES], uend[MAX_RANGES], ubase[MAX_RANGES];
    bool ufs[MAX_RANGES];
    int unit_of[MAX_RANGES], nu = 0;
    for (int i = 0; i < nr; i++) {
        if (nu && r[i].start < uend[nu - 1]) {
            if (r[i].start + r[i].count > uend[nu - 1]) uend[nu - 1] = r[i].start + r[i].count;
            ufs[nu - 1] |= r[i].fs;
        } else {
            ustart[nu] = r[i].start, uend[nu] = r[i].start + r[i].count, ufs[nu] = r[i].fs;
            nu++;
        }
        unit_of[i] = nu - 1;
    }
    // the fragment shader's units first, then the outputs nothing reads (in old order within each)
    uint32_t next = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int u = 0; u < nu; u++)
            if (ufs[u] == (pass == 0)) ubase[u] = next, next += uend[u] - ustart[u];
    int o = 0;
    if (info && ninfo) o = snprintf(info, ninfo, "fragment inputs");
    for (int i = 0; i < nr; i++) {
        uint32_t nl = ubase[unit_of[i]] + (r[i].start - ustart[unit_of[i]]);
        if (info && r[i].fs && o >= 0 && (size_t)o < ninfo)
            o += snprintf(info + o, ninfo - (size_t)o, " %u->%u", r[i].start, nl);
        *r[i].word = nl;
    }
    return 1;
}
