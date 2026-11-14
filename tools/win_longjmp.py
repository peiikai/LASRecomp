"""Windows (x64 lasgen, also under x64 emulation on ARM): unicorn's longjmp without VCRUNTIME140's checks (#337, #779).

unicorn leaves the code it generated with longjmp from a setjmp that saved no frame (its setjmp wrapper passes 0, so
longjmp restores the registers instead of unwinding through generated code, which has no unwind data). VCRUNTIME140's
longjmp first checks the target when its Control Flow Guard check pointer isn't its own no-op, and fast-fails
(0xC0000409) on a frame of 0. On x64 Windows that pointer is set only when CFG is on (#337: the generator is started
with CFG off). Under x64 emulation on Windows on ARM, Windows sets it for x64 code even with CFG off: it is how calls
into ARM64EC code are caught (Microsoft's ARM64EC ABI notes). So every first jump table emulation ended the process
there (#779).

apply() points unicorn.dll's longjmp import at a plain register restore: what VCRUNTIME140's longjmp does after that
check, with no frame and no shadow stack (the runtime also starts the generator with shadow stacks off,
runtime/platform/windows.c spawn_child)."""
import ctypes
import os
import platform
import struct

# x64, rcx = the jmp_buf (_JUMP_BUFFER), edx = the value:
#   mov eax, edx / test eax, eax / jnz 1f / inc eax          (longjmp(b, 0) returns 1)
#   1: mov rbx, [rcx+8] / mov rsp, [rcx+0x10] / mov rbp, [rcx+0x18] / mov rsi, [rcx+0x20] / mov rdi, [rcx+0x28]
#   mov r12..r15, [rcx+0x30..0x48] / ldmxcsr [rcx+0x58] / fnclex / fldcw [rcx+0x5c]
#   movdqu xmm6..xmm15, [rcx+0x60..0xf0] / jmp qword ptr [rcx+0x50]
LONGJMP_X64 = bytes.fromhex(
    '89d085c07502ffc0'
    '488b5908488b6110488b6918488b7120488b79284c8b61304c8b69384c8b71404c8b7948'
    '0fae5158dbe2d9695c'
    'f30f6f7160f30f6f7970'
    'f3440f6f8180000000f3440f6f8990000000f3440f6f91a0000000f3440f6f99b0000000'
    'f3440f6fa1c0000000f3440f6fa9d0000000f3440f6fb1e0000000f3440f6fb9f0000000'
    'ff6150')

_stub = None  # (kept for the life of the process)


def _x64_windows():
    return os.name == 'nt' and struct.calcsize('P') == 8 and platform.machine().lower() in ('amd64', 'x86_64')


def _k32():
    from ctypes import wintypes as w
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.GetCurrentProcess.restype = w.HANDLE
    k.GetCurrentProcess.argtypes = []
    k.GetModuleHandleW.restype = w.HMODULE
    k.GetModuleHandleW.argtypes = [w.LPCWSTR]
    k.GetProcAddress.restype = ctypes.c_void_p
    k.GetProcAddress.argtypes = [w.HMODULE, w.LPCSTR]
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
    """Point unicorn.dll's longjmp at LONGJMP_X64 (call after `import unicorn`). Returns a word for the log."""
    global _stub
    if not _x64_windows():
        return ''
    if _stub:
        return 'own longjmp'
    k = _k32()
    uc = k.GetModuleHandleW('unicorn.dll')
    slot = _iat_slot(uc, 'vcruntime140.dll', b'longjmp') if uc else None
    if not slot:
        return 'longjmp not found'
    mem = k.VirtualAlloc(None, 4096, 0x3000, 0x04)  # (MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    if not mem:
        return f'longjmp not replaced (error {ctypes.get_last_error()})'
    ctypes.memmove(mem, LONGJMP_X64, len(LONGJMP_X64))
    from ctypes import wintypes as w
    old = w.DWORD()
    if not k.VirtualProtect(mem, 4096, 0x20, ctypes.byref(old)):  # PAGE_EXECUTE_READ
        return f'longjmp not replaced (error {ctypes.get_last_error()})'
    k.FlushInstructionCache(k.GetCurrentProcess(), mem, len(LONGJMP_X64))
    if not k.VirtualProtect(slot, 8, 0x04, ctypes.byref(old)):
        return f'longjmp not replaced (error {ctypes.get_last_error()})'
    ctypes.c_uint64.from_address(slot).value = mem
    k.VirtualProtect(slot, 8, old.value, ctypes.byref(old))
    _stub = mem
    return 'own longjmp'


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
        this = 'x64' if _x64_windows() else platform.machine().lower()
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
