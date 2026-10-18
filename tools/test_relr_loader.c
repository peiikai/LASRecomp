// Exercise the production loader with synthetic dynamic tables, without game files.
#include <assert.h>
#include <setjmp.h>
#include "../runtime/loader.c"

static jmp_buf fatal_jump;
static bool expect_fatal;
static _Alignas(16) uint8_t image[4096], before[4096];
static uint64_t *dynamic = (uint64_t *)(image + 0x100);

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; abort(); }
HostFn hle_lookup_impl(const char *name) { (void)name; return NULL; }
uint64_t hle_register(const char *name, HostFn fn) { (void)name; (void)fn; abort(); }
void rt_fatal_msg(const char *fmt, ...) { assert(strstr(fmt, "packed relative relocations")); }
void rt_fatal_trace(void) {
    assert(expect_fatal);
    longjmp(fatal_jump, 1);
}

static void put(size_t offset, uint64_t value) { memcpy(image + offset, &value, 8); }
static uint64_t get(size_t offset) { uint64_t value; memcpy(&value, image + offset, 8); return value; }

static Module fixture(void) {
    memset(image, 0, sizeof image);
    const uint64_t tags[] = {
        DT_RELR, 0x200, DT_RELRSZ, 32, DT_RELRENT, 8,
        DT_RELA, 0x300, DT_RELASZ, sizeof(Elf64Rela),
        DT_JMPREL, 0x318, DT_PLTRELSZ, sizeof(Elf64Rela), DT_NULL, 0
    };
    memcpy(dynamic, tags, sizeof tags);
    const uint64_t entries[] = {0x400, UINT64_C(0x8000000000000003), 5, 0x802};
    memcpy(image + 0x200, entries, sizeof entries);
    const size_t sites[] = {0x400, 0x408, 0x5f8, 0x608, 0x802};
    for (unsigned i = 0; i < sizeof sites / sizeof sites[0]; i++) put(sites[i], i + 1);
    put(0x802, UINT64_MAX - 7);  // unaligned even address and a negative implicit addend
    const Elf64Rela rela[] = {{0xa00, R_RELATIVE, 0x123}, {0xa08, R_RELATIVE, 0x456}};
    memcpy(image + 0x300, rela, sizeof rela);
    return (Module){.name = "synthetic", .base = (uintptr_t)image,
                    .size = sizeof image, .dynamic = (uintptr_t)dynamic};
}

static void reject_unchanged(Module *module) {
    memcpy(before, image, sizeof image);
    expect_fatal = true;
    if (!setjmp(fatal_jump)) {
        relocate(module);
        assert(!"invalid RELR metadata was accepted");
    }
    expect_fatal = false;
    assert(!memcmp(before, image, sizeof image));
}

int main(void) {
    Module m = fixture();
    relocate(&m);
    assert(get(0x400) == m.base + 1 && get(0x408) == m.base + 2);
    assert(get(0x5f8) == m.base + 3 && get(0x608) == m.base + 4);
    assert(get(0x802) == m.base - 8);
    assert(get(0xa00) == m.base + 0x123 && get(0xa08) == m.base + 0x456);

    // Existing RELA / JMPREL modules and an explicitly empty RELR table still work.
    m = fixture(); m.dynamic += 6 * sizeof(uint64_t);
    relocate(&m);
    assert(get(0x400) == 1 && get(0xa00) == m.base + 0x123 && get(0xa08) == m.base + 0x456);
    m = fixture(); dynamic[3] = 0;
    relocate(&m);
    assert(get(0x400) == 1 && get(0xa00) == m.base + 0x123);

    // Every RELR tag is mandatory; rejecting must precede any relocation writes.
    for (unsigned i = 0; i < 3; i++) {
        m = fixture(); dynamic[2 * i] = 1; reject_unchanged(&m);
    }
    m = fixture(); dynamic[5] = 4; reject_unchanged(&m);
    m = fixture(); dynamic[3] = 31; reject_unchanged(&m);
    m = fixture(); dynamic[1] = sizeof image; reject_unchanged(&m);
    m = fixture(); dynamic[1] = 0x202; reject_unchanged(&m);
    m = fixture(); put(0x200, 3); reject_unchanged(&m);  // bitmap before address
    m = fixture(); put(0x218, sizeof image - 6); reject_unchanged(&m);
    m = fixture(); put(0x218, 0x200); reject_unchanged(&m);  // target aliases the table
    m = fixture(); put(0x218, UINT64_MAX - 7); reject_unchanged(&m);
    puts("PASS: loader RELR/RELA/JMPREL integration and rejection before mutation");
    return 0;
}
