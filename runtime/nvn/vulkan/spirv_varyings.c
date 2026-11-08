// Varying Location renumbering for a vertex + fragment SPIR-V pair (spirv_varyings.h).
//
// Why (exec.c compile_program, device.c compact_varyings): PanVK before Mesa 26.1 on Valhall v9 / v10 (Mali-G57,
// G610, G710) compiles a fragment shader with at most 16 input slots to read its varyings with LD_VAR_BUF_IMM, whose
// byte offset is 16 x (Location) in an 8-bit field. A varying at Location 16 or above (offset 256 and up) overflows
// into the instruction's source format bits and reads garbage: the translated shaders keep the guest's attribute
// numbers as Locations, and some Field shaders read 24-27 (white triangles over the grass, fence and houses on
// Mali-G610, Mesa 25.2.7 / 25.3.6 / 26.0.3). Fixed in Mesa 26.1 (0f0a3225a599, which falls back to LD_VAR when the
// layout doesn't fit). One renumbering for every shader can't work (the Field alone uses 18 different Locations), so
// each vertex + fragment pair is renumbered when its modules are made.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spirv_varyings.h"

enum { OP_ENTRY_POINT = 15, OP_TYPE_BOOL = 20, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
       OP_TYPE_MATRIX = 24, OP_TYPE_ARRAY = 28, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_CONSTANT = 43,
       OP_VARIABLE = 59, OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_GROUP_DECORATE = 74,
       OP_GROUP_MEMBER_DECORATE = 75 };
enum { DEC_BUILTIN = 11, DEC_LOCATION = 30 };
enum { SC_INPUT = 1, SC_OUTPUT = 3 };
enum { MAX_LOC = 64, MAX_RANGES = 64 };

typedef struct { uint32_t start, count; uint32_t *word; bool fs; } Range;

typedef struct {
    const uint32_t *w;
    size_t n;
    uint32_t bound;
    uint32_t *def;  // id -> word index of the instruction defining it (types, constants, variables), 0 if none
} Mod;

// Locations a value of type id takes in a vertex output / fragment input (no per-vertex arrays: vertex + fragment
// only), or -1 if unknown.
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

// Appends the module's generic varyings in storage class sc to r (true), or false with *why set.
static bool collect(uint32_t *spv, size_t n, uint32_t sc, bool fs, Range *r, int *nr, const char **why) {
    if (n < 5 || spv[0] != 0x07230203) { *why = "not SPIR-V"; return false; }
    Mod m = {spv, n, spv[3], calloc(spv[3] ? spv[3] : 1, sizeof(uint32_t))};
    uint32_t **loc = calloc(m.bound ? m.bound : 1, sizeof *loc);  // id -> its Location literal
    uint8_t *builtin = calloc(m.bound ? m.bound : 1, 1);
    int entries = 0;
    bool ok = true;
    for (size_t i = 5; i < n && ok;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc || i + wc > n) { *why = "truncated"; ok = false; break; }
        uint32_t *w = spv + i;
        switch (op) {
        case OP_ENTRY_POINT: entries++; break;
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
                else if (w[2] == DEC_BUILTIN) builtin[w[1]] = 1;
            }
            break;
        case OP_MEMBER_DECORATE:
            if (wc >= 4 && w[3] == DEC_LOCATION) { *why = "block member Locations"; ok = false; }
            break;
        case OP_GROUP_DECORATE: case OP_GROUP_MEMBER_DECORATE: *why = "decoration groups"; ok = false; break;
        default: break;
        }
        i += wc;
    }
    if (ok && entries != 1) { *why = "not one entry point"; ok = false; }
    for (uint32_t id = 0; ok && id < m.bound; id++) {
        if (!m.def[id] || !loc[id] || builtin[id]) continue;
        const uint32_t *v = spv + m.def[id];
        if ((v[0] & 0xFFFF) != OP_VARIABLE || v[3] != sc) continue;
        uint32_t pt = v[1];
        if (pt >= m.bound || !m.def[pt] || (spv[m.def[pt]] & 0xFFFF) != OP_TYPE_POINTER) { *why = "variable type"; ok = false; break; }
        int c = loc_count(&m, spv[m.def[pt] + 3], 0);
        if (c <= 0 || *loc[id] + (uint32_t)c > MAX_LOC) { *why = "unknown varying type or Location"; ok = false; break; }
        if (*nr >= MAX_RANGES) { *why = "too many varyings"; ok = false; break; }
        r[(*nr)++] = (Range){*loc[id], (uint32_t)c, loc[id], fs};
    }
    free(m.def);
    free(loc);
    free(builtin);
    return ok;
}

static int by_start(const void *a, const void *b) {
    const Range *x = a, *y = b;
    return x->start != y->start ? (x->start < y->start ? -1 : 1) : (x->fs != y->fs ? (x->fs ? -1 : 1) : 0);
}

int spirv_compact_varyings(uint32_t *vs, size_t nvs, uint32_t *fs, size_t nfs, uint32_t limit, char *info, size_t ninfo) {
    Range r[MAX_RANGES];
    int nr = 0;
    const char *why = NULL;
    if (info && ninfo) info[0] = 0;
    if (!collect(vs, nvs, SC_OUTPUT, false, r, &nr, &why) || !collect(fs, nfs, SC_INPUT, true, r, &nr, &why)) {
        if (info) snprintf(info, ninfo, "%s", why);
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
    // the fragment shader's units first, then the vertex outputs nothing reads (in old order within each)
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
