// The jump-table evaluator (emu.cpp).
#pragma once

#include "gen_internal.hpp"

#include <unordered_set>

namespace lasgen {

// An access outside the mapped memory, an instruction the evaluator does not run, or an unknown value that would
// decide an address or a branch (gen.py: unicorn.UcError).
struct EmuFault {};

class Emu {
  public:
    static constexpr uint64_t SCRATCH_MEM = 0x7300000000ull;  // gen.py SCRATCH_MEM
    static constexpr size_t SCRATCH_SIZE = 0x100000;

    Emu(const Nso &nso, uint64_t base);
    // Run the instruction at `at`; pc is then the next one. Throws EmuFault.
    void step(uint64_t at);

    void set_x(int r, uint64_t v) { x[r] = v, xk[r] = true; }
    void set_sp(uint64_t v) { sp = v, spk = true; }
    // Register r's value; false if it is unknown.
    bool get_x(int r, uint64_t &v) const {
        v = x[r];
        return xk[r];
    }
    uint64_t pc = 0;

  private:
    // Values the evaluator does not compute (FP arithmetic, values derived from them, memory written with them) are
    // unknown. Unknown values flow through registers and memory like any other; only where one would decide an
    // address, a branch or the table's target does the table stop, so it never gives a wrong target.
    uint64_t x[31] = {};
    bool xk[31];
    uint64_t sp = 0, nzcv = 0;
    bool spk = true, flags_known = true;
    unsigned __int128 v[32] = {};
    bool vknown[32];
    std::unordered_set<uint64_t> tainted;  // bytes of memory holding an unknown value
    bool unk = false;                      // the instruction being run read an unknown value

    uint64_t base;
    std::vector<uint8_t> image;    // the module's memory (writable, as mapped in gen.py)
    std::vector<uint8_t> scratch;  // SCRATCH_MEM
    bool debug = false;

    uint8_t *mem(uint64_t a, size_t n);
    void need_known(const char *what);
    uint64_t load(uint64_t a, int size, bool &known);
    void store(uint64_t a, int size, uint64_t v, bool known);
    bool is_tainted(uint64_t a, int size) const;
    void taint(uint64_t a, int size, bool unknown);
    void vload(int r, uint64_t a, int size);
    void vstore(int r, uint64_t a, int size);
    void simd_ldst(uint32_t w);
    void fp_simd(uint32_t w);
    uint64_t xr(int r);
    uint64_t xsp(int r);
    void wx(int r, uint64_t v) { wxk(r, v, !unk); }
    void wxsp(int r, uint64_t v) { wxspk(r, v, !unk); }
    void wxk(int r, uint64_t v, bool known);
    void wxspk(int r, uint64_t v, bool known);
    void set_flags(uint64_t f) { nzcv = f, flags_known = !unk; }
    uint64_t add_with_carry(int sf, uint64_t a, uint64_t b, int carry, bool flags);
    void logic_flags(int sf, uint64_t r);
    bool cond_holds(int cond);
    [[noreturn]] void unsupported(uint32_t w);
};

}  // namespace lasgen
