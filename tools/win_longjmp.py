"""Windows (x64 lasgen, also under x64 emulation on ARM): unicorn's setjmp / longjmp without VCRUNTIME140's (#337, #779).

unicorn leaves the code it generated with longjmp, from a setjmp its wrapper calls with frame 0 so that longjmp
restores the registers instead of unwinding through generated code, which has no unwind data. VCRUNTIME140's longjmp
doesn't always do that:
- when its Control Flow Guard check pointer isn't its own no-op, it fast-fails (0xC0000409) on frame 0. On x64
  Windows that is when CFG is on (#337: the generator is started with CFG off). Under x64 emulation on ARM, Windows
  may set that pointer with CFG off as well: it is how calls into ARM64EC code are caught (Microsoft's ARM64EC notes).
- when there is a shadow stack (rdsspq gives one), it always unwinds with RtlUnwindEx, which fails in generated code.
On Windows 11 26H1 on ARM every first jump table emulation ended the generator there (#779), even started with CFG
and shadow stacks off; the Windows on ARM CI runner (an older Windows) runs VCRUNTIME140's fine.

apply() points unicorn.dll's setjmp and longjmp imports at SETJMP_X64 / LONGJMP_X64: a plain register save and
restore (what VCRUNTIME140's longjmp does with frame 0 and no checks) that also pops a shadow stack, when there is
one, back to where setjmp was (setjmp keeps it in the frame field, which only LONGJMP_X64 reads)."""
import ctypes
import os
import struct

# x64, rcx = the jmp_buf (_JUMP_BUFFER: Frame, Rbx, Rsp, Rbp, Rsi, Rdi, R12-R15, Rip, MxCsr, FpCsr, Xmm6-15), rdx unused:
#   xor eax, eax / rdsspq rax / test rax, rax / jz 1f / add rax, 8     (the shadow stack after this ret; 0: none)
#   1: mov [rcx], rax / mov [rcx+8], rbx / lea r8, [rsp+8] / mov [rcx+0x10], r8 / mov [rcx+0x18..0x48], rbp rsi rdi
#   r12-r15 / mov r8, [rsp] / mov [rcx+0x50], r8 / stmxcsr [rcx+0x58] / fnstcw [rcx+0x5c]
#   movdqu [rcx+0x60..0xf0], xmm6-xmm15 / xor eax, eax / ret
SETJMP_X64 = bytes.fromhex(
    '31c0f3480f1ec84885c074044883c008488901488959084c8d4424084c8941104889691848897120488979284c8961304c8969384c897140'
    '4c8979484c8b04244c8941500fae5958d9795cf30f7f7160f30f7f7970f3440f7f8180000000f3440f7f8990000000f3440f7f91a0000000'
    'f3440f7f99b0000000f3440f7fa1c0000000f3440f7fa9d0000000f3440f7fb1e0000000f3440f7fb9f000000031c0c3')
# rcx = the jmp_buf, edx = the value:
#   mov r8, [rcx] / test r8, r8 / jz 3f / xor r9d, r9d / rdsspq r9 / test r9, r9 / jz 3f
#   sub r8, r9 / shr r8, 3 / 2: cmp r8, 255 / jbe 4f / mov r9d, 255 / incsspq r9 / sub r8, 255 / jmp 2b / 4: incsspq r8
#   3: mov eax, edx / test eax, eax / jnz 5f / inc eax                  (longjmp(b, 0) returns 1)
#   5: mov rbx, [rcx+8] / mov rsp, [rcx+0x10] / mov rbp..r15, [rcx+0x18..0x48] / ldmxcsr [rcx+0x58] / fnclex
#   fldcw [rcx+0x5c] / movdqu xmm6-xmm15, [rcx+0x60..0xf0] / jmp qword ptr [rcx+0x50]
LONGJMP_X64 = bytes.fromhex(
    '4c8b014d85c074364531c9f3490f1ec94d85c974294d29c849c1e8034981f8ff000000761441b9ff000000f3490faee94981e8ff000000'
    'ebe3f3490faee889d085c07502ffc0488b5908488b6110488b6918488b7120488b79284c8b61304c8b69384c8b71404c8b79480fae5158'
    'dbe2d9695cf30f6f7160f30f6f7970f3440f6f8180000000f3440f6f8990000000f3440f6f91a0000000f3440f6f99b0000000f3440f6f'
    'a1c0000000f3440f6fa9d0000000f3440f6fb1e0000000f3440f6fb9f0000000ff6150')

_stubs = None  # (kept for the life of the process)


def _k32():
    from ctypes import wintypes as w
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.GetCurrentProcess.restype = w.HANDLE
    k.GetCurrentProcess.argtypes = []
    k.GetModuleHandleW.restype = w.HMODULE
    k.GetModuleHandleW.argtypes = [w.LPCWSTR]
    k.VirtualAlloc.restype = ctypes.c_void_p
    k.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, w.DWORD, w.DWORD]
    k.VirtualProtect.restype = w.BOOL
    k.VirtualProtect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, w.DWORD, ctypes.POINTER(w.DWORD)]
    k.FlushInstructionCache.restype = w.BOOL
    k.FlushInstructionCache.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t]
    k.GetProcessMitigationPolicy.restype = w.BOOL
    k.GetProcessMitigationPolicy.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
    if hasattr(k, 'IsWow64Process2'):  # (Windows 10 1709 on)
        k.IsWow64Process2.restype = w.BOOL
        k.IsWow64Process2.argtypes = [w.HANDLE, ctypes.POINTER(w.USHORT), ctypes.POINTER(w.USHORT)]
    return k


def _u32(a):
    return ctypes.c_uint32.from_address(a).value


def _u64(a):
    return ctypes.c_uint64.from_address(a).value


def _machine(base):
    """A loaded module's PE machine (0x8664 x64, 0xAA64 arm64). platform.machine() says ARM64 under emulation."""
    return ctypes.c_uint16.from_address(base + _u32(base + 0x3C) + 4).value


def _iat_slot(base, dll, name):
    """The address of module base's import-table slot for dll!name, or None."""
    opt = base + _u32(base + 0x3C) + 24
    rva = _u32(opt + 112 + 8)  # (data directory 1: imports)
    if not rva:
        return None
    d = base + rva
    while _u32(d + 12):  # IMAGE_IMPORT_DESCRIPTOR: OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk
        if ctypes.string_at(base + _u32(d + 12)).decode('ascii', 'replace').lower() == dll:
            names, slots = base + (_u32(d) or _u32(d + 16)), base + _u32(d + 16)
            i = 0
            while True:
                t = _u64(names + i * 8)
                if not t:
                    break
                if not t >> 63 and ctypes.string_at(base + (t & 0x7FFFFFFF) + 2) == name:
                    return slots + i * 8
                i += 1
        d += 20
    return None


def apply():
    """Point unicorn.dll's setjmp and longjmp at ours (call after `import unicorn`). Returns words for the log."""
    global _stubs
    if os.name != 'nt' or struct.calcsize('P') != 8:
        return ''
    if _stubs:
        return 'own setjmp/longjmp'
    from ctypes import wintypes as w
    k = _k32()
    uc = k.GetModuleHandleW('unicorn.dll')
    if not uc or _machine(uc) != 0x8664:
        return ''
    slots = [_iat_slot(uc, 'vcruntime140.dll', n) for n in (b'__intrinsic_setjmp', b'longjmp')]
    if not all(slots):  # (both or neither: our longjmp reads the frame field our setjmp writes)
        return 'setjmp/longjmp not found'
    mem = k.VirtualAlloc(None, 4096, 0x3000, 0x04)  # (MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    if not mem:
        return f'setjmp/longjmp not replaced (error {ctypes.get_last_error()})'
    ctypes.memmove(mem, SETJMP_X64, len(SETJMP_X64))
    ctypes.memmove(mem + 0x100, LONGJMP_X64, len(LONGJMP_X64))
    old = w.DWORD()
    if not k.VirtualProtect(mem, 4096, 0x20, ctypes.byref(old)):  # PAGE_EXECUTE_READ
        return f'setjmp/longjmp not replaced (error {ctypes.get_last_error()})'
    k.FlushInstructionCache(k.GetCurrentProcess(), mem, 4096)
    if not k.VirtualProtect(min(slots), max(slots) - min(slots) + 8, 0x04, ctypes.byref(old)):
        return f'setjmp/longjmp not replaced (error {ctypes.get_last_error()})'
    ctypes.c_uint64.from_address(slots[0]).value = mem
    ctypes.c_uint64.from_address(slots[1]).value = mem + 0x100
    k.VirtualProtect(min(slots), max(slots) - min(slots) + 8, old.value, ctypes.byref(old))
    _stubs = mem
    return 'own setjmp/longjmp'


def _vcruntime_cfg_check(k):
    """Whether VCRUNTIME140's CFG check pointer is still its own no-op (the check longjmp skips then)."""
    vc = k.GetModuleHandleW('vcruntime140.dll')
    if not vc:
        return None
    opt = vc + _u32(vc + 0x3C) + 24
    size, rva = _u32(opt + 56), _u32(opt + 112 + 10 * 8)  # SizeOfImage, data directory 10: load config
    if not rva or _u32(vc + rva) < 0x78:
        return None
    ptr = _u64(vc + rva + 0x70)  # GuardCFCheckFunctionPointer
    if not ptr:
        return None
    target = _u64(ptr)
    return 'its own' if vc <= target < vc + size else 'set by Windows'


def describe():
    """Windows: the CPU this runs on, and whether CFG and shadow stacks are on in this process, for the log."""
    if os.name != 'nt':
        return ''
    try:
        from ctypes import wintypes as w
        k = _k32()
        me = k.GetCurrentProcess()
        machine = {0x8664: 'x64', 0xAA64: 'arm64'}
        exe = _machine(k.GetModuleHandleW(None))
        this = machine.get(exe, hex(exe))
        proc, native = w.USHORT(), w.USHORT()
        ok = hasattr(k, 'IsWow64Process2') and k.IsWow64Process2(me, ctypes.byref(proc), ctypes.byref(native))
        host = machine.get(native.value, hex(native.value)) if ok else '?'
        out = [f'{this} on {host}' + (' (emulated)' if host not in (this, '?') else '')]
        for name, policy, bit in (('Control Flow Guard', 7, 1), ('shadow stacks', 17, 1),
                                  ('strict shadow stacks', 17, 0x10)):
            flags = w.DWORD()
            if k.GetProcessMitigationPolicy(me, policy, ctypes.byref(flags), ctypes.sizeof(flags)):
                out.append(f'{name} {"on" if flags.value & bit else "off"}')
            else:
                out.append(f'{name} unknown (error {ctypes.get_last_error()})')
        check = _vcruntime_cfg_check(k)
        if check:
            out.append(f'VCRUNTIME140 CFG check {check}')
        return ', '.join(out)
    except Exception as e:  # (only for the log)
        return f'mitigations unknown ({e})'
