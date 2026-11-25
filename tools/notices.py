#!/usr/bin/env python3
"""Third-party notices of the release packages (#868).

The Windows .zip, the macOS .dmg (and the app inside it), the Linux AppImages and tarballs, the Android .apk and the
iOS kit (and the app inside it) carry libraries, fonts and a compiler that are not ours. Their licenses ask for their notices and license texts to
travel with the binaries: THIRD-PARTY-NOTICES.txt at the root of each package holds them all.

  tools/notices.py fetch [id ...]  download each component's license text(s) at the version shipped into
                                   licenses/<id>.txt (after a version bump in a build script: update COMPONENTS first)
  tools/notices.py build           write THIRD-PARTY-NOTICES.txt from COMPONENTS and licenses/
  tools/notices.py check           licenses/ and THIRD-PARTY-NOTICES.txt match COMPONENTS and the repository (CI)
  tools/notices.py package <file>  the notices file is in a built package, unchanged: a .zip, .tar.gz, .dmg,
                                   .AppImage, .apk, .laskit, .ipa, a .app or an unpacked folder (the release workflow
                                   runs it on every package)

COMPONENTS is the list of what the packages carry, with the version pinned in the build scripts (tools/build_deps_*.sh,
tools/linux/, android/deps/, tools/fetch_*.sh). Texts come from each project's own source at
that version; repo: entries are copies of files already in third_party/.
"""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
LICENSES = os.path.join(ROOT, 'licenses')
NOTICES = os.path.join(ROOT, 'THIRD-PARTY-NOTICES.txt')
CACHE = os.path.join(ROOT, 'build', 'notices-cache')
NAME = 'THIRD-PARTY-NOTICES.txt'

W, M, L, A, I = 'Windows', 'macOS', 'Linux', 'Android', 'iOS'
DESKTOP = (W, M, L)
ALL = (W, M, L, A, I)
VULKAN = (W, L, A)


def gh(repo, ref, path):
    return f'https://raw.githubusercontent.com/{repo}/{ref}/{path}'


def cb(repo, ref, path):
    return f'https://codeberg.org/{repo}/raw/tag/{ref}/{path}'


SHADERC_DEPS = {  # (shaderc's DEPS file at each tag: the glslang / SPIRV-Tools / SPIRV-Headers built into it)
    '2026.3': ('168d452a4f460d24b588fed08477a81c44ee27a1', 'b707790a898e44038547df54580022fc1cf89c3d',
               '29981f65241605e08b0ede4cfeb999fe3b723c6a'),
    '2026.4': ('e1b562a8bed273a02f30b59b66a5d499793cede5', 'ef96ed763b43b59b33b31b362f09a02b729fa1c9',
               '04fd3caa1e8267e4d95c806cad901181728e1006'),
}

# id, name, version, license, source, packages, where / notes, texts: [(label, source)]. A source is a URL, a
# URL!member (a file inside that archive, wheel or zip), or repo:<path>.
COMPONENTS = [
    dict(id='ffmpeg', name='FFmpeg', version='9.0.2', license='LGPL 2.1 or later (LGPL configuration: no GPL parts)',
         source='https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz '
                '(sha256 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e)',
         packages=ALL,
         notes='Plays the game\'s movies. Shared libraries: avcodec, avformat, avutil, swscale (Windows: *.dll next to '
               'lasrecomp.exe; macOS: ReAwakening.app/Contents/Frameworks; Linux: usr/lib in the AppImage; Android: '
               'lib/arm64-v8a in the .apk). iOS: linked into the app; `make deps-ios` and `make ipa` in LAS Recomp\'s '
               'repository relink it from the source above. Built unmodified from the source above with only the decoders the game '
               'uses; the exact configure lines are in tools/build_deps_windows.sh, tools/build_deps_macos.sh, '
               'tools/linux/build_deps.sh, android/deps/build_deps.sh and tools/build_deps_ios.sh in LAS Recomp\'s repository '
               '(https://github.com/peiikai/LASRecomp), and each library reports its own with '
               'avcodec_configuration(). You may replace these libraries with your own build of FFmpeg 9.0.x '
               'built the same way.',
         texts=[('LICENSE.md', gh('FFmpeg/FFmpeg', 'n9.0.2', 'LICENSE.md')),
                ('COPYING.LGPLv2.1', gh('FFmpeg/FFmpeg', 'n9.0.2', 'COPYING.LGPLv2.1'))]),
    dict(id='sdl3', name='SDL 3', version='3.4.18 (Windows, macOS, iOS), 3.4.16 (Linux, Android)', license='zlib',
         source='https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18, '
                'https://github.com/libsdl-org/SDL/releases/tag/release-3.4.16',
         packages=ALL,
         texts=[('LICENSE.txt (3.4.18)', gh('libsdl-org/SDL', 'release-3.4.18', 'LICENSE.txt')),
                ('LICENSE.txt (3.4.16)', gh('libsdl-org/SDL', 'release-3.4.16', 'LICENSE.txt'))]),
    dict(id='imgui', name='Dear ImGui (with stb_truetype, stb_rect_pack, stb_textedit)', version='1.91.9',
         license='MIT; stb: MIT or public domain', source='https://github.com/ocornut/imgui', packages=ALL,
         notes='Built into the executable (third_party/imgui).',
         texts=[('LICENSE.txt', 'repo:third_party/imgui/LICENSE.txt'),
                ('stb license (end of imstb_truetype.h)', 'repo-tail:third_party/imgui/imstb_truetype.h:'
                                                          'This software is available under 2 licenses')]),
    dict(id='smaa', name='SMAA', version='2013 (repository master)', license='MIT',
         source='https://github.com/iryoku/smaa', packages=ALL,
         notes='Shader and lookup textures, built into the executable (third_party/smaa).',
         texts=[('LICENSE.txt', 'repo:third_party/smaa/LICENSE.txt')]),
    dict(id='ryujinx', name='Ryujinx (ported audio renderer and shader structuring code)', version='-',
         license='MIT', source='https://github.com/Ryujinx/Ryujinx', packages=ALL,
         notes='Ported code, built into the executable (third_party/ryujinx).',
         texts=[('LICENSE.txt', 'repo:third_party/ryujinx/LICENSE.txt')]),
    dict(id='las-randomizer', name='LAS-Randomizer', version='as in third_party/LAS-Randomizer', license='MIT',
         source='https://github.com/Owen-Splat/LAS-Randomizer', packages=ALL,
         notes='The randomizer, ported to C++ (runtime/rando/) and built into the executable.',
         texts=[('LICENSE.txt', 'repo:third_party/LAS-Randomizer/LICENSE.txt')]),
    dict(id='kenney', name='Kenney Input Prompts (D-pad art)', version='-', license='CC0 1.0',
         source='https://www.kenney.nl', packages=ALL, notes='Built into the executable (third_party/kenney).',
         texts=[('LICENSE.txt', 'repo:third_party/kenney/LICENSE.txt')]),
    dict(id='baloo2', name='Baloo 2 font (SemiBold)', version='1.640', license='SIL Open Font License 1.1',
         source='https://github.com/EkType/Baloo2', packages=ALL,
         texts=[('OFL.txt', 'repo:third_party/fonts/Baloo2/OFL.txt')]),
    dict(id='hylia-serif', name='Hylia Serif font (Beta)', version='0.009', license='Free, not for profit (no license file)',
         source='https://artsyomni.com/hyliaserif', packages=ALL, notes='The start screen title. By Omni Jacala.',
         texts=[('README.md', 'repo:third_party/fonts/HyliaSerif/README.md')]),
    dict(id='biz-udpgothic', name='BIZ UDPGothic font (Regular)', version='1.05 (Google Fonts)',
         license='SIL Open Font License 1.1', source='https://github.com/googlefonts/morisawa-biz-ud-gothic',
         packages=(L, A, I), texts=[('OFL.txt', 'repo:third_party/fonts/BIZUDPGothic/OFL.txt')]),
    dict(id='dejavu', name='DejaVu Sans font (Book, Bold)', version='2.37',
         license='Bitstream Vera license (DejaVu changes: public domain)',
         source='https://github.com/dejavu-fonts/dejavu-fonts/releases/tag/version_2_37', packages=ALL,
         texts=[('LICENSE', 'repo:third_party/fonts/DejaVu/LICENSE')]),
    dict(id='miniz', name='miniz', version='3.0.2', license='MIT',
         source='https://github.com/richgel999/miniz/releases/tag/3.0.2', packages=DESKTOP + (I,),
         notes='Built into the executable: reads the iOS kit and writes the .ipa (iOS: the kit\'s miniz-LICENSE.txt).',
         texts=[('LICENSE', gh('richgel999/miniz', '3.0.2', 'LICENSE'))]),
    dict(id='shaderc', name='shaderc (with glslang, SPIRV-Tools, SPIRV-Headers inside)',
         version='2026.4 (Windows, MSYS2 package), 2026.3 (Linux, Android)',
         license='Apache 2.0; glslang: BSD 3-clause and others (see its text); SPIRV-Tools: Apache 2.0; '
                 'SPIRV-Headers: MIT-style',
         source='https://github.com/google/shaderc, https://github.com/KhronosGroup/glslang, '
                'https://github.com/KhronosGroup/SPIRV-Tools, https://github.com/KhronosGroup/SPIRV-Headers '
                '(the revisions in shaderc\'s DEPS file at each tag)',
         packages=VULKAN, notes='libshaderc_shared: GLSL to SPIR-V for the Vulkan renderer.',
         texts=[('shaderc LICENSE (v2026.3)', gh('google/shaderc', 'v2026.3', 'LICENSE')),
                ('shaderc LICENSE (v2026.4)', gh('google/shaderc', 'v2026.4', 'LICENSE'))]
               + [t for tag, (gl, st, sh) in SHADERC_DEPS.items() for t in (
                   (f'glslang LICENSE.txt ({gl[:12]}, shaderc {tag})', gh('KhronosGroup/glslang', gl, 'LICENSE.txt')),
                   (f'SPIRV-Tools LICENSE ({st[:12]}, shaderc {tag})', gh('KhronosGroup/SPIRV-Tools', st, 'LICENSE')),
                   (f'SPIRV-Headers LICENSE ({sh[:12]}, shaderc {tag})',
                    gh('KhronosGroup/SPIRV-Headers', sh, 'LICENSE')))]),
    dict(id='vma', name='Vulkan Memory Allocator', version='3.3.0', license='MIT',
         source='https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/releases/tag/v3.3.0',
         packages=VULKAN, notes='Built into the executable.',
         texts=[('LICENSE.txt', gh('GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator', 'v3.3.0', 'LICENSE.txt'))]),
    dict(id='fsr1', name='AMD FidelityFX Super Resolution 1 (ported)', version='1.0.2', license='MIT',
         source='https://github.com/GPUOpen-Effects/FidelityFX-FSR/releases/tag/v1.0.2', packages=VULKAN,
         notes='The EASU / RCAS passes, ported into the Vulkan renderer.',
         texts=[('license.txt', gh('GPUOpen-Effects/FidelityFX-FSR', 'v1.0.2', 'license.txt'))]),
    dict(id='volk', name='volk', version='vulkan-sdk-1.4.350.1', license='MIT',
         source='https://github.com/zeux/volk/releases/tag/vulkan-sdk-1.4.350.1', packages=(A,),
         texts=[('LICENSE.md', gh('zeux/volk', 'vulkan-sdk-1.4.350.1', 'LICENSE.md'))]),
    dict(id='adrenotools', name='libadrenotools', version='8fae8ce254dfc1344527e05301e43f37dea2df80',
         license='BSD 2-clause', source='https://github.com/bylaws/libadrenotools', packages=(A,),
         texts=[('LICENSE', gh('bylaws/libadrenotools', '8fae8ce254dfc1344527e05301e43f37dea2df80', 'LICENSE'))]),
    dict(id='linkernsbypass', name='liblinkernsbypass', version='aa3975893d83ef1bc84c321ec60c65fbf1287887',
         license='BSD 2-clause', source='https://github.com/bylaws/liblinkernsbypass', packages=(A,),
         texts=[('LICENSE', gh('bylaws/liblinkernsbypass', 'aa3975893d83ef1bc84c321ec60c65fbf1287887', 'LICENSE'))]),
    dict(id='zig', name='Zig', version='0.17.0', license='MIT; its C library headers and sources keep their own '
                                                          'licenses (musl: MIT; mingw-w64: see its text; glibc: '
                                                          'LGPL 2.1 or later and others; Apple\'s headers: their '
                                                          'own notices, kept in each file)',
         source='https://ziglang.org/download/0.17.0/, https://codeberg.org/ziglang/zig/src/tag/0.17.0',
         packages=ALL,
         notes='Builds the game code on the player\'s machine during setup (zig/; on Android libzig.so and '
               'assets/gen.zip). Only the C library files for the package\'s own OS are kept: mingw-w64 (Windows, '
               'with its runtime built ahead of time in zig/rt), Darwin (macOS), glibc (Linux), musl (Android). '
               'The iOS kit carries Zig\'s Darwin headers (include/) and libSystem.B.tbd. '
               'Zig contains LLVM, Clang and LLD: see "LLVM".',
         texts=[('LICENSE', cb('ziglang/zig', '0.17.0', 'LICENSE')),
                ('lib/libc/musl/COPYRIGHT', cb('ziglang/zig', '0.17.0', 'lib/libc/musl/COPYRIGHT')),
                ('lib/libc/mingw/COPYING', cb('ziglang/zig', '0.17.0', 'lib/libc/mingw/COPYING')),
                ('lib/libc/glibc/LICENSES', cb('ziglang/zig', '0.17.0', 'lib/libc/glibc/LICENSES')),
                ('GNU Lesser General Public License 2.1 (glibc)', 'https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt')]),
    dict(id='llvm', name='LLVM (in Zig: LLVM, Clang, LLD, compiler-rt, libunwind; LLD as rust-lld in the iOS kit; '
                         'libc++ in the Android app and, with libunwind, the Windows ARM64 .zip)',
         version='22 (Zig 0.17.0); 21.1.2 (rust-lld from Rust 1.91.0, iOS kit lld/, LICENSE.TXT next to each); as '
                 'built by the Android NDK and MSYS2 CLANGARM64',
         license='Apache 2.0 with LLVM Exceptions', source='https://github.com/llvm/llvm-project, '
                                                           'https://static.rust-lang.org/dist/rustc-1.91.0-src.tar.xz',
         packages=ALL,
         texts=[('LICENSE.TXT', gh('llvm/llvm-project', 'llvmorg-22.1.0', 'llvm/LICENSE.TXT'))]),
    dict(id='monocypher', name='Monocypher', version='4.0.2', license='CC0 1.0 or BSD 2-clause, at your choice',
         source='https://monocypher.org', packages=ALL,
         notes='Checks update signatures; built into the executable (third_party/monocypher).',
         texts=[('LICENCE.md', 'repo:third_party/monocypher/LICENCE.md')]),
    dict(id='yyjson', name='yyjson', version='0.13.0', license='MIT', source='https://github.com/ibireme/yyjson',
         packages=ALL, notes='Reads and writes seed files; built into the executable (third_party/yyjson).',
         texts=[('LICENSE', 'repo:third_party/yyjson/LICENSE')]),
    dict(id='rgbcx', name='rgbcx', version='1.12', license='MIT or public domain',
         source='https://github.com/richgel999/bc7enc', packages=ALL,
         notes='Encodes the randomizer\'s textures; built into the executable (third_party/rgbcx).',
         texts=[('LICENSE', 'repo:third_party/rgbcx/LICENSE')]),
    dict(id='sparkle', name='Sparkle', version='2.10.1', license='MIT (its text also covers what Sparkle contains)',
         source='https://github.com/sparkle-project/Sparkle/releases/tag/2.10.1', packages=(M,),
         notes='The macOS updater (ReAwakening.app/Contents/Frameworks/Sparkle.framework).',
         texts=[('LICENSE', gh('sparkle-project/Sparkle', '2.10.1', 'LICENSE'))]),
    dict(id='mingw-runtime', name='Windows compiler runtime: libgcc, libstdc++ (GCC), winpthreads (mingw-w64)',
         version='GCC 16.2.0, mingw-w64 14.0.0 (MSYS2 UCRT64 packages)',
         license='GCC runtime: GPL 3.0 with the GCC Runtime Library Exception; winpthreads: MIT and BSD 3-clause',
         source='https://gcc.gnu.org/, https://www.mingw-w64.org/, https://packages.msys2.org',
         packages=(W,),
         notes='x64: libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll next to lasrecomp.exe. ARM64 '
               '(MSYS2 CLANGARM64): libwinpthread-1.dll, with LLVM\'s libc++ and libunwind instead of GCC\'s (see '
               '"LLVM").',
         texts=[('GCC COPYING.RUNTIME', gh('gcc-mirror/gcc', 'releases/gcc-16.2.0', 'COPYING.RUNTIME')),
                ('GCC COPYING3', gh('gcc-mirror/gcc', 'releases/gcc-16.2.0', 'COPYING3')),
                ('mingw-w64 winpthreads COPYING',
                 gh('mingw-w64/mingw-w64', 'v14.0.0', 'mingw-w64-libraries/winpthreads/COPYING'))]),
    dict(id='libiconv', name='GNU libiconv', version='1.19 (MSYS2 UCRT64 package)', license='LGPL 2.1 or later',
         source='https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.19.tar.gz', packages=(W,),
         notes='libiconv-2.dll next to lasrecomp.exe (used by SDL 3), a shared library you may replace.',
         texts=[('COPYING.LIB', 'https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.19.tar.gz!libiconv-1.19/COPYING.LIB')]),
    dict(id='appimage-runtime', name='AppImage type 2 runtime (with libfuse, squashfuse, Zstandard, zlib, mimalloc, '
                                     'musl inside)',
         version='20251108 (libfuse 3.15.0, squashfuse 0.5.2; the rest from Alpine Linux packages)',
         license='MIT; libfuse: LGPL 2.1; squashfuse: BSD 2-clause; Zstandard: BSD 3-clause; zlib: zlib; '
                 'mimalloc: MIT; musl: MIT',
         source='https://github.com/AppImage/type2-runtime/releases/tag/20251108, '
                'https://github.com/libfuse/libfuse/releases/tag/fuse-3.15.0, '
                'https://github.com/vasi/squashfuse/releases/tag/0.5.2',
         packages=(L,), notes='The start of the .AppImage file, which mounts the rest.',
         texts=[('type2-runtime LICENSE', gh('AppImage/type2-runtime', '20251108', 'LICENSE')),
                ('libfuse LICENSE', gh('libfuse/libfuse', 'fuse-3.15.0', 'LICENSE')),
                ('libfuse LGPL2.txt', gh('libfuse/libfuse', 'fuse-3.15.0', 'LGPL2.txt')),
                ('squashfuse LICENSE', gh('vasi/squashfuse', '0.5.2', 'LICENSE')),
                ('mimalloc LICENSE', gh('microsoft/mimalloc', 'v2.2.4', 'LICENSE')),
                ('musl COPYRIGHT', cb('ziglang/zig', '0.17.0', 'lib/libc/musl/COPYRIGHT'))]),
]

HEADER = """\
LAS Recomp: third-party notices
===============================

LAS Recomp's own code is under the MIT License (the LICENSE file). The release packages also carry the third-party
software below. Each entry gives its version, its license, where its source code is, and which packages carry it:

  Windows  the Windows .zip (x64 and ARM64)
  macOS    the macOS .dmg and the ReAwakening.app inside it
  Linux    the Linux AppImages and .tar.gz (x86-64 and aarch64)
  Android  the Android .apk
  iOS      the iOS kit (.laskit) and the LAS Recomp.app inside it

The full license texts follow the list, in the same order, each taken from that project's source at the version
shipped. The libraries under the LGPL are shared libraries, except FFmpeg in the iOS app (relinkable from its
source with LAS Recomp's build scripts); their complete source is at the link given for each. This file is made by tools/notices.py in https://github.com/peiikai/LASRecomp.

"""
RULE = '=' * 118


def entry_head(c):
    lines = [f'{c["name"]}', f'  Version:  {c["version"]}', f'  License:  {c["license"]}',
             f'  Source:   {c["source"]}', f'  Packages: {", ".join(c["packages"])}']
    if c.get('notes'):
        lines.append(f'  Notes:    {c["notes"]}')
    return '\n'.join(lines) + '\n'


def license_file(c):
    return os.path.join(LICENSES, c['id'] + '.txt')


# ---- fetch

def download(url):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, hashlib.sha256(url.encode()).hexdigest()[:16] + '-' + os.path.basename(url))
    if not os.path.exists(path):
        req = urllib.request.Request(url, headers={'User-Agent': 'las-recomp-notices'})
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                data = r.read()
        except OSError as e:
            sys.exit(f'notices: {url}: {e}')
        with open(path + '.part', 'wb') as f:
            f.write(data)
        os.replace(path + '.part', path)
    with open(path, 'rb') as f:
        return f.read()


def read_source(src):
    if src.startswith('repo-tail:'):  # (repo-tail:<path>:<first line>: a license at the end of a source file)
        path, first = src[10:].split(':', 1)
        with open(os.path.join(ROOT, path), 'rb') as f:
            data = f.read()
        tail = data[data.index(first.encode()):]
        return tail[:tail.rindex(b'*/')] if b'*/' in tail else tail
    if src.startswith('repo:'):
        with open(os.path.join(ROOT, src[5:]), 'rb') as f:
            return f.read()
    if '!' in src:
        url, member = src.split('!', 1)
        data = download(url)
        if url.endswith(('.whl', '.zip')):
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                return z.read(member)
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            return t.extractfile(member).read()
    return download(src)


def text_of(raw):
    t = raw.decode('utf-8-sig', errors='replace').replace('\r\n', '\n').replace('\r', '\n')
    return '\n'.join(line.rstrip() for line in t.split('\n')).strip('\n') + '\n'


def render_license(c):
    out = [entry_head(c)]
    if not c['texts']:
        out.append('\n(No license text to include: see the notes above.)\n')
    seen = {}  # (the same text at two versions: once)
    for label, src in c['texts']:
        t = text_of(read_source(src))
        out.append(f'\n----- {label} -----\n\n')
        out.append(f'(The same text as "{seen[t]}" above.)\n' if t in seen else t)
        seen.setdefault(t, label)
    return ''.join(out)


def cmd_fetch(ids):
    os.makedirs(LICENSES, exist_ok=True)
    for c in COMPONENTS:
        if ids and c['id'] not in ids:
            continue
        body = render_license(c)
        with open(license_file(c), 'w', encoding='utf-8', newline='\n') as f:
            f.write(body)
        print(f'licenses/{c["id"]}.txt: {len(c["texts"])} text(s), {len(body)} bytes')


# ---- build / check

def notices_text():
    out = [HEADER, 'Contents\n--------\n\n']
    for c in COMPONENTS:
        out.append(f'  {c["name"]} ({", ".join(c["packages"])})\n')
    for c in COMPONENTS:
        with open(license_file(c), encoding='utf-8') as f:
            body = f.read()
        out.append(f'\n\n{RULE}\n{body}')
    return ''.join(out)


def cmd_build():
    with open(NOTICES, 'w', encoding='utf-8', newline='\n') as f:
        f.write(notices_text())
    print(f'{NAME}: {os.path.getsize(NOTICES)} bytes, {len(COMPONENTS)} components')


def cmd_check():
    bad = []
    ids = {c['id'] for c in COMPONENTS}
    for c in COMPONENTS:
        p = license_file(c)
        if not os.path.exists(p):
            bad.append(f'licenses/{c["id"]}.txt missing (tools/notices.py fetch {c["id"]})')
            continue
        with open(p, encoding='utf-8') as f:
            body = f.read()
        if not body.startswith(entry_head(c)):
            bad.append(f'licenses/{c["id"]}.txt: its header is not COMPONENTS\' (tools/notices.py fetch {c["id"]})')
        for label, src in c['texts']:
            if f'\n----- {label} -----\n' not in body:
                bad.append(f'licenses/{c["id"]}.txt: no "{label}" (tools/notices.py fetch {c["id"]})')
            elif src.startswith(('repo:', 'repo-tail:')) and text_of(read_source(src)) not in body:
                bad.append(f'licenses/{c["id"]}.txt: {src.split(":")[1]} changed (tools/notices.py fetch {c["id"]})')
    for n in sorted(os.listdir(LICENSES)):
        if n.endswith('.txt') and n[:-4] not in ids:
            bad.append(f'licenses/{n}: not in COMPONENTS')
    if not bad:
        try:
            with open(NOTICES, encoding='utf-8') as f:
                if f.read() != notices_text():
                    bad.append(f'{NAME} is out of date (tools/notices.py build)')
        except FileNotFoundError:
            bad.append(f'no {NAME} (tools/notices.py build)')
    for b in bad:
        print('notices:', b)
    if bad:
        sys.exit(1)
    print(f'notices: {len(COMPONENTS)} components, {NAME} up to date')


# ---- package

def expected():
    with open(NOTICES, 'rb') as f:
        return f.read()


def check_dir(d, wanted):
    hits = []
    for w in wanted:
        p = os.path.join(d, w)
        if not os.path.isfile(p):
            return f'no {w}'
        with open(p, 'rb') as f:
            if f.read() != expected():
                return f'{w} differs from the repository\'s {NAME}'
        hits.append(w)
    return hits


def cmd_package(path):
    base = os.path.basename(path)
    if os.path.isdir(path) and base.rstrip('/').endswith('.app'):  # (macOS: its root can't hold files; iOS: flat)
        r = check_dir(path, [f'Contents/Resources/{NAME}' if os.path.isdir(os.path.join(path, 'Contents')) else NAME])
    elif os.path.isdir(path):
        r = check_dir(path, [NAME])
    elif base.endswith(('.laskit', '.ipa')):  # (iOS: the kit's root and the app in it; the .ipa: the app)
        app = f'Payload/LAS Recomp.app/{NAME}'
        wanted = [NAME, app] if base.endswith('.laskit') else [app]
        with zipfile.ZipFile(path) as z:
            have = set(z.namelist())
            r = next((f'no {w}' for w in wanted if w not in have), None) or \
                next((f'{w} differs' for w in wanted if z.read(w) != expected()), None) or wanted
    elif base.endswith('.tar.gz'):  # (Linux: one top folder, "LAS Recomp/")
        with tarfile.open(path) as t:
            names = [n for n in t.getnames() if n.count('/') == 1 and n.endswith('/' + NAME)]
            r = f'no <top folder>/{NAME}' if not names else (
                names if all(t.extractfile(n).read() == expected() for n in names) else f'{names[0]} differs')
    elif base.endswith('.zip'):  # (Windows: one top folder, "LAS Recomp/")
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if n.count('/') == 1 and n.endswith('/' + NAME)]
            r = f'no <top folder>/{NAME}' if not names else (
                names if all(z.read(n) == expected() for n in names) else f'{names[0]} differs')
    elif base.endswith('.apk'):
        with zipfile.ZipFile(path) as z:
            r = ([NAME] if z.read(NAME) == expected() else f'{NAME} differs') if NAME in z.namelist() \
                else f'no {NAME} at the .apk root'
    elif base.endswith('.dmg'):
        mnt = tempfile.mkdtemp()
        subprocess.run(['hdiutil', 'attach', '-readonly', '-nobrowse', '-mountpoint', mnt, path], check=True,
                       stdout=subprocess.DEVNULL)
        try:
            r = check_dir(mnt, [NAME, f'ReAwakening.app/Contents/Resources/{NAME}'])
        finally:
            subprocess.run(['hdiutil', 'detach', mnt], check=False, stdout=subprocess.DEVNULL)
    elif base.endswith('.AppImage'):  # (runs it: on a machine of the AppImage's own arch)
        tmp = tempfile.mkdtemp()
        try:
            subprocess.run([os.path.abspath(path), '--appimage-extract', NAME], cwd=tmp, check=True,
                           stdout=subprocess.DEVNULL)
            r = check_dir(os.path.join(tmp, 'squashfs-root'), [NAME])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    else:
        sys.exit(f'notices: {path}: not a .zip, .tar.gz, .dmg, .AppImage, .apk, .laskit, .ipa, .app or folder')
    if isinstance(r, str):
        sys.exit(f'notices: {path}: {r}')
    print(f'notices: {path}: {", ".join(r)} ok')


def main():
    a = sys.argv[1:]
    if not a or a[0] in ('-h', '--help'):
        print(__doc__)
    elif a[0] == 'fetch':
        cmd_fetch(set(a[1:]))
    elif a[0] == 'build':
        cmd_build()
    elif a[0] == 'check':
        cmd_check()
    elif a[0] == 'package' and len(a) >= 2:
        for p in a[1:]:
            cmd_package(p)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
