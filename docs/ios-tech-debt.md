# iOS port: technical debt

Known shortcuts in the iOS port, with where they live and what the fix is. Add to this when taking another one.

## Embedded Python (the seed generator on the device)

Done (kept here as the record of what the build now guarantees):
- The Python version is spelled once: `PYV` in the [Makefile](../Makefile), passed to
  [tools/build_deps_ios.sh](../tools/build_deps_ios.sh) and [tools/bundle_ios.sh](../tools/bundle_ios.sh) and to the
  runtime as `LARECOMP_PY_VERSION` ([runtime/platform/ios_python.m](../runtime/platform/ios_python.m)'s `sys.path`).
  The extension suffix and the wheel platform tag are read from the cross venv's `sysconfig`, and the host
  `python$PYV` is checked to be that version (not just named so).
- The pins in `build_deps_ios.sh` are checked against the randomizer's
  [requirements.txt](../third_party/LAS-Randomizer/requirements.txt) (`req`: the script stops when a pin no longer
  satisfies the file's specifier), and the pure-Python packages (evfl, appdirs) are downloaded by the file's own
  lines. The native ones still have their own recipes with a hand-moved version: keystone, oead, quicktex, PyYAML
  (the pure-Python copy), Pillow.
- Every download is pinned by checksum, oead's sdist and BeeWare's Pillow wheel included.
- keystone's two build-file fixes (CMake 4's removed `CMP0051`, the ELF-only `-Wl,-z,defs` for an iOS target) are
  [tools/patches/keystone-0.9.2.patch](../tools/patches/keystone-0.9.2.patch), applied by the macOS and iOS deps
  scripts alike. (The `PYTHON_EXECUTABLE` / `PYTHONINTERP_FOUND` overrides stay as `-D` options: they're inputs, not
  source changes.)
- The frameworks layout is [tools/ios_frameworks.py](../tools/ios_frameworks.py), tested by
  [tools/test_ios_frameworks.py](../tools/test_ios_frameworks.py) in CI (module names, `.fwork` pointers, no symlink
  anywhere in the app, bundle ids without underscores, idempotence). keystone's library (not a Python extension:
  `keystone.py` asks ctypes for `libkeystone.dylib` by name) is a framework like the others, and the runtime
  `dlopen`s it before Python starts ([runtime/platform/ios_python.m](../runtime/platform/ios_python.m)) so ctypes
  gets the loaded image back by its install name's leaf. It used to be a relative symlink in
  `site-packages/keystone/`, kept as a symlink entry through the kit and the .ipa - and dropped by the sideloading
  tool's install (an .ipa exported on Linux, installed with SideStore: `ModuleNotFoundError: distutils` from
  keystone.py's last-resort path, i.e. the dylib not found). Nothing in the app depends on a symlink now, and the
  kit script refuses one.
- The generator's sources are `rsync`ed from `tools/rando/` (less the helper's venv / spec and caches), not a `cp`
  list of extensions.
- [tools/test_ios_python.sh](../tools/test_ios_python.sh) `<devicectl id> <udid>` runs the self-test on a device,
  checks its log and compares the seed it generated with the same seed generated here, file by file.

Still open:
- **Pillow comes from BeeWare's package index** (`https://pypi.anaconda.org/beeware/simple`), not PyPI: a third-party
  binary we don't build and whose availability we don't control (pinned by checksum now, so at worst the build
  stops). Building it from the sdist in the cross venv (its codecs static) would remove the dependency. It is also
  the one piece of the app that holds a path of someone else's machine (`/Users/runner/...` from BeeWare's CI):
  everything we compile maps its source folder away (`-ffile-prefix-map`, `tools/build_deps_ios.sh`) and
  `tools/bundle_ios.sh` refuses an app holding this machine's paths, as the desktop bundles do since beta 6.
- **BeeWare's Python-Apple-support release is a binary dependency** (`Python-3.13-iOS-support.b16`): a prebuilt
  CPython we don't rebuild. Acceptable (it's the CPython project's own iOS support, PEP 730), but a new `b`-release
  changes the ABI of every extension we build (`PYB` in `build_deps_ios.sh`: bump it and rebuild them all).
- **The generator's modules are reset by hand** before each run (`sys.modules` entries whose `__file__` or
  `__path__` is under `rando-gen/`, plus the `PySide6` stubs) because the "process" is really a thread in one
  interpreter. Verified by the self-test, which now generates the seed twice in one interpreter and compares: 291
  identical files. The check found two bugs in the reset on its first run - the bundle path differs between
  `NSBundle` (`/var/containers/…`) and the importer (`/private/var/containers/…`), so no module matched and
  nothing was reset; and `RandomizerCore.Tools` is a namespace package (no `__init__.py`, so no `__file__`) that
  survived and handed out the old `event_tools` with its `AutoEvent` counter - a second seed in one session got
  different event files than the desktop gives for the same seed string. Module-level state in a C extension
  (oead, quicktex, keystone) would still survive, none is known; a sub-interpreter (`Py_NewInterpreter`) or a fresh
  `Py_Initialize` per run would make it impossible if pybind11 modules allow it.
- ~~**Reproducibility relies on `hash_seed = 0`** mirroring the other platforms' `PYTHONHASHSEED=0`.~~ Verified: seed
  `pytest` (`{"seed": "pytest", "create-spoiler": true}`) generated on the Mac (`PYTHONHASHSEED=0`, the randomizer's
  venv, Python 3.13.7) and on an iPhone17,1 (embedded Python 3.13.16) gives 290 byte-identical files (`flags.tsv`,
  `logic.json`, `hints.*`, both `.pchtxt`, every romfs file); the only difference is `spoiler_view.txt`'s first line,
  which names the output folder. (A first run differed in `EUes/System.msbt` because the device's romfs copy lacked
  `EUes/message/Place.msbt`: `loc_text.Names` silently falls back to English names when a language's `Place.msbt`
  is missing, so an incomplete romfs changes the output without an error.)
- ~~**The self-test (`LARECOMP_IOS_PYTEST=1`) runs in CI on no device.**~~ The `ios` job now builds the app for the
  iOS Simulator too (`make ios-sim`: `IOS_SIM=1` in the Makefile, `SIM=1` in `build_deps_ios.sh` - the simulator
  SDK, `-mios-simulator-version-min`, the xcframework's `ios-arm64_x86_64-simulator` slice, BeeWare's simulator
  Pillow wheel; nothing patched) and runs the self-test in it ([tools/test_ios_sim.sh](../tools/test_ios_sim.sh):
  `simctl` boots a simulator, installs, launches with the variable, reads `lasrecomp.log` from the data container;
  the start screen's screenshot is an artifact). Without game files the generator step reports itself skipped and
  the imports are the test; `SIM_ROMFS=<dir>` by hand runs the generator twice and compares, as on a device
  (291 identical files on an iPhone 17 Pro Max simulator). What the simulator taught: it refuses an executable
  whose ad-hoc signature carries an entitlement ("Security policy issue"), so the simulator app is signed plain;
  its installer rejects a framework folder without its binary, so the empty `LASGame.framework` is left out;
  `MetalFX` is not in the simulator SDK (`metal.m` compiles the scaler out under `__has_include`); and quicktex's
  `setup.py` gives CMake no sysroot, so its module had been built with the macOS SDK and `-arch arm64` (it loaded on
  a device by luck of the shared headers) - the cross venv's `CC` / `CXX` wrappers are now passed, for the device
  build too. **The game itself is not run in the simulator**, by decision: its GPU side is the Mac's, not a
  phone's, the macOS build already exercises the game code, and the 12.5 GB window at the iOS base may not fit a
  simulator process (a separate base and a separate `generated-ios-sim/` for nothing).
- **`make rando-helper` (PyInstaller) and the embedded interpreter are two ways to ship the same generator.** The
  desktop helper freezes `las_headless.py`; iOS runs the sources (`tools/rando/` and `RandomizerCore` / `Resources`
  rsynced into `rando-gen/`).

## The export from the desktop (the iOS kit)

Closed (the `.laskit` kit, miniz fetched by tools/fetch_miniz.sh): the player no longer unpacks anything - the kit is one zip
(tools/ios_kit.sh) the runtime reads in place (runtime/zipfile.h over miniz): this host's linker, the headers and
the link stub come out once into `<data root>/cache/ios-kit/<kit id>/` (sha256 of the file; older kits' folders are
sweeps), the app ships as `Payload/LAS Recomp.app/` entries and the export clones them into the .ipa - data,
name and central-directory record, so modes come along without recompressing - and appends the library
(deflated: ~50 MB instead of 170). The app holds no symlinks (an iOS install drops them; the `.symlink` files and
the symlink entries that replaced them are both gone - keystone's library is preloaded as a framework). A
folder picked instead of the file gets a message saying so. Files the runtime writes itself carry no quarantine
attribute, so the `removexattr` on the linker is gone too.

- **The kit is 209 MB (658 MB unpacked), most of it four linkers** (rust-lld for macOS arm64, Linux x64 / arm64,
  Windows x64 - 8-122 MB compressed each; the app itself is 65 MB). One file for every desktop keeps the release
  page simple and was chosen deliberately; a per-host kit would be a quarter of the size.
- **The export's game files folder is found the way setup finds it** (runtime/romfs_build.h `romfs_find_game`:
  exefs/ and romfs/ inside, their contents in one folder, or nested), and only the exefs files are staged into
  `<data root>/cache/ios-build/` by name - nothing is written into the player's folder; "Clear build cache" removes
  that staging folder too. One build folder for whatever game folder was picked last: picking another version's
  files replaces the exefs copy and the build cache rebuilds (its inputs changed), as it should.
- **The picked kit and game files folder aren't remembered across runs** (deliberate: settings.h has no string
  slots, and the kit is picked once per release anyway).
- **Windows extraction turns a symlink into a copy of its target** (runtime/zipfile.c `zip_extract`: an unprivileged
  Windows user can't make symlinks). Nothing in the kit is a symlink today, so this is a fallback nobody hits; a
  link whose target isn't in the archive fails the unpack.
- **The two files the export adds carry no Unix mode** (stock miniz writes none; `LASGame` and `LASGame.id` come
  out `-rw-r--r--`-less, mode 0). dyld maps a library without an executable bit, so the app runs; anything added
  later that must be executable (none today) would need a way to set the mode - a 12-line addition to miniz, kept
  out so the release is used unmodified (the build fetches it like zig: network once, `build/miniz-dl` caches it).
- **ld64.lld is Rust's `rust-lld`** (tools/fetch_lld.sh: the 60-70 MB `rustc` component of a pinned Rust release,
  sha256 published next to it, for all four hosts). Chosen over the LLVM release archives (1.4-2 GB, and their Linux
  lld needs the host's libxml2 and Ubuntu 22.04's exact ICU soname): rust-lld needs only libc / libz, plus on Linux
  its own `lib/libLLVM.so` (155 MB, found by RUNPATH `$ORIGIN/../lib`, hence the `bin/` + `lib/` layout). Verified in a
  bare `ubuntu:22.04` arm64 container: the Linux zig + Linux rust-lld produce the same `__TEXT` / `__DATA_CONST` bytes
  as the macOS toolchain (only LINKEDIT differs: STABS paths, hence the UUID). The lld version follows Rust's
  (21.1.2 for Rust 1.91), which is fine but is not something we pick.
- **rust-lld isn't in the README's "License" section.** It lists the Zig compiler's LLVM / Clang / LLD (Apache 2.0
  with LLVM exceptions), but the kit ships a second linker: LLD built by the Rust project (Apache 2.0 with LLVM
  exceptions as well, plus on Linux `libLLVM.so` under the same), fetched from Rust's release archives. Add it to
  the list (with the Rust release it comes from, as the Zig entry names its version), and ship LLVM's `LICENSE.TXT`
  next to the linker in the kit (`lld/<host>/`) the way `tools/zig_slim.sh` copies zig's `LICENSE`. miniz (MIT,
  build/miniz/LICENSE after tools/fetch_miniz.sh) belongs in that section too.
- **zig can't link for iOS** (0.13-0.17: segfaults or a broken libubsan sub-build for the ios target). If a later
  zig can, lld and the tbd stub go away.
- **Tests to add for what already broke once**, each found by hand on a device or a Linux box rather than by CI:
  - the exported library's id equals the kit executable's on every desktop: `GAMELIB_IOS_ID` from the macOS,
    Linux and Windows Makefile branches against `GAMELIB_ID` of `PLATFORM=ios` (a Linux export was rejected as
    "built for another version": host `-march` / `-fPIC` leaked into the id), and the generator gets `--simd asm`
    for iOS whatever the host (tools/test_gamebuild_ios.py can assert both from `launched`);
  - the kit itself: release.yml checks its layout (Payload/, no symlink entries, keystone's framework, four linkers, headers,
    stub); still unchecked: the executable's ad-hoc signature carries `increased-memory-limit` (ci.yml checks the
    .app, not the kit) and `lld/<host>/bin/lld -flavor darwin --version` runs on each host after extraction
    (tools/test_gamebuild_ios.py links with it, which covers the host CI runs on);
  - `plat_reserve_guest` on iOS without the entitlement fails with the `[vm]` diagnostics (needs a device: a
    manual checklist in docs/ios.md at least);
  - `build_deps_ios.sh` on a clean machine (the `_iphoneos.whl` glob broke only there); CI covers this now.

## Everything else

- **No `docs/ios.md`.** The app has its own bundle id now (`com.peiikai.lasrecomp.ios`, tools/bundle_ios.sh; a
  sideloading tool rewrites it per team), but nothing documents the setup for a player: the kit, the export, the
  sideloading tools that keep `increased-memory-limit`, where the files go, which devices and iOS versions run it.
- **Launched from the home screen there are no arguments / environment** (`--play`, `LARECOMP_*` only through
  `devicectl … launch`): a settings entry ("skip the start screen") would be the user-facing form of `--play`.
- **SDL 3.4.18 is patched for iOS 27** ([tools/patches/SDL3-3.4.18-ios27-orientation.patch](../tools/patches/SDL3-3.4.18-ios27-orientation.patch),
  SDL's own commits ca97578 and 2ee7eb6 from its release-3.4.x branch): on iOS 27 `statusBarOrientation` is a no-op,
  so SDL took the window for portrait, kept its view at 393x852 under a landscape 852x393 window, and the game drew
  into the top-left of the screen (SDL issue 16436; an iPhone 15 Pro on 27.0.1). The patch goes once
  `tools/build_deps_ios.sh` moves to SDL 3.4.20 or later, where both are in.
- **The guest base differs from the Android port's `GUEST_SHIFT`.** Two mechanisms for one problem; one has to go
  before both merge (below). `GUEST_ADDR(<offset>)` (next item) makes the generated code base-free, which is what
  `GUEST_SHIFT` wanted too.
- **Untested on iPad and on devices with less than 8 GB**; the 5.5 GB measured ceiling is an iPhone 15 Pro number.
  Run so far on an iPhone 15 Pro and a virtual iPhone 17 Pro (vphone "Lime", iOS 26.6), both at 60 fps.
- **The window is one 5.25 GB piece, placed where the process has room - and iOS 27 may have none.** Where an iOS
  process has free address space differs by device and version: on iOS 26 the gap above `0x3_0000_0000` (an 8 GB
  iPhone 15 Pro and a 16 GB iPhone 17 Pro alike, 58 GB of address space with or without
  `com.apple.developer.kernel.extended-virtual-addressing`), on iOS 27.0.1 (a 15 Pro) the dyld shared cache
  (tag 32) spans `0x2_EA00_0000..0xF_C000_0000` - over every gap iOS 26 had - and the gaps under it are reported
  under 4 GB, with room only above it, which only `extended-virtual-addressing` (a paid team) opens: 507 GB, a 64 GB
  gap at `0x70_0000_0000`. So no fixed base works across versions, and the generated code no longer has one: it
  writes every address as `GUEST_ADDR(<offset>)` ([runtime/cpu.h](../runtime/cpu.h)), the desktops fold it to their
  compile-time base (the same machine code as before), and the iOS build (`LARECOMP_DYNAMIC_BASE`) adds a base
  chosen at start-up - `plat_place_guest` ([runtime/platform/ios.c](../runtime/platform/ios.c)) walks the map and
  takes the smallest free gap below 2^39 that holds the window (the 5.5 GB one on iOS 26). Measured on macOS in the
  overworld: no CPU cost (game thread 4.1 vs 4.2-4.3 ms a frame, within the noise), the library 3% larger. One
  generated tree for every platform, functions named by the Switch address (`f_7000…`) everywhere. What's left:
  the regions placed separately (the heap's 3.25 GB is the only piece that must be contiguous - the modules and HLE
  memory, the 1 GB of thread stacks and the 512 MB alias region could each go in a gap of their own, which is what a
  map of sub-4 GB gaps calls for), and if the gaps are smaller still, a smaller `GetInfo(6)` so the game asks for a
  smaller heap - whether it plays on 2-2.5 GB is untested. A failure logs the map (into `lasrecomp.log`).
- **4 GB devices don't fit; the heap size is not what decides it.** `phys_footprint` (the `mem` figure of the
  `[metal]` stats line, what jetsam goes by) in 2.5 minutes of the overworld: 1.8-1.9 GB on an iPhone 15 Pro,
  2.4 GB on macOS. A 4 GB iPhone (SE 3, 11, 12 mini) gives a foreground app ~2 GB with `increased-memory-limit`,
  ~1.4 GB without; a 6 GB one ~3 GB. The guest heap is address space, not memory: the game asks `SetHeapSize` for
  `GetInfo(6)` minus 64 MB (3220 MB as on hardware) and plays the same down to 1856 MB reported (1792 MB heap) -
  below that it fails at boot, its fixed arenas leaving a negative size for the last one - with the same footprint
  at every size measured (2.39 GB median at 3220, 2048, 1920 and 1856). So a smaller heap only helps placing the
  window (a 2 GB heap: a ~4 GB window, or a 2 GB piece once the regions are placed separately). What the
  footprint is: the textures twice - the game's block-linear copies in guest memory and ours as `MTLTexture`
  (~465 MB of them in the overworld) - render targets at the output size, shader caches, and the game's own data.
  The biggest saving is the guest copy of each texture once it is on the GPU (released while the game doesn't read
  it back; whether it ever does is to be checked), then rendering at the screen's own size on small devices.
  Measuring on a 4 GB device: a virtual iPhone ("Lime") can be given 4 GB, once it runs past the title screen
  (its paravirtual GPU has no BC formats).
- **`increased-memory-limit` changes the address space too, not only the memory limit.** Signed without it, an
  iPhone 15 Pro (iOS 26.5) gives the window a gap of 5.5 GB instead of 52 GB - the packed 5.25 GB layout fits with
  ~128 MB to spare - and plays the overworld at 60 fps with a 1.8-1.9 GB footprint. The comment in
  [runtime/platform/ios.entitlements](../runtime/platform/ios.entitlements) (the same address space with or without
  `extended-virtual-addressing`) is about that other entitlement. iOS 27 without `increased-memory-limit` is
  unmeasured: with it, the only gap that holds the window is above the shared cache.

## To take from the Android port

The Android port lives in [Nycolazs/LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus)
(formerly upstream PR #91; `main`, merged up to upstream `5c7e498`). Both phone ports need the same pieces, which
there sit behind `#ifdef __ANDROID__`: they should become one shared condition (`PLAT_TOUCH` / mobile in
[runtime/platform.h](../runtime/platform.h)), not two copies.

Taken:
- `runtime/ui/touch_ui.h` (a finger in the ImGui pages: a tap clicks on lift, a drag scrolls, a flick keeps going)
  and its hooks in `launcher.mm` / `menu.mm`.
- `runtime/ui/touch_controls.mm`, the reworked one (`8b272b8`): only what the game uses right now, the game's own
  item icons, taps on its own screens, the layout editor (saved as `touch_ctl_<name>`), hidden while a controller is
  in use; the `input.c` hook and the Controls-tab settings (`touch_mode`, `touch_opacity`, `touch_size`).
- The launcher's "no setup on this platform" structure, as iOS branches (`PLAT_IOS`: no build, no data-folder move,
  the folder picker by `runtime/platform/ios_picker.m`), and DejaVu Sans for the overlay text.
- The phone menu scale (`d9e0d71`: the menu and the start screen's panels a third larger, in the safe area), the
  on-screen keyboard's touch form for the name entry (`bc8cfd8`, `osk.mm`), and the tap hints on the title and the
  file select ("Tap to start", "Tap a file to play", in `touch_controls.mm`).
- `larecomp.ini` written to `.tmp` then renamed, and saved when the app goes to the background (with the game paused
  there, `metal.m`).
- BRX reading only its jump table (upstream took it from the same source: #237, `brx_table_labels`), and the rscale
  table only for shaders that use it (Metal had it already: `uses_rscale`).

Still open, in order of value for iOS:

**Touch and phone UI**
- One shared condition instead of `PLAT_IOS`: the touch code is behind `PLAT_IOS` here and `__ANDROID__` there.
- `option_rows.mm` as one column of cards on phones (the name, its description under it, the value with its arrows
  on the right) instead of rows with the description in a pane beside them - small to read and to touch.
- Side bars filled with the picture's blurred edges (`4d234daurs still opens the first one listed and keeps it (the
  "controller in use" check there only hides the touch controls). On iOS a keyboard / mouse receiver or Siri Remote
  listed aspect; Metal side to write.

**Input**
- `input.c` controller choice (`ef11b46`): the best-ranked pad wins (a known type > sticks + face buttons > anything),
  and the one whose button is pressed becomes the one read. On iOS a keyboard / mouse receiver or Siri Remote listed
  as a gamepad has the same effect as on Android.

**Runtime performance (shared; measure on the phone before taking)**
- `rt_dispatch` dcache inside `Cpu` instead of `__thread` (`cpu.h` / `dispatch.c`): their motive was dlopen'ed-library
  TLS cost on Android; iOS loads the game as a dlopen'ed framework too. Upstream since took "no stack frame on the
  cached path" (`02146de`), so only the in-`Cpu` array and the slot hash remain to take. Needs regeneration
  (`CPU_RESTRICT`).
- `FN_ENTER` compiled out on phones unless `LARECOMP_FNPROF` (~10% of the game code's instructions).
- Cold functions at -O0 (`53ac961`): `tools/hot_functions.py` + `gen.py --hot` put the 108k functions a play profile
  never ran in their own files compiled at -O0 - the library builds 3x faster (their phone 26.6 -> 10.8 min) at no
  game-speed cost, 95 -> 175 MB. For us that's the desktop export's build time on the player's machine (the kit's
  zig compiling the whole library), the longest st (a memcmp instead of hashing ~700 bytes per draw) and a vectorized
  max for the largest index (`4988c41`): to measure on Metal, whose draw path differs from their Vulkan oner
  shaders that bind it (`4988c41`, Vulkan `rqueue.c`): the first two are in the shared `rqueue.c` and apply to Metal.
- Game-speed guard (`framepace.c`: `rt_interval_choice`, `g_pace_slow`) and the "keep ~10 % GPU spare" dynamic
  resolution targets (`output_scale.c`): not needed on an A17 at 60 fps; right for older devices and throttling.
  Upstream's `28a68f8` (steady 30 fps on machines at the edge) covers part of this already.

**Widescreen**
- Interiors and dungeons rendered wide and filling the phone's aspect instead of a 16:9 box (`78f5a53`, `1bb9ea3`,
  and `40bff76` keeping their framing with the unlocked aspect): directly applicable to 19.5:9 iPhones; the Metal half
  of it is to write (`renderer.m` had +67 lines in #91).

**Shaders (Maxwell tran(`4988c41`, from the mamcjr port): a BRX whose table can't be read labels only the window
  between it and the join of the PBK / SSY around it, not every instruction (`brx_table_labels` returning false still
  does `memset(label, 1, end)`) - an SSAO pass 20 ms -> under 1 on Adreno;88c41`, from the mamcjr port): an SSAO pass 20 ms ->
  under 1 on Adreno; Metal compiles the same translated source, so check the same pass on the A17.

**Decide, don't copy**
- Guest base: their `GUEST_SHIFT` (a compile-time shift, names stay `f_7000…`) vs ours, now `GUEST_ADDR(<offset>)`
  in the generated code (names `f_7000…` too; a compile-time base on the desktops, one chosen at start-up on iOS).
  Ours covers theirs - Android could take the dynamic base as well - but one mechanism before both reach upstream.
- Brazilian Portuguese (`i18n.mm`, `i18n_pt_br.h`, the fan translation as a RomFS patch): the `T()` plumbing is fine to
  share; the translation itself is their feature.

**Not applicable to iOS**: Vulkan / Turnip / SGSR, BC decode on the CPU, pre-rotated swapchain, frame generation (A17
holds 60 natively), on-device build and extraction (no spawn, no keys), root power modes, `PlayService`.
