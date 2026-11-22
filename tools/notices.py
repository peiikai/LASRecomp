#!/usr/bin/env python3
"""Third-party notices of the release packages (#868).

The Windows .zip, the macOS .dmg (and the app inside it), the Linux AppImages and the Android .apk carry libraries,
fonts, a compiler and two Python runtimes that are not ours. Their licenses ask for their notices and license texts to
travel with the binaries: THIRD-PARTY-NOTICES.txt at the root of each package holds them all.

  tools/notices.py fetch [id ...]  download each component's license text(s) at the version shipped into
                                   licenses/<id>.txt (after a version bump in a build script: update COMPONENTS first)
  tools/notices.py build           write THIRD-PARTY-NOTICES.txt from COMPONENTS and licenses/
  tools/notices.py check           licenses/ and THIRD-PARTY-NOTICES.txt match COMPONENTS and the repository (CI)
  tools/notices.py package <file>  the notices file is in a built package, unchanged: a .zip, .dmg, .AppImage,
                                   .apk, or an unpacked folder (the release workflow runs it on every package)

COMPONENTS is the list of what the packages carry, with the version pinned in the build scripts (tools/build_deps_*.sh,
tools/linux/, android/deps/, the Makefile's pip pins, tools/fetch_*.sh). Texts come from each project's own source at
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

W, M, L, A = 'Windows', 'macOS', 'Linux', 'Android'
DESKTOP = (W, M, L)
ALL = (W, M, L, A)
VULKAN = (W, L, A)


def gh(repo, ref, path):
    return f'https://raw.githubusercontent.com/{repo}/{ref}/{path}'


def cb(repo, ref, path):
    return f'https://codeberg.org/{repo}/raw/tag/{ref}/{path}'


PYPI = 'https://files.pythonhosted.org/packages/'
OEAD_SDIST = PYPI + 'e1/39/37fe44f34587c00419cfdb015b3c753c7bf82ff82437c973f9d0ac17f2ad/oead-1.3.0.tar.gz'
QUICKTEX_SDIST = PYPI + '91/c5/0156ccfe21dcfcce37c3ecbac7bb96c1633c4e14b136c27540fd1c0c9981/quicktex-0.2.1.tar.gz'
KEYSTONE_SDIST = PYPI + '0a/65/3a2e7e55cc1db188869bbbacee60036828330e0ce57fc5f05a3167ab4b4d/keystone-engine-0.9.2.tar.gz'
EVFL_WHL = PYPI + 'b5/3c/49415ce6123f36ad2352610baf07170b695a599739da91cf13f3bf11d343/evfl-1.2.0-py3-none-any.whl'
APPDIRS_WHL = PYPI + '3b/00/2344469e2084fb287c2e0b57b72910309874c3245463acd6cf5e3db69324/appdirs-1.4.4-py2.py3-none-any.whl'
PYYAML_SDIST = PYPI + '05/8e/961c0007c59b8dd7729d542c61a4d537767a59645b82a0b521206e1e25c2/pyyaml-6.0.3.tar.gz'
PBS = 'astral-sh/python-build-standalone'
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
               'lasrecomp.exe; macOS: LAS Recomp.app/Contents/Frameworks; Linux: usr/lib in the AppImage; Android: '
               'lib/arm64-v8a in the .apk). Built unmodified from the source above with only the decoders the game '
               'uses; the exact configure lines are in tools/build_deps_windows.sh, tools/build_deps_macos.sh, '
               'tools/linux/build_deps.sh and android/deps/build_deps.sh in LAS Recomp\'s repository '
               '(https://github.com/peiikai/LASRecomp), and each library reports its own with '
               'avcodec_configuration(). You may replace these libraries with your own build of FFmpeg 9.0.x '
               'built the same way.',
         texts=[('LICENSE.md', gh('FFmpeg/FFmpeg', 'n9.0.2', 'LICENSE.md')),
                ('COPYING.LGPLv2.1', gh('FFmpeg/FFmpeg', 'n9.0.2', 'COPYING.LGPLv2.1'))]),
    dict(id='sdl3', name='SDL 3', version='3.4.18 (Windows, macOS), 3.4.16 (Linux, Android)', license='zlib',
         source='https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18, '
                'https://github.com/libsdl-org/SDL/releases/tag/release-3.4.16',
         packages=ALL,
         texts=[('LICENSE.txt (3.4.18)', gh('libsdl-org/SDL', 'release-3.4.18', 'LICENSE.txt')),
                ('LICENSE.txt (3.4.16)', gh('libsdl-org/SDL', 'release-3.4.16', 'LICENSE.txt'))]),
    dict(id='imgui', name='Dear ImGui', version='1.91.9', license='MIT',
         source='https://github.com/ocornut/imgui', packages=ALL, notes='Built into the executable (third_party/imgui).',
         texts=[('LICENSE.txt', 'repo:third_party/imgui/LICENSE.txt')]),
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
         notes='The seed generator (Windows, macOS, Linux: rando-gen/; Android: inside assets/gen.zip).',
         texts=[('LICENSE.txt', 'repo:third_party/LAS-Randomizer/LICENSE.txt')]),
    dict(id='kenney', name='Kenney Input Prompts (D-pad art)', version='-', license='CC0 1.0',
         source='https://www.kenney.nl', packages=ALL, notes='Built into the executable (third_party/kenney).',
         texts=[('LICENSE.txt', 'repo:third_party/kenney/LICENSE.txt')]),
    dict(id='baloo2', name='Baloo 2 font (SemiBold)', version='1.640', license='SIL Open Font License 1.1',
         source='https://github.com/EkType/Baloo2', packages=ALL,
         texts=[('OFL.txt', 'repo:third_party/fonts/Baloo2/OFL.txt')]),
    dict(id='biz-udpgothic', name='BIZ UDPGothic font (Regular)', version='1.05 (Google Fonts)',
         license='SIL Open Font License 1.1', source='https://github.com/googlefonts/morisawa-biz-ud-gothic',
         packages=(L, A), texts=[('OFL.txt', 'repo:third_party/fonts/BIZUDPGothic/OFL.txt')]),
    dict(id='dejavu', name='DejaVu Sans font (Book, Bold)', version='2.37',
         license='Bitstream Vera license (DejaVu changes: public domain)',
         source='https://github.com/dejavu-fonts/dejavu-fonts/releases/tag/version_2_37', packages=ALL,
         texts=[('LICENSE', 'repo:third_party/fonts/DejaVu/LICENSE')]),
    dict(id='miniz', name='miniz', version='3.0.2', license='MIT',
         source='https://github.com/richgel999/miniz/releases/tag/3.0.2', packages=DESKTOP,
         notes='Built into the executable: reads the iOS kit and writes the .ipa.',
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
               'Zig contains LLVM, Clang and LLD: see "LLVM".',
         texts=[('LICENSE', cb('ziglang/zig', '0.17.0', 'LICENSE')),
                ('lib/libc/musl/COPYRIGHT', cb('ziglang/zig', '0.17.0', 'lib/libc/musl/COPYRIGHT')),
                ('lib/libc/mingw/COPYING', cb('ziglang/zig', '0.17.0', 'lib/libc/mingw/COPYING')),
                ('lib/libc/glibc/LICENSES', cb('ziglang/zig', '0.17.0', 'lib/libc/glibc/LICENSES')),
                ('GNU Lesser General Public License 2.1 (glibc)', 'https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt')]),
    dict(id='llvm', name='LLVM (in Zig: LLVM, Clang, LLD, compiler-rt, libunwind; libomp in the macOS seed '
                         'generator; libc++ in the Android app and, with libunwind, the Windows ARM64 .zip)',
         version='22 (Zig 0.17.0); as built by the Android NDK, MSYS2 CLANGARM64 and Homebrew (libomp)',
         license='Apache 2.0 with LLVM Exceptions', source='https://github.com/llvm/llvm-project', packages=ALL,
         texts=[('LICENSE.TXT', gh('llvm/llvm-project', 'llvmorg-22.1.0', 'llvm/LICENSE.TXT'))]),
    dict(id='capstone', name='Capstone', version='5.0.9',
         license='BSD 3-clause; the parts derived from LLVM: University of Illinois/NCSA',
         source='https://github.com/capstone-engine/capstone/releases/tag/5.0.9, https://pypi.org/project/capstone/5.0.9/',
         packages=ALL,
         notes='The game code generator\'s disassembler (Windows, macOS, Linux: lasgen/_internal/capstone; Android: '
               'libcapstone.so).',
         texts=[('LICENSE.TXT', gh('capstone-engine/capstone', '5.0.9', 'LICENSE.TXT')),
                ('LICENSE_LLVM.TXT', gh('capstone-engine/capstone', '5.0.9', 'LICENSE_LLVM.TXT'))]),
    dict(id='unicorn', name='Unicorn', version='2.1.4', license='GPL 2.0 (parts from QEMU: LGPL 2.1, GLib: LGPL 2)',
         source='https://github.com/unicorn-engine/unicorn/releases/tag/2.1.4, https://pypi.org/project/unicorn/2.1.4/',
         packages=ALL,
         notes='Used by the game code generator, a separate program the game runs (Windows, macOS, Linux: '
               'lasgen/_internal/unicorn; Android: libunicorn.so). Its complete source is at the link above.',
         texts=[('COPYING', gh('unicorn-engine/unicorn', '2.1.4', 'COPYING')),
                ('COPYING.LGPL2', gh('unicorn-engine/unicorn', '2.1.4', 'COPYING.LGPL2')),
                ('COPYING_GLIB', gh('unicorn-engine/unicorn', '2.1.4', 'COPYING_GLIB'))]),
    dict(id='lz4', name='python-lz4 and LZ4', version='python-lz4 4.4.5 with LZ4 1.9.4 (Windows, macOS, Linux); '
                                                        'LZ4 1.10.0 (Android)',
         license='python-lz4: BSD 3-clause; LZ4 library: BSD 2-clause',
         source='https://pypi.org/project/lz4/4.4.5/, https://github.com/lz4/lz4/releases/tag/v1.9.4, '
                'https://github.com/lz4/lz4/releases/tag/v1.10.0',
         packages=ALL,
         texts=[('python-lz4 LICENSE', gh('python-lz4/python-lz4', 'v4.4.5', 'LICENSE')),
                ('lz4 lib/LICENSE (v1.9.4)', gh('lz4/lz4', 'v1.9.4', 'lib/LICENSE')),
                ('lz4 lib/LICENSE (v1.10.0)', gh('lz4/lz4', 'v1.10.0', 'lib/LICENSE'))]),
    dict(id='keystone', name='Keystone', version='0.9.2', license='GPL 2.0 (Python bindings: BSD 3-clause; the '
                                                                     'parts derived from LLVM: University of '
                                                                     'Illinois/NCSA)',
         source='https://github.com/keystone-engine/keystone/releases/tag/0.9.2, '
                'https://pypi.org/project/keystone-engine/0.9.2/',
         packages=ALL,
         notes='Used by the seed generator, a separate program the game runs (rando-gen/_internal/keystone; '
               'Android: inside assets/gen.zip). Its complete source is at the links above.',
         texts=[('LICENSE.TXT (Python bindings)', KEYSTONE_SDIST + '!keystone-engine-0.9.2/LICENSE.TXT'),
                ('src/COPYING', KEYSTONE_SDIST + '!keystone-engine-0.9.2/src/COPYING'),
                ('src/llvm/LICENSE.TXT', KEYSTONE_SDIST + '!keystone-engine-0.9.2/src/llvm/LICENSE.TXT'),
                ('src/llvm/lib/Support/COPYRIGHT.regex',
                 KEYSTONE_SDIST + '!keystone-engine-0.9.2/src/llvm/lib/Support/COPYRIGHT.regex'),
                ('src/llvm/lib/Target/ARM/LICENSE.TXT',
                 KEYSTONE_SDIST + '!keystone-engine-0.9.2/src/llvm/lib/Target/ARM/LICENSE.TXT')]),
    dict(id='oead', name='oead (with abseil, libyaml, ordered-map, pybind11, rapidyaml, zlib-ng inside)',
         version='1.3.0', license='GPL 2.0 or later; the libraries inside: see their texts',
         source='https://github.com/zeldamods/oead/releases/tag/v1.3.0, https://pypi.org/project/oead/1.3.0/',
         packages=ALL, notes='Used by the seed generator. Its complete source is at the links above.',
         texts=[('LICENSE.md', OEAD_SDIST + '!oead-1.3.0/LICENSE.md'),
                ('lib/abseil/LICENSE', OEAD_SDIST + '!oead-1.3.0/lib/abseil/LICENSE'),
                ('lib/libyaml/License', OEAD_SDIST + '!oead-1.3.0/lib/libyaml/License'),
                ('lib/ordered-map/LICENSE', OEAD_SDIST + '!oead-1.3.0/lib/ordered-map/LICENSE'),
                ('lib/pybind11/LICENSE', OEAD_SDIST + '!oead-1.3.0/lib/pybind11/LICENSE'),
                ('lib/rapidyaml/LICENSE.txt', OEAD_SDIST + '!oead-1.3.0/lib/rapidyaml/LICENSE.txt'),
                ('lib/zlib-ng/LICENSE.md', OEAD_SDIST + '!oead-1.3.0/lib/zlib-ng/LICENSE.md')]),
    dict(id='evfl', name='evfl', version='1.2.0', license='GPL 2.0 or later',
         source='https://github.com/zeldamods/evfl/releases/tag/v1.2.0, https://pypi.org/project/evfl/1.2.0/',
         packages=ALL, notes='Used by the seed generator. Its complete source is at the links above.',
         texts=[('LICENSE', EVFL_WHL + '!evfl-1.2.0.dist-info/LICENSE')]),
    dict(id='quicktex', name='quicktex', version='0.2.1', license='LGPL 3.0 (which adds to the GPL 3.0)',
         source='https://github.com/drewcassidy/quicktex/releases/tag/v0.2.1, https://pypi.org/project/quicktex/0.2.1/',
         packages=ALL,
         notes='Used by the seed generator: a Python extension module (_quicktex) loaded at run time, which can be '
               'replaced with your own build of it.',
         texts=[('LICENSE.md', QUICKTEX_SDIST + '!quicktex-0.2.1/LICENSE.md'),
                ('GNU General Public License 3.0', 'https://www.gnu.org/licenses/gpl-3.0.txt')]),
    dict(id='pybind11', name='pybind11', version='3.1.0 (Android); as built into the oead and quicktex wheels',
         license='BSD 3-clause', source='https://github.com/pybind/pybind11/releases/tag/v3.1.0', packages=ALL,
         texts=[('LICENSE', gh('pybind/pybind11', 'v3.1.0', 'LICENSE'))]),
    dict(id='pillow', name='Pillow (with the image libraries its wheels bundle)', version='10.4.0',
         license='MIT-CMU (HPND); the bundled libraries: see the texts',
         source='https://github.com/python-pillow/Pillow/releases/tag/10.4.0, https://pypi.org/project/pillow/10.4.0/',
         packages=ALL,
         notes='Used by the seed generator. Windows, macOS, Linux: the PyPI wheel (macOS and Linux: with its '
               'libraries in PIL/.dylibs or pillow.libs); Android: only the _imaging module, built from the source '
               'and using the system\'s zlib.',
         texts=[('pillow-10.4.0.dist-info/LICENSE (macOS and Linux wheels)',
                 PYPI + 'e7/cf/5c558a0f247e0bf9cec92bff9b46ae6474dd736f6d906315e60e4075f737/'
                        'pillow-10.4.0-cp312-cp312-macosx_11_0_arm64.whl!pillow-10.4.0.dist-info/LICENSE'),
                ('pillow-10.4.0.dist-info/LICENSE (Windows wheel)',
                 PYPI + '74/0a/d4ce3c44bca8635bd29a2eab5aa181b654a734a29b263ca8efe013beea98/'
                        'pillow-10.4.0-cp312-cp312-win_amd64.whl!pillow-10.4.0.dist-info/LICENSE')]),
    dict(id='pyyaml', name='PyYAML (with LibYAML in the desktop wheels)',
         version='6.0.2 (Windows, macOS, Linux; LibYAML 0.2.5), 6.0.3 (Android, pure Python)', license='MIT',
         source='https://pypi.org/project/PyYAML/6.0.2/, https://pypi.org/project/PyYAML/6.0.3/, '
                'https://github.com/yaml/libyaml/releases/tag/0.2.5',
         packages=ALL,
         texts=[('LICENSE (6.0.2)', gh('yaml/pyyaml', '6.0.2', 'LICENSE')),
                ('LICENSE (6.0.3)', PYYAML_SDIST + '!pyyaml-6.0.3/LICENSE'),
                ('LibYAML License (0.2.5)', gh('yaml/libyaml', '0.2.5', 'License'))]),
    dict(id='appdirs', name='appdirs', version='1.4.4', license='MIT', source='https://pypi.org/project/appdirs/1.4.4/',
         packages=ALL, texts=[('LICENSE.txt', APPDIRS_WHL + '!appdirs-1.4.4.dist-info/LICENSE.txt')]),
    dict(id='pyinstaller', name='PyInstaller (bootloader)', version='6.22.3',
         license='GPL 2.0 or later with the PyInstaller bootloader exception',
         source='https://github.com/pyinstaller/pyinstaller/releases/tag/v6.22.3', packages=DESKTOP,
         notes='Packs the seed generator (rando-gen) and the game code generator (lasgen) as standalone programs.',
         texts=[('COPYING.txt', gh('pyinstaller/pyinstaller', 'v6.22.3', 'COPYING.txt'))]),
    dict(id='python', name='CPython',
         version='3.12.10 (Windows, python.org), 3.12.14 (macOS, python-build-standalone 20260924), '
                 '3.12.15 (Linux, python-build-standalone 20261020), 3.14.8 (Android, python.org)',
         license='Python Software Foundation License 2', source='https://github.com/python/cpython, '
                                                             'https://github.com/astral-sh/python-build-standalone',
         packages=ALL,
         notes='Runs the seed generator and the game code generator (rando-gen/_internal and lasgen/_internal; '
               'Android: libpython3.14.so and assets/gen.zip). The libraries built into it are listed under '
               '"Libraries inside CPython".',
         texts=[(f'LICENSE (v{v})', gh('python/cpython', f'v{v}', 'LICENSE'))
                for v in ('3.12.10', '3.12.14', '3.12.15', '3.14.8')]
               + [(f'Doc/license.rst: licenses and acknowledgements for incorporated software (v{v})',
                   gh('python/cpython', f'v{v}', 'Doc/license.rst')) for v in ('3.12.15', '3.14.8')]),
    dict(id='python-libs', name='Libraries inside CPython',
         version='python-build-standalone 20260924 / 20261020 (macOS, Linux): bzip2 1.0.8, expat, libedit, libffi '
                 '3.4.8, mpdecimal 4.0.0, ncurses 6.5, OpenSSL 3.5, SQLite, libuuid 1.0.3, XZ 5.8, zlib; '
                 'python.org 3.12.10 (Windows): bzip2 1.0.8, libffi 3.4.4, XZ 5.2.5, zlib 1.3.1; '
                 'python.org 3.14.8 (Android): bzip2 1.0.8, libffi 3.4.4, XZ 5.4.6, Zstandard 1.5.7',
         license='bzip2: bzip2 license; expat, libffi, ncurses: MIT-style; libedit, libuuid: BSD 3-clause; '
                 'mpdecimal: BSD 2-clause; OpenSSL: Apache 2.0; SQLite: public domain; XZ: 0BSD (5.2.5: public '
                 'domain); zlib: zlib; Zstandard: BSD 3-clause',
         source='https://github.com/astral-sh/python-build-standalone/tree/20260924/pythonbuild/downloads.json, '
                'https://github.com/python/cpython/blob/v3.12.10/PCbuild/get_externals.bat, '
                'https://github.com/python/cpython/blob/v3.14.8/Android/android.py',
         packages=ALL,
         texts=[(f'{n}', gh(PBS, '20260924', f'LICENSE.{f}.txt')) for n, f in (
                    ('bzip2', 'bzip2'), ('expat', 'expat'), ('libedit', 'libedit'), ('libffi (3.4.8)', 'libffi'),
                    ('mpdecimal', 'mpdecimal'), ('ncurses', 'ncurses'), ('OpenSSL 3', 'openssl-3'),
                    ('SQLite', 'sqlite'), ('libuuid', 'libuuid'), ('XZ (liblzma 5.8)', 'liblzma'),
                    ('zlib', 'zlib'))]
               + [('libffi LICENSE (3.4.4)', gh('libffi/libffi', 'v3.4.4', 'LICENSE')),
                  ('XZ COPYING (5.2.5)', gh('tukaani-project/xz', 'v5.2.5', 'COPYING')),
                  ('XZ COPYING (5.4.6)', gh('tukaani-project/xz', 'v5.4.6', 'COPYING')),
                  ('zlib LICENSE (1.3.1)', gh('madler/zlib', 'v1.3.1', 'LICENSE')),
                  ('Zstandard LICENSE (1.5.7)', gh('facebook/zstd', 'v1.5.7', 'LICENSE'))]),
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
    dict(id='msvc-runtime', name='Microsoft Visual C++ runtime', version='as shipped with the python.org CPython '
                                                                        'and the PyPI wheels',
         license='Microsoft Visual C++ Redistributable license terms', source='-', packages=(W,),
         notes='VCRUNTIME140.dll, VCRUNTIME140_1.dll, MSVCP140.dll, VCOMP140.DLL inside rando-gen/_internal and '
               'lasgen/_internal, redistributed as the Visual C++ Redistributable allows.', texts=[]),
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
  macOS    the macOS .dmg and the LAS Recomp.app inside it
  Linux    the Linux AppImages (x86-64 and aarch64)
  Android  the Android .apk

The full license texts follow the list, in the same order, each taken from that project's source at the version
shipped. The libraries under the GPL or LGPL are separate programs or shared libraries; their complete source is at
the link given for each. This file is made by tools/notices.py in https://github.com/peiikai/LASRecomp.

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
            elif src.startswith('repo:') and text_of(read_source(src)) not in body:
                bad.append(f'licenses/{c["id"]}.txt: {src[5:]} changed (tools/notices.py fetch {c["id"]})')
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
    if os.path.isdir(path):
        r = check_dir(path, [NAME])
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
            r = check_dir(mnt, [NAME, f'LAS Recomp.app/Contents/Resources/{NAME}'])
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
        sys.exit(f'notices: {path}: not a .zip, .dmg, .AppImage, .apk or folder')
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
