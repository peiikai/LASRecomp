// Game code generator internals (port of tools/gen.py, lifter.py, simd_c.py, patches.py, nso.py).
//
// The output must stay byte-identical to the Python generator, which stays in tools/ as the reference until this
// port has shipped (tools/test_gen_cpp.py runs both and compares). Keep the structure close to the Python so the two
// can be read side by side: each function here names the Python function it ports.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lasgen {

// lifter.Unsupported: an encoding the lifter does not handle (the instruction becomes a trap)
struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Anything else that stops the generator (bad input, a patch whose instruction does not match).
struct GenError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Cpu struct offsets (must match runtime/cpu.h)
constexpr int OFF_X = 0, OFF_SP = 256, OFF_PC = 264, OFF_NZCV = 272, OFF_V = 512;

extern const char *const COND_NAMES[16];
extern const int SCRATCH[9];

// lifter.CANON / WINDOW / DYN_BASE (--dyn-base). Per thread: setup runs the sdk and main generators side by side.
constexpr uint64_t CANON = 0x7000000000ull;
constexpr uint64_t WINDOW = 0x320000000ull;
extern thread_local bool DYN_BASE;

inline uint64_t bits(uint64_t w, int lo, int n) { return (w >> lo) & (n >= 64 ? ~0ull : ((1ull << n) - 1)); }
inline int64_t sext(uint64_t v, int n) {
    return (v & (1ull << (n - 1))) ? (int64_t)v - (int64_t)(1ull << n) : (int64_t)v;
}
inline uint64_t ones(int n) { return n >= 64 ? ~0ull : ((1ull << n) - 1); }

// ---------------------------------------------------------------- text helpers (Python f-string pieces)

inline std::string D(int64_t v) { return std::to_string(v); }  // {v} of a Python int
std::string hexs(uint64_t v);                                    // f'{v:x}'
std::string hx(uint64_t v);                                      // lifter.hx: 0x..ull
std::string hx_signed(int64_t v);                                // hx() of a value that may be negative
std::string gaddr(uint64_t v);                                   // lifter.gaddr
std::string hex08(uint32_t v);                                   // f'{v:08x}'

namespace detail {
inline void app(std::string &s, const std::string &v) { s += v; }
inline void app(std::string &s, const char *v) { s += v; }
inline void app(std::string &s, char v) { s += v; }
inline void app(std::string &s, int v) { s += std::to_string(v); }
inline void app(std::string &s, unsigned v) { s += std::to_string(v); }
inline void app(std::string &s, long v) { s += std::to_string(v); }
inline void app(std::string &s, unsigned long v) { s += std::to_string(v); }
inline void app(std::string &s, long long v) { s += std::to_string(v); }
inline void app(std::string &s, unsigned long long v) { s += std::to_string(v); }
}  // namespace detail

// Concatenate strings and integers (integers in decimal, as in an f-string).
template <class... A> std::string cat(const A &...a) {
    std::string s;
    (detail::app(s, a), ...);
    return s;
}

std::string join(const std::vector<std::string> &v, const std::string &sep);

// ---------------------------------------------------------------- register helpers (lifter.py)

std::string X(int r);
std::string XS(int r);
std::string W(int r);
std::string WS(int r);
std::string R(int sf, int r, bool sp = false);
std::string set_reg(int sf, int r, const std::string &expr, bool sp = false);
std::string shift_expr(int sf, const std::string &val, int typ, int amt);
std::string extend_expr(int r, int option, int shift);
std::string cond_expr(int cond);

// ---------------------------------------------------------------- lifted instructions

constexpr int GPR_SP = 32;  // lifter AsmInsn.gprs: 'sp'

// lifter.AsmInsn: an FP/SIMD instruction passed through as inline asm
struct AsmInsn {
    uint32_t word = 0;
    std::set<int> vregs;
    std::vector<std::pair<int, int>> gprs;          // (bit position, guest reg or GPR_SP)
    bool flags = false;
    std::vector<std::pair<int, uint64_t>> consts;   // (bit position, constant) loaded into scratch
};

enum class Kind { op, asm_, b, bcond, bl, blr, br, ret, trap };

// lifter.Insn. target is an int (b/bcond/bl: target_int) or a register expression (blr/br: target_expr).
struct Insn {
    Kind kind = Kind::op;
    std::string code;
    bool has_int_target = false;
    uint64_t target = 0;
    std::string target_expr;
    std::string cond;
    std::shared_ptr<AsmInsn> asm_;

    static Insn op(std::string code) {
        Insn i;
        i.kind = Kind::op;
        i.code = std::move(code);
        return i;
    }
    static Insn trap(std::string code) {
        Insn i;
        i.kind = Kind::trap;
        i.code = std::move(code);
        return i;
    }
    static Insn jump(Kind k, uint64_t t, std::string cond = {}) {
        Insn i;
        i.kind = k;
        i.has_int_target = true;
        i.target = t;
        i.cond = std::move(cond);
        return i;
    }
    static Insn reg(Kind k, std::string expr) {
        Insn i;
        i.kind = k;
        i.target_expr = std::move(expr);
        return i;
    }
    static Insn as(AsmInsn a) {
        Insn i;
        i.kind = Kind::asm_;
        i.asm_ = std::make_shared<AsmInsn>(std::move(a));
        return i;
    }
    // C for the target, as Python's f'{ins.target}' prints it
    std::string target_text() const { return has_int_target ? D((int64_t)target) : target_expr; }
};

std::string emit_asm_batch(const std::vector<const AsmInsn *> &batch);

// simd_c.SimdC: FP/SIMD lowered to portable C (runtime/simd.h)
class SimdC {
  public:
    std::string dp(uint32_t w, uint64_t pc);
    std::string ldst(uint32_t w, uint64_t pc);
    static std::string crc(uint32_t w);

  private:
    std::string crypto_aes(uint32_t w);
    std::string crypto_sha3(uint32_t w);
    std::string crypto_sha2(uint32_t w);
    std::string scalar_copy(uint32_t w);
    std::string copy(uint32_t w);
    std::string permute(uint32_t w);
    std::string ext(uint32_t w);
    std::string tbl(uint32_t w);
    std::string three_same(uint32_t w, bool scalar = false);
    std::string three_same_scalar(int U, int sz, int opc, int d, int n, int m);
    std::string three_same_fp(uint32_t w, bool scalar);
    std::string three_diff(uint32_t w, bool scalar = false);
    std::string misc2(uint32_t w, bool scalar = false);
    std::string misc2_scalar(int U, int sz, int opc, int d, int n);
    std::string misc2_fp(uint32_t w, bool scalar);
    std::string across(uint32_t w);
    std::string scalar_pairwise(uint32_t w);
    std::string modimm(uint32_t w);
    std::string shift_imm(uint32_t w, bool scalar = false);
    std::string indexed(uint32_t w, bool scalar = false);
    std::string fp_dp3(uint32_t w);
    std::string fp_dp2(uint32_t w);
    std::string fp_dp1(uint32_t w);
    std::string fp_cmp(uint32_t w);
    std::string fp_ccmp(uint32_t w);
    std::string fp_csel(uint32_t w);
    std::string fp_imm(uint32_t w);
    std::string fp_fixed(uint32_t w);
    std::string fp_int(uint32_t w);
    std::string ldst_pair(uint32_t w);
    std::string ldst_reg(uint32_t w);
    std::string ldst_struct(uint32_t w);
};

// lifter.Lifter
class Lifter {
  public:
    Lifter(uint64_t base, const std::string &simd);
    Insn lift(uint32_t w, uint64_t pc);
    bool simd_c() const { return sc != nullptr; }

  private:
    [[maybe_unused]] uint64_t base;  // (the module's load address; lifter.py keeps it too)
    std::unique_ptr<SimdC> sc;
    Insn dp_imm(uint32_t w, uint64_t pc);
    Insn branch_sys(uint32_t w, uint64_t pc);
    Insn system(uint32_t w, uint64_t pc);
    Insn ldst(uint32_t w, uint64_t pc);
    Insn ldst_excl(uint32_t w, uint64_t pc);
    Insn ldst_pair(uint32_t w);
    Insn ldst_reg(uint32_t w);
    Insn ldst_simd(uint32_t w, uint64_t pc);
    Insn dp_reg(uint32_t w, uint64_t pc);
    Insn fp_simd(uint32_t w, uint64_t pc);
};

// ---------------------------------------------------------------- NSO (nso.py)

constexpr uint32_t R_AARCH64_RELATIVE = 1027;

struct NsoSym {
    std::string name;
    uint8_t info;
    uint16_t shndx;
    uint64_t value, size;
};

struct NsoReloc {
    uint64_t off;
    uint32_t type, sym;
    int64_t add;
};

class Nso {
  public:
    explicit Nso(const std::string &path);
    std::string build_id;  // hex, trailing zero bytes stripped
    std::pair<uint64_t, uint64_t> text, rodata, data;
    uint64_t bss_size = 0, size = 0;
    std::vector<uint8_t> image;
    uint64_t dynamic = 0, eh_start = 0, eh_end = 0;
    std::vector<NsoSym> syms;
    std::vector<NsoReloc> relocs;
    uint32_t u32(uint64_t off) const;
    uint64_t u64(uint64_t off) const;
    std::string cstr(uint64_t off) const;
};

// Decode one LZ4 block (nso.py: lz4.block.decompress). Throws GenError on malformed input.
std::vector<uint8_t> lz4_block_decompress(const uint8_t *src, size_t n, size_t out_size);

// analyze.eh_frame_functions
std::vector<int64_t> eh_frame_functions(const Nso &n);

// ---------------------------------------------------------------- game versions (game_addresses.py)

struct GameVersion {
    const char *key, *label, *build_id;
};
extern const GameVersion GAME_VERSIONS[];
extern const int GAME_VERSION_COUNT;

// The address table of one version (patches_<key>.h): name -> address
using AddressTable = std::map<std::string, uint64_t>;
const AddressTable &address_table(const std::string &key);

// game_addresses.version_for_build_id; throws GenError (the message the app shows) for an unknown build ID
std::string version_for_build_id(const std::string &build_id);

// game_addresses.addr_text: @NAME@ -> hex digits
std::string addr_text(const AddressTable &a, const std::string &code);

// ---------------------------------------------------------------- patches (patches.py)

class Module;
using PatchFn = std::string (*)(Module &m, uint64_t a);

struct Hook {
    uint32_t word;
    std::vector<uint64_t> targets;
};

struct PatchSet {
    std::map<uint64_t, PatchFn> patches;
    std::map<uint64_t, Hook> hooks;
};

// patches.PATCHES / HOOKS for the selected version (the address table is bound per thread while building them)
PatchSet build_patches(const std::string &version_key);

// The address table the patch functions read (patches.py `A`), set by the generator for the module being built
extern thread_local const AddressTable *PATCH_ADDRESSES;

// ---------------------------------------------------------------- the module being generated (gen.Module)

class Module {
  public:
    Module(const std::string &name, const std::string &path, uint64_t base, const std::string &simd);
    ~Module();
    std::string name;
    Nso nso;
    uint64_t base, lo, hi;
    std::vector<uint32_t> words;
    Lifter lifter;

    // struct.unpack_from('<I', gen.nso.image, a - gen.base)[0] (what patches.py reads)
    uint32_t image_word(uint64_t a) const { return nso.u32(a - base); }
    bool in_text(uint64_t a) const { return lo <= a && a < hi && a % 4 == 0; }
    uint32_t word(uint64_t a) const { return words[(a - lo) / 4]; }
    const Insn &lift(uint64_t a);

    // (the rest of gen.Module lives in module.cpp)
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace lasgen
