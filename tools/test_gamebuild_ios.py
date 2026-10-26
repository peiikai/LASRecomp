"""The iOS game-code build (runtime/gamebuild.c GAMEBUILD_IOS) on this desktop, with tiny generated programs.

The real builder, the bundled zig (build/zig: tools/fetch_zig.sh) and the Mach-O linker of the iOS kit
(build/lld/<arch>-<os>: tools/fetch_lld.sh), packed into a fixture .laskit as tools/ios_kit.sh lays it out, compile
the fixture for arm64 iOS and link lasgame-ios.dylib; the result must be an arm64 Mach-O dylib with the install name
the exported app loads it by. The kit's tools must land in <data root>/cache/ios-kit/<kit id>/ once. Only process
launching and resource lookup are bridged to Python, as tools/test_gamebuild_resume.py does. No game files, no device.
"""
import ctypes as ct
import os
from pathlib import Path
import platform
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
EXE = '.exe' if os.name == 'nt' else ''
HOST = ('aarch64' if platform.machine().lower() in ('arm64', 'aarch64') else 'x86_64') + \
       ('-macos' if sys.platform == 'darwin' else '-windows' if os.name == 'nt' else '-linux')
ZIG = ROOT/'build/zig'/('zig'+EXE)
LLD = ROOT/'build/lld'/HOST/'bin'/('lld'+EXE)
MINIZ = ROOT/'build/miniz'
for p, how in ((ZIG, 'tools/fetch_zig.sh'), (LLD, f'LLD_TARGET={HOST} tools/fetch_lld.sh')):
    assert p.exists(), f'{p} missing ({how})'
if not (MINIZ/'.fetched').exists():
    subprocess.run(['bash', str(ROOT/'tools/fetch_miniz.sh')], check=True)
# (the bridge and the generator of tools/test_gamebuild_resume.py)
BRIDGE = r'''
#include "platform.h"
#include <chrono>
#include <thread>
#include <filesystem>
#include <string>
#include <cstdio>
static std::string root, data;
static intptr_t (*start_cb)(const char *const *, const char *, bool);
static int (*wait_cb)(intptr_t, bool);
static void (*kill_cb)(intptr_t);
extern "C" void test_init(const char *r, intptr_t (*s)(const char *const *,const char *,bool),
                          int (*w)(intptr_t,bool), void (*k)(intptr_t)) { root=r;data=root+"/data";start_cb=s;wait_cb=w;kill_cb=k; }
extern "C" const char *rt_data_root() { return data.c_str(); }
extern "C" bool plat_remove_tree(const char *d) { std::error_code ec; std::filesystem::remove_all(std::filesystem::u8path(d), ec); return !ec; }
extern "C" bool rt_resource(const char *name,const char *dev,char *out,size_t n) {  // (shipped names: <root>/res/)
    for (std::string p : {name ? root+"/res/"+name : std::string(), dev ? root+"/"+dev : std::string()})
        if(!p.empty() && std::filesystem::exists(std::filesystem::u8path(p))) return snprintf(out,n,"%s",p.c_str())<(int)n;
    return false;
}
extern "C" bool rt_dev_mode() { return false; }
extern "C" uint64_t plat_now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
extern "C" void plat_sleep_until_ns(uint64_t t) { auto now=plat_now_ns(); if(t>now)std::this_thread::sleep_for(std::chrono::nanoseconds(t-now)); }
extern "C" intptr_t plat_spawn(const char *const *a,const char *l,bool append) { return start_cb(a,l,append); }
extern "C" int plat_proc_wait(intptr_t p,bool block) { return wait_cb(p,block); }
extern "C" void plat_proc_kill(intptr_t p) { kill_cb(p); }
'''
GENERATOR = r'''
import pathlib,sys
name,source,base,out=sys.argv[1:5]
out=pathlib.Path(out);out.mkdir(parents=True,exist_ok=True)
for i in range(3):
    (out/f'{name}_{i:03}.c').write_text(f'int {name}_{i:03}(void) {{ return {i}; }}\n')
    print(f'{name}: progress {i+1}/3 files {i+1}',file=sys.stderr,flush=True)
(out/f'{name}_table.c').write_text(f'int {name}_table(void) {{ return 3; }}\n')
'''

with tempfile.TemporaryDirectory() as temporary:
    tmp = Path(temporary).resolve()
    resources, game, skeleton = tmp/'resources', tmp/'game', tmp/'skeleton'
    (resources/'runtime/hle').mkdir(parents=True)
    (resources/'tools').mkdir()
    (resources/'data').mkdir()  # (the bridge's data root: the kit's tools go to data/cache/ios-kit/<id>/)
    (game/'exefs').mkdir(parents=True)
    for name in ('cpu.h', 'simd.h', 'simd_fp.h', 'gamelib.h'):
        (resources/'runtime'/name).write_text('/* fixture header */\n')
    (resources/'tools/gamelib_entry.c').write_text('#include <stdlib.h>\nint cache_entry(void) { return abs(-1); }\n')
    (resources/'runtime/hle/hle_symbols.txt').write_text('fixture\n')
    # (an NSO header with a build ID: the library's id is GAMELIB_IOS_ID-<build ID>, runtime/gamelib.h)
    main = b'NSO0' + bytes(0x3c) + bytes.fromhex('0123456789abcdef') + bytes(0x18) + b'fixture main'
    (game/'exefs/main').write_bytes(main)
    (game/'exefs/sdk').write_bytes(b'fixture sdk')
    # the bundled zig, as a release ships it (rt_resource("zig/zig"): the bridge's <resources>/res/)
    (resources/'res').mkdir()
    if os.name == 'nt': shutil.copytree(ZIG.parent, resources/'res/zig')
    else: os.symlink(ZIG.parent, resources/'res/zig')
    # the kit's contents (tools/ios_kit.sh): the app's marker, the Darwin headers (any-darwin-any from the zig archive
    # is what the real kit holds; the headers zig keeps for a macOS host are the same set, a Linux / Windows host's
    # zig has none, so the archive is read), the linker, the tbd
    (skeleton/'Payload/LAS Recomp.app').mkdir(parents=True)
    (skeleton/'Payload/LAS Recomp.app/Info.plist').write_text('<plist/>\n')
    darwin = ZIG.parent/'lib/libc/include/any-darwin-any'
    if not darwin.exists():
        archive = next(p for p in (ROOT/'build/zig-dl').iterdir() if p.name.startswith('zig-') and p.suffix in ('.xz', '.zip'))
        (skeleton/'include').mkdir()
        if archive.suffix == '.zip':
            import zipfile
            with zipfile.ZipFile(archive) as z:
                top = z.namelist()[0].split('/')[0]
                members = [m for m in z.namelist() if m.startswith(f'{top}/lib/libc/include/any-darwin-any/')]
                z.extractall(tmp/'zig-x', members)
            shutil.move(tmp/'zig-x'/top/'lib/libc/include/any-darwin-any', skeleton/'include/any-darwin-any')
        else:
            subprocess.run(['tar', '-xJf', str(archive), '-C', str(skeleton/'include'), '--strip-components', '4',
                            '--wildcards', '*/lib/libc/include/any-darwin-any'], check=True)
    else:
        shutil.copytree(darwin, skeleton/'include/any-darwin-any')
    shutil.copytree(LLD.parent.parent, skeleton/'lld'/HOST)
    shutil.copy(ROOT/'runtime/platform/ios-sdk/libSystem.B.tbd', skeleton/'libSystem.B.tbd')
    assert (skeleton/'include/any-darwin-any/stdlib.h').exists()
    # zipped as the kit (Unix modes kept: the linker must come out executable; symlinks as symlink entries)
    kit = tmp/'fixture-ios.laskit'
    with zipfile.ZipFile(kit, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in sorted(skeleton.rglob('*')):
            rel = p.relative_to(skeleton).as_posix()
            if p.is_symlink():
                info = zipfile.ZipInfo(rel); info.create_system = 3; info.external_attr = 0o120755 << 16
                z.writestr(info, os.readlink(p))
            elif p.is_dir():
                info = zipfile.ZipInfo(rel + '/'); info.create_system = 3; info.external_attr = (0o40755 << 16) | 0x10
                z.writestr(info, b'')
            else:
                info = zipfile.ZipInfo.from_file(p, rel); info.create_system = 3; info.compress_type = zipfile.ZIP_DEFLATED
                with open(p, 'rb') as f: z.writestr(info, f.read())

    generator = tmp/'generator.py'; generator.write_text(GENERATOR)
    os.environ['LARECOMP_GEN'] = str(generator)
    os.environ.pop('LARECOMP_CC', None)
    cc = shlex.split(os.environ.get('CC', 'clang'))
    cxx = shlex.split(os.environ.get('CXX', 'clang++'))
    if os.name == 'nt':  # (find_tools wants the exe's import library; the iOS link doesn't use it)
        (resources/'build').mkdir()
        stub = tmp/'stub.c'; stub.write_text('int fixture_export;\n')
        subprocess.run(cc+[str(stub), '-shared', '-Wl,--out-implib,'+str(resources/'build/lasrecomp.dll.a'),
                           '-o', str(tmp/'stub.dll')], check=True)
    obj = tmp/'gamebuild.o'
    subprocess.run(cc+['-std=gnu11', '-fPIC', '-Iruntime', '-DGAMELIB_ID="fixture"', '-DGAMELIB_IOS_ID="fixture-ios"',
                       '-DGAMELIB_CFLAGS="-O0"', '-DGAMELIB_SHARED="-shared"',
                       '-c', 'runtime/gamebuild.c', '-o', str(obj)], cwd=ROOT, check=True)
    zipobj, miniz = tmp/'zipfile.o', tmp/'miniz.o'
    subprocess.run(cc+['-std=gnu11', '-fPIC', '-Iruntime', '-I'+str(MINIZ), '-c', 'runtime/zipfile.c', '-o', str(zipobj)], cwd=ROOT, check=True)
    subprocess.run(cc+['-fPIC', '-w', '-c', str(MINIZ/'miniz.c'), '-o', str(miniz)], cwd=ROOT, check=True)
    bridge = tmp/'bridge.cpp'; bridge.write_text(BRIDGE)
    libfile = tmp/('scheduler.dll' if os.name == 'nt' else 'scheduler.so')
    subprocess.run(cxx+['-shared', '-fPIC', '-pthread', '-std=c++17', '-Iruntime', '-x', 'c++', str(bridge),
                        'runtime/build_cache.cpp', '-x', 'none', str(obj), str(zipobj), str(miniz), '-o', str(libfile)] +
                   (['-Wl,--export-all-symbols'] if os.name == 'nt' else []), cwd=ROOT, check=True)
    library = ct.CDLL(str(libfile))
    processes = {}
    def unload():  # (Windows: a loaded DLL can't be deleted with the temporary folder)
        for p in processes.values():
            if p.poll() is None: p.kill()
            p.wait()
        if os.name == 'nt':
            import _ctypes
            _ctypes.FreeLibrary(library._handle)
    SPAWN = ct.CFUNCTYPE(ct.c_ssize_t, ct.POINTER(ct.c_char_p), ct.c_char_p, ct.c_bool)
    WAIT = ct.CFUNCTYPE(ct.c_int, ct.c_ssize_t, ct.c_bool)
    KILL = ct.CFUNCTYPE(None, ct.c_ssize_t)
    PROGRESS = ct.CFUNCTYPE(None, ct.c_double, ct.c_char_p)
    CANCEL = ct.CFUNCTYPE(ct.c_bool)
    class Options(ct.Structure):
        _fields_ = [('progress', PROGRESS), ('jobs', ct.c_int), ('cancelled', CANCEL),
                    ('target', ct.c_int), ('ios_kit', ct.c_char_p)]
    launched = []; next_id = 0
    @SPAWN
    def spawn(args, log, append):
        global next_id
        command = []; i = 0
        while args[i]: command.append(args[i].decode()); i += 1
        launched.append(command)
        if command[0] == str(generator): command = [sys.executable, *command]
        stream = open(log.decode(), 'ab' if append else 'wb')
        try:
            p = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
        finally: stream.close()
        next_id += 1; processes[next_id] = p; return next_id
    @WAIT
    def wait(pid, block):
        p = processes[pid]; result = p.wait() if block else p.poll()
        if result is None: return -1
        del processes[pid]; return result
    @KILL
    def kill(pid):
        p = processes.pop(pid)
        if p.poll() is None: p.kill()
        p.wait()
    @PROGRESS
    def progress(fraction, step): pass
    @CANCEL
    def cancelled(): return False
    library.test_init.argtypes = [ct.c_char_p, SPAWN, WAIT, KILL]
    library.test_init(str(resources).encode(), spawn, wait, kill)
    library.gamebuild_ios_available.argtypes = [ct.c_char_p, ct.c_void_p, ct.c_size_t]
    library.gamebuild_ios_available.restype = ct.c_bool
    library.gamebuild_run.argtypes = [ct.c_char_p, ct.POINTER(Options), ct.c_void_p, ct.c_size_t]
    library.gamebuild_run.restype = ct.c_bool
    why = ct.create_string_buffer(1024)
    options = Options(progress, 2, cancelled, 1, str(kit).encode())
    try:
        assert not library.gamebuild_ios_available(str(tmp/'nowhere').encode(), why, len(why)) and b'kit' in why.value, why.value
        # an unpacked folder is refused with a hint, a zip that isn't a kit too
        assert not library.gamebuild_ios_available(str(skeleton).encode(), why, len(why)) and b'not an unpacked folder' in why.value, why.value
        other = tmp/'other.zip'
        with zipfile.ZipFile(other, 'w') as z: z.writestr('hello.txt', 'hi')
        assert not library.gamebuild_ios_available(str(other).encode(), why, len(why)) and b"isn't an iOS kit" in why.value, why.value
        assert library.gamebuild_ios_available(str(kit).encode(), why, len(why)), why.value
        assert not list((resources/'data').iterdir()), 'the availability check unpacked something'
        assert library.gamebuild_run(str(game).encode(), ct.byref(options), why, len(why)), (why.value, [p.read_text() for p in (game/'.gamebuild').glob('*.log')])
        kits = list((resources/'data/cache/ios-kit').iterdir())
        assert len(kits) == 1 and (kits[0]/'.complete').exists() and (kits[0]/'libSystem.B.tbd').exists(), kits
        kit_id = kits[0].name  # (the runtime's paths use the host's separators: compare with '/' throughout)
        def joined(a): return ' '.join(a).replace('\\', '/')
        compiles = [a for a in launched if '-c' in a]
        assert compiles and all('aarch64-ios' in joined(a) and f'ios-kit/{kit_id}/include' in joined(a) for a in compiles), compiles[0]
        link = next(a for a in launched if a[0].endswith('lld'+EXE))
        assert f'ios-kit/{kit_id}/lld' in joined(link[:1]) and link[1:3] == ['-flavor', 'darwin'] and '@rpath/LASGame.framework/LASGame' in link, link
        out = game/'lasgame-ios.dylib'
        head = out.read_bytes()[:8]
        magic, cputype = struct.unpack('<II', head)
        assert magic == 0xFEEDFACF and cputype == 0x0100000C, f'not an arm64 Mach-O: {head.hex()}'  # (CPU_TYPE_ARM64)
        assert (game/'lasgame-ios.dylib.id').read_text().strip() == 'fixture-ios-0123456789ABCDEF'
        # a second build finds the tools unpacked already: nothing is unpacked twice
        marker = kits[0]/'.complete'; before = marker.stat().st_mtime_ns
        assert library.gamebuild_run(str(game).encode(), ct.byref(options), why, len(why)), why.value
        assert marker.stat().st_mtime_ns == before, 'the kit was unpacked again'
        print(f'iOS game-code build on {HOST}: compile for arm64 iOS with the kit\'s headers, link with its lld, '
              f'{out.stat().st_size} byte dylib passed')
    finally:
        unload()
