# iOS port: technical debt

Known shortcuts in the iOS port, with where they live and what the fix is. Add to this when taking another one.

## Embedded Python (the seed generator on the device)

- **The Python version is spelled out in several places.** `3.13` / `313` in [Makefile](../Makefile) (`DEPINC`),
  [tools/build_deps_ios.sh](../tools/build_deps_ios.sh) (`PYV`, the `.cpython-313-iphoneos.so` names, `--python-version`),
  [tools/bundle_ios.sh](../tools/bundle_ios.sh) (`python/lib/python3.13`, the `lib-dynload` prefix) and
  [runtime/platform/ios_python.m](../runtime/platform/ios_python.m) (`sys.path` entries). Fix: one `PYV` in the
  Makefile exported to both scripts and passed as `-DLARECOMP_PY_VERSION=\"3.13\"`; the extension suffix read from the
  slice (`python3-config --extension-suffix` of the cross venv) instead of typed.
- **The native packages aren't driven by `requirements.txt`.** `build_deps_ios.sh` pins keystone 0.9.2, oead 1.3.0,
  PyYAML 6.0.2, quicktex 0.2.1, Pillow 10.4 by hand, and installs evfl / appdirs unpinned; the randomizer's own
  [requirements.txt](../third_party/LAS-Randomizer/requirements.txt) is never read. A version bump there is silently
  ignored on iOS. Fix: read the requirements file, `pip download` the sdists it names, and build from those; or at
  least a check in the script that the versions it pins satisfy the file's specifiers.
- **oead's checksum is fetched from PyPI at build time** (`curl … /pypi/oead/1.3.0/json`), unlike every other source,
  which is pinned in the script. Pin it.
- **keystone is built with CMake directly and patched with `sed`.** Its Python binding can't build the library for
  another target (its `setup.py` runs `make-share.sh` for the host and copies the result), so the library is built
  the way `tools/build_deps_macos.sh` builds it, plus two iOS patches: the ELF-only `-Wl,-z,defs` taken out of
  `HandleLLVMOptions.cmake` (keystone's LLVM 3.x knows Darwin and Windows, not iOS), and
  `PYTHON_EXECUTABLE` / `PYTHON_VERSION_STRING` / `PYTHONINTERP_FOUND` forced (with `CMAKE_SYSTEM_NAME=iOS` the
  old `FindPythonInterp` looks for a target interpreter). Both are gaps in keystone, not shortcuts of ours; put them
  in a `tools/patches/keystone-ios.patch` applied with `git apply` so the diff is one file, and share the `CMP0051`
  fix with the macOS script. (quicktex, by contrast, builds as a wheel through its own `setup.py` from the cross
  venv, as oead does - the direct-CMake path it had was a misread of `cmake` missing from PATH.)
- **Pillow comes from BeeWare's package index** (`https://pypi.anaconda.org/beeware/simple`), not PyPI: a third-party
  binary we don't build and whose availability we don't control. Fix: build it from the sdist in the cross venv
  (its codecs static), or at least pin the wheel's checksum.
- **BeeWare's Python-Apple-support release is a binary dependency** (`Python-3.13-iOS-support.b16`): a prebuilt
  CPython we don't rebuild. Acceptable (it's the CPython project's own iOS support, PEP 730), but a new `b`-release
  changes the ABI of every extension we build; note the pairing.
- **The cross venv needs a host `python3.13`** of exactly the embedded version (`HOSTPY`), found via `command -v` and
  not checked against `PYV` beyond the name. Mismatch -> wheels tagged for the wrong ABI.
- **`.fwork` framework-per-extension layout is reimplemented in the bundle script** (~60 lines of shell reproducing
  BeeWare's testbed recipe: one `Frameworks/<dotted.module>.framework` + `Info.plist` per `.so`, a pointer file in
  place). Any mistake only shows at load time on the device. Fix: a small Python script (clearer, testable), or
  BeeWare's own tooling if it's packaged.
- **The generator's modules are reset by hand** before each run (`sys.modules` entries whose `__file__` is under
  `rando-gen/`, plus the `PySide6` stubs) because the "process" is really a thread in one interpreter. Module-level
  state in a C extension would survive between runs; a sub-interpreter (`Py_NewInterpreter`) or a fresh
  `Py_Initialize` per run would be cleaner if it works with pybind11 modules.
- **Reproducibility relies on `hash_seed = 0`** mirroring the other platforms' `PYTHONHASHSEED=0`. Not yet verified
  that one seed ID gives byte-identical output on Mac and iOS (`flags.tsv`, `logic.json`, the romfs files).
- **The self-test (`LARECOMP_IOS_PYTEST=1`, [runtime/platform/ios.c](../runtime/platform/ios.c) `plat_init`) has no automated run.** Fix: an iOS
  Simulator build in CI (the `ios` job on `macos-15`, arm64, so `arm64-apple-ios-simulator` runs natively): the
  start screen and the embedded Python, launched with `simctl` and the variable, `lasrecomp.log` from the app's
  data container checked for `[py] selftest ok` and `generator exit 0`, a `simctl io screenshot` of the start
  screen as an artifact. What it takes: a Makefile `IOS_SIM=1` (`-target arm64-apple-ios16.0-simulator`,
  `--sdk iphonesimulator`, `build/ios-sim`, `build/deps-ios-sim`, the xcframework's
  `ios-arm64_x86_64-simulator` slice, which BeeWare ships already); `build_deps_ios.sh SIM=1` (the same five
  sources built against the simulator SDK: `-mios-simulator-version-min`, `.cpython-313-iphonesimulator.so`,
  `platform-config/arm64-iphonesimulator`; Pillow's BeeWare wheel may have no simulator build - then from the
  sdist); `bundle_ios.sh` ad-hoc signed (no profile in the simulator). **The game itself is not run in the
  simulator**, by decision: its GPU side is the Mac's, not a phone's, the macOS build already exercises the game
  code, and the 12.5 GB window at the iOS base may not fit a simulator process (a separate base and a separate
  `generated-ios-sim/` for nothing). The simulator build is the start screen + the seed generator, no more.
- **`make rando-helper` (PyInstaller) and the embedded interpreter are two ways to ship the same generator.** The
  desktop helper freezes `las_headless.py`; iOS runs the sources. `tools/rando/*` and `RandomizerCore` are copied
  flat into `rando-gen/` with a `cp` list in the bundle script - a new file type there is missed.

## The export from the desktop (the iOS skeleton zip)

- **The skeleton zip is 208 MB (652 MB unpacked), most of it four linkers** (tools/ios_skeleton.sh: rust-lld for
  macOS arm64, Linux x64 / arm64, Windows x64 - 112-195 MB each; the app itself is 59 MB). One zip for every desktop
  keeps the release page simple; a per-host zip would be a quarter of the size.
- **The player unpacks the zip and picks the folder** (the start screen reads it in place: no unzip in the runtime).
  The picked folders aren't remembered across runs (settings.h has no string slots for them).
- **The keystone symlink travels as a `.symlink` file** (tools/bundle_ios.sh SKELETON=1; runtime/iosexport.c turns it
  back into a symlink entry in the .ipa) because a zip unpacked on Windows keeps no links. Any other symlink added to
  the app later needs the same.
- **The .ipa is written uncompressed** (runtime/iosexport.c: its own zip writer, store only - the runtime doesn't
  link zlib on every platform: no `-lz` on macOS / Linux / Windows, no `zlib.h` in runtime/; only the iOS build has
  it, through the static FFmpeg). 170 MB instead of ~50; fine for a file that's installed and deleted, but deflate
  would be nicer once zlib is a dependency anyway. Adding it is one `-lz` on macOS and Linux (`zlib1g-dev` in the
  AppImage container) but a new MSYS2 package plus `zlib1.dll` in the Windows zip - or miniz in third_party/ with
  no new dependency at all.
  Beyond size, a zip the runtime can *read* as well as write would remove the `.symlink` workaround above: the
  skeleton could ship the app as a ready `.ipa` (a zip keeps symlink entries on every host, Windows included) and
  the export would copy its entries through and append `Frameworks/LASGame.framework/{LASGame,LASGame.id}` -
  instead of walking an unpacked `.app` folder and rebuilding what Windows lost. Stored entries can be copied
  without inflating, so even the store-only writer could do it given a zip reader; deflate makes the shipped
  skeleton smaller too.
- **ld64.lld is Rust's `rust-lld`** (tools/fetch_lld.sh: the 60-70 MB `rustc` component of a pinned Rust release,
  sha256 published next to it, for all four hosts). Chosen over the LLVM release archives (1.4-2 GB, and their Linux
  lld needs the host's libxml2 and Ubuntu 22.04's exact ICU soname): rust-lld needs only libc / libz, plus on Linux
  its own `lib/libLLVM.so` (155 MB, found by RUNPATH `$ORIGIN/../lib`, hence the `bin/` + `lib/` layout). Verified in a
  bare `ubuntu:22.04` arm64 container: the Linux zig + Linux rust-lld produce the same `__TEXT` / `__DATA_CONST` bytes
  as the macOS toolchain (only LINKEDIT differs: STABS paths, hence the UUID). The lld version follows Rust's
  (21.1.2 for Rust 1.91), which is fine but is not something we pick.
- **rust-lld isn't in the README's "License" section.** It lists the Zig compiler's LLVM / Clang / LLD (Apache 2.0
  with LLVM exceptions), but the skeleton zip ships a second linker: LLD built by the Rust project (Apache 2.0 with
  LLVM exceptions as well, plus on Linux `libLLVM.so` under the same), fetched from Rust's release archives. Add it to
  the list (with the Rust release it comes from, as the Zig entry names its version), and ship LLVM's `LICENSE.TXT`
  next to the linker in the skeleton (`lld/<host>/`) the way `tools/zig_slim.sh` copies zig's `LICENSE`.
- **zig can't link for iOS** (0.13-0.17: segfaults or a broken libubsan sub-build for the ios target). If a later
  zig can, lld and the tbd stub go away.
- **Tests to add for what already broke once**, each found by hand on a device or a Linux box rather than by CI:
  - the exported library's id equals the skeleton executable's on every desktop: `GAMELIB_IOS_ID` from the macOS,
    Linux and Windows Makefile branches against `GAMELIB_ID` of `PLATFORM=ios` (a Linux export was rejected as
    "built for another version": host `-march` / `-fPIC` leaked into the id), and the generator gets `--simd asm`
    for iOS whatever the host (tools/test_gamebuild_ios.py can assert both from `launched`);
  - the skeleton zip itself (tools/ios_skeleton.sh): unpacks with no symlinks, the executable's ad-hoc signature
    carries `increased-memory-limit`, `lld/<host>/bin/lld -flavor darwin --version` runs on each host, `include/`
    has `any-darwin-any/stdlib.h`;
  - a quarantined linker on macOS (`xattr -w com.apple.quarantine`) still links (runtime/gamebuild.c lifts it);
  - `plat_reserve_guest` on iOS without the entitlement fails with the `[vm]` diagnostics (needs a device: a
    manual checklist in docs/ios.md at least);
  - `build_deps_ios.sh` on a clean machine (the `_iphoneos.whl` glob broke only there); CI covers this now.

## Everything else

- **Signed under the device probe's App ID** (`com.lasrecomp.probe`) and a profile found by UUID: no real bundle id,
  no documented App ID setup. [docs/ios.md] doesn't exist yet.
- **Settings > Prelaunch > Game font doesn't work on iOS.** "Import fonts" calls `SDL_ShowOpenFileDialog`, which
  SDL has no iOS backend for, so the button does nothing (the folder picker was replaced by
  [runtime/platform/ios_picker.m](../runtime/platform/ios_picker.m); the file picker wasn't). Needs the same
  `UIDocumentPickerViewController` for files (`.bfttf` / `.ttf`, multiple) behind a `show_file_dialog` in
  `launcher.mm`, and the row should say the fonts can also be dropped into the app's `fonts/` folder in Files.
- **Launched from the home screen there are no arguments / environment** (`--play`, `LARECOMP_*` only through
  `devicectl … launch`): a settings entry ("skip the start screen") would be the user-facing form of `--play`.
- **The guest base differs from the Android port's `GUEST_SHIFT`.** Two mechanisms for one problem; one has to go
  before both merge (below).
- **Untested on iPad and on devices with less than 8 GB**; the 5.5 GB measured ceiling is an iPhone 15 Pro number.
- **The 12.5 GB guest window is the desktop layout, not what the game needs.** It fits because an 8 GB iPhone gives a
  process ~59 GB of address space with or without `com.apple.developer.kernel.extended-virtual-addressing` (measured
  on a 15 Pro: identical VA range, the same 12.5 GB window at `0x8_0000_0000`, the same 48 GB single allocation), so
  that paid entitlement was dropped from [runtime/platform/ios.entitlements](../runtime/platform/ios.entitlements) and a
  free personal team signs the app. Devices with less memory are likely to have a smaller VA limit, where the window
  won't fit. The game uses about 4.5 GB of it: `HEAP_MAX` 3.25 GB (`SetHeapSize` asks for ~3 GB), the modules
  (45 MB in a 2 GB slot), HLE / TLS / HLE heap (~230 MB in 1.8 GB), 48 thread stacks (in a 1 GB `STACK_SIZE`), a few
  `svcMapMemory` copies (in a 4 GB `ALIAS_SIZE`). Fix: a `PLAT_IOS` layout in [runtime/rt.h](../runtime/rt.h) /
  [kernel.h](../runtime/kernel.h) with the regions packed (alias 512 MB, stacks 256 MB, modules + HLE 512 MB) to ~4.5 GB,
  and the probe (`tools/ios_probe`, out of tree) run on a 6 GB device to see the real limit; as a fallback the
  entitlement back on for those devices. `GetInfo` 2/3/12-15 report whatever the layout says, so the game follows.
- **The fixed base `0x8_0000_0000` doesn't fit every device: the game does not start on an iPhone17,1.** Measured on
  a virtual iPhone 17 Pro (vphone "Lime", arm64e, iOS 26.6, 16 GB given to it): the same 58 GB address space as the
  15 Pro (`0x1_0000_0000..0xF_C000_0000`, so RAM isn't it), but libmalloc's and the system's reserved regions lie in
  the middle of it there - `0x4_5800_0000..0xB_4400_0000` and `0xD_C400_0000..0xF_C000_0000` are taken (user tags 31 /
  0, no protection: reservations), leaving `0x3_0000_0000..0x4_5800_0000` (5.5 GB) and `0xC_7000_0000..0xE_F000_0000`
  (10 GB) free. On the 15 Pro everything above `0x3_0000_0000` was free. A newer SoC / OS lays the process out
  differently, and a real iPhone 17 will behave as the virtual one. The 12.5 GB window fits neither gap; the packed
  ~5.5 GB layout above fits both, at `0xC_8000_0000` (free on both devices measured) - so the packed layout is
  what makes iOS work beyond the 15 Pro, not just a saving. `plat_reserve_guest`'s failure now logs the process's
  address-space limit, its free gaps of 256 MB or more and its large regions with their tags
  ([runtime/platform/ios.c](../runtime/platform/ios.c)), so the next device reports its own map. A base chosen at
  run time would be the general answer, but the generated code bakes the base into its literals (`f_8…`, `adrp`
  results), as the Android port's `GUEST_SHIFT` does: moving it means regenerating, not relocating.
- **Fitting weaker devices (4-6 GB of RAM) is unexplored.** The 15 Pro's 2.3 GB footprint in the overworld is
  against an 8 GB phone's ~5.5 GB ceiling; a 6 GB iPhone (13 / 14, SE) has ~3-3.5 GB for a foreground app and a
  4 GB one less, with no swap. Where the footprint goes: the guest heap the game actually touches (`SetHeapSize`
  ~3 GB asked, how much is dirtied?), the renderer's second copy of every texture as `MTLTexture` (~850 MB graphics
  on the 15 Pro), the shader caches, 256 MB thread stacks (VA only, but each touched page stays). To study:
  footprint by region over a play session (`footprint`-style breakdown from `task_vm_info` / `vm_region_recurse`
  in the probe), the texture copy made purgeable or streamed, a smaller `HEAP_MAX` the game accepts, and the
  `increased-memory-limit` entitlement's effect on those devices. For the measurements without many phones:
  `viphone-cli` (virtualised iPhone with a chosen RAM size) as a test bed, if its Metal is faithful enough.

## To take from the Android port

The Android port lives in [Nycolazs/LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus)
(formerly upstream PR #91; `main`, merged up to upstream `5c7e498`). Both phone ports need the same pieces, which
there sit behind `#ifdef __ANDROID__`: they should become one shared condition (`PLAT_TOUCH` / mobile in
[runtime/platform.h](../runtime/platform.h)), not two copies. In order of value for iOS:

**Touch and phone UI (shared code, no Vulkan in it)**
- `runtime/ui/touch_ui.h` + its hooks in `launcher.mm` / `menu.mm`: a finger in the ImGui pages - tap clicks on
  lift, a drag scrolls pixel for pixel, a flick keeps going. Without it the start screen and the menu aren't usable
  by finger at all.
- `runtime/ui/touch_controls.mm` (now 704 lines) + the `input.c` hook (`touch_buttons()` / `touch_stick()`) + the
  Controls-tab settings: the on-screen controls. The rework (`8b272b8`) shows only what the game uses right now
  (B once Link has the sword, R the shield, X / Y while an item is on them; ZL / ZR never), draws the game's own item
  icons in the bubbles (`ui_game_icon` from Game.arc), taps on the game's own screens (skip the opening, L + R on the
  title, pick a file, move the Items cursor), a layout editor (drag, resize, hide; saved per control), a fixed-stick
  option, and hides them while a controller is in use. Taken as a whole, not the PR #91 version.
- Phone menu scale (`menu.mm`: a third larger, fonts built at that size), DejaVu Sans for the name entry / FPS / overlay
  text, the touch-friendlier `option_rows.mm` / `osk.mm`, `controls_ui.mm`'s touch rows.
- `launcher.mm` phone branches: the structure for "no setup on this platform" (status lines, no folder picker, no
  QUIT), the tap hints on the title and file select. Their on-device setup (SAF picker, .xci / .nsp extraction with
  prod.keys, the game code built on the phone) doesn't apply - iOS has no spawn and the .ipa carries the code.
- Side bars filled with the picture's blurred edges (`4d234da`, Video > Side bars) for interiors at the phone's
  aspect; Metal side to write.

**Input and settings (shared, platform-neutral)**
- `input.c` controller choice (`ef11b46`): the best-ranked pad wins (a known type > sticks + face buttons > anything),
  and the one whose button is pressed becomes the one read. On iOS a keyboard / mouse receiver or Siri Remote listed
  as a gamepad has the same effect as on Android.
- `settings.c` written to `larecomp.ini.tmp` then renamed, and saved when the app goes to the background
  (`ef11b46`): iOS ends backgrounded apps at any moment too. Pair with our missing background / foreground handling.

**Runtime performance (shared; measure on the phone before taking)**
- `rt_dispatch` dcache inside `Cpu` instead of `__thread` (`cpu.h` / `dispatch.c`): their motive was dlopen'ed-library
  TLS cost on Android; iOS loads the game as a dlopen'ed framework too. Upstream since took "no stack frame on the
  cached path" (`02146de`), so only the in-`Cpu` array and the slot hash remain to take. Needs regeneration
  (`CPU_RESTRICT`).
- `FN_ENTER` compiled out on phones unless `LARECOMP_FNPROF` (~10% of the game code's instructions).
- Cold functions at -O0 (`53ac961`): `tools/hot_functions.py` + `gen.py --hot` put the 108k functions a play profile
  never ran in their own files compiled at -O0 - the library builds 3x faster (their phone 26.6 -> 10.8 min) at no
  game-speed cost, 95 -> 175 MB. For us that's the desktop export's build time (the skeleton's link on the player's
  machine), worth it once `make PLATFORM=ios gen` is on the player's side.
- Render thread: the last pipeline key's hash kept, a vectorized max for the largest index, the rscale table only for
  shaders that bind it (`4988c41`, Vulkan `rqueue.c`): the first two are in the shared `rqueue.c` and apply to Metal.
- Game-speed guard (`framepace.c`: `rt_interval_choice`, `g_pace_slow`) and the "keep ~10 % GPU spare" dynamic
  resolution targets (`output_scale.c`): not needed on an A17 at 60 fps; right for older devices and throttling.
  Upstream's `28a68f8` (steady 30 fps on machines at the edge) covers part of this already.

**Widescreen**
- Interiors and dungeons rendered wide and filling the phone's aspect instead of a 16:9 box (`78f5a53`, `1bb9ea3`,
  and `40bff76` keeping their framing with the unlocked aspect): directly applicable to 19.5:9 iPhones; the Metal half
  of it is to write (`renderer.m` had +67 lines in #91).

**Shaders (Maxwell translator, shared)**
- BRX window labelling and `brx_table` reading only the table (`4988c41`, from the mamcjr port): an SSAO pass 20 ms ->
  under 1 on Adreno; Metal compiles the same translated source, so check the same pass on the A17.

**Decide, don't copy**
- Guest base: their `GUEST_SHIFT` (names stay `f_7000…`, values move; `generated/` portable between hosts) vs our
  `GUEST_BASE` (simpler; byte-identical at the default base). One mechanism before both reach upstream.
- Brazilian Portuguese (`i18n.mm`, `i18n_pt_br.h`, the fan translation as a RomFS patch): the `T()` plumbing is fine to
  share; the translation itself is their feature.

**Not applicable to iOS**: Vulkan / Turnip / SGSR, BC decode on the CPU, pre-rotated swapchain, frame generation (A17
holds 60 natively), on-device build and extraction (no spawn, no keys), root power modes, `PlayService`.
