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

- **rust-lld isn't in the README's "License" section.** It lists the Zig compiler's LLVM / Clang / LLD (Apache 2.0
  with LLVM exceptions), but the kit ships a second linker: LLD built by the Rust project (Apache 2.0 with LLVM
  exceptions as well, plus on Linux `libLLVM.so` under the same), fetched from Rust's release archives. Add it to
  the list (with the Rust release it comes from, as the Zig entry names its version), and ship LLVM's `LICENSE.TXT`
  next to the linker in the kit (`lld/<host>/`) the way `tools/zig_slim.sh` copies zig's `LICENSE`. miniz (MIT,
  build/miniz/LICENSE after tools/fetch_miniz.sh) belongs in that section too.
- **Tests to add for what already broke once**, each found by hand on a device or a Linux box rather than by CI:
  - ~~the exported library's id equals the kit executable's on every desktop~~: tools/test_gamebuild_ios.py reads
    `GAMELIB_IOS_ID` / `GENFLAGS_IOS` from the macOS, Linux and Windows (x86-64 and aarch64) Makefile branches
    against `PLATFORM=ios`'s, checks gamebuild.c's fallback flags against them, and from `launched`: no host flag in
    the compiles, `--simd asm` to the generator;
  - the kit itself: release.yml checks its layout (Payload/, no symlink entries, keystone's framework, five linkers, headers,
    stub) and the executable's signature carrying `increased-memory-limit`; still unchecked:
    `lld/<host>/bin/lld -flavor darwin --version` runs on each host after extraction (tools/test_gamebuild_ios.py
    links with it, which covers the host CI runs on);
  - `plat_reserve_guest` on iOS without the entitlement fails with the `[vm]` diagnostics (needs a device: a
    manual checklist in docs/ios.md at least). The start screen now tells the player the signature lacks
    `increased-memory-limit` (`plat_ios_memory_entitled`, `LARECOMP_TEST_NO_MEMORY_ENTITLEMENT=1` for the no case on
    any desktop), which covers the message, not the placement failure;
  - `build_deps_ios.sh` on a clean machine (the `_iphoneos.whl` glob broke only there); CI covers this now.
- **The iOS strings have no translations.** Upstream's menus are in 11 languages (`runtime/ui/lang/*.inc`,
  `tools/menu_strings.py check`); the ~29 strings the port adds (touch controls, the kit page, the Files picker, the
  entitlement warning) show in English everywhere. `tools/menu_strings.py missing <code>` lists them; nothing in CI
  asks for them.

## Everything else

- **No `docs/ios.md`.** The app has its own bundle id now (`com.peiikai.lasrecomp.ios`, tools/bundle_ios.sh; a
  sideloading tool rewrites it per team), but nothing documents the setup for a player: the kit, the export, the
  sideloading tools that keep `increased-memory-limit`, where the files go, which devices and iOS versions run it.
- ~~**Launched from the home screen there are no arguments / environment**~~: the Home Screen quick actions are
  the player's `--play` (`ios_glue.m`): "Play Vanilla" (static, Info.plist), then one dynamic item per seed (up to
  two: iOS shows four in all), the seed PLAY would start first, then the newest, then "Metal HUD" - dynamic too,
  since iOS puts static items before dynamic ones. A launch from one goes through the start screen's PLAY (the
  first-launch questions, the shader preparation); in game it reboots on that seed. The seed items are rebuilt from
  `rando/` (`rando_seed_there`: `settings.json` + `romfs/`, as the start screen's list) when the start screen
  opens, when the Randomizer tab re-reads its list (a seed made, renamed or deleted) and when the app goes to the
  background; a seed deleted through the Files app in between leaves the start screen up. `LARECOMP_*` stays
  `devicectl`-only.
- ~~**Left / right values have no touch gesture.**~~ A row's `◀ value ▶` arrows are tap targets (larger on the
  phone's cards, `option_rows.mm`), and the start page's seed choice is `‹ [choice] ›` buttons that also take a
  sideways swipe (`launcher.mm`). A swipe on an option row still scrolls rather than steps (touch_ui.h: a sideways
  drag only moves strips made to scroll that way); the arrows are enough.
- **SDL 3.4.18 is patched for iOS 27** ([tools/patches/SDL3-3.4.18-ios27-orientation.patch](../tools/patches/SDL3-3.4.18-ios27-orientation.patch),
  SDL's own commits ca97578 and 2ee7eb6 from its release-3.4.x branch): on iOS 27 `statusBarOrientation` is a no-op,
  so SDL took the window for portrait, kept its view at 393x852 under a landscape 852x393 window, and the game drew
  into the top-left of the screen (SDL issue 16436; an iPhone 15 Pro on 27.0.1). The patch goes once
  `tools/build_deps_ios.sh` moves to SDL 3.4.20 or later, where both are in.
- **Untested on devices with less than 8 GB**; the 5.5 GB measured ceiling is an iPhone 15 Pro number. Run so far on
  an iPhone 15 Pro and a virtual iPhone 17 Pro (vphone "Lime", iOS 26.6), both at 60 fps. iPad: the UI takes the
  monitor's scale there and the touch controls 0.6 of the phone's size (`plat_ios_tablet`, `23f3ee7`), set from a
  simulator's screen - the game itself has not run on an iPad.
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
- **Textures on GPUs without BC, and the two copies of every texture, are one problem; the two answers worked out
  so far don't fit together.** The facts first. Metal has the BC formats on Apple7 (A14) and later only; the
  iPhones without them (8, X, XR / XS, 11, SE 2) are exactly the 3-4 GB ones, so whatever replaces BC there has to
  save memory, not spend it. The game's textures (a scan of every BNTX in the 1565 bfres / arc / bntx files, all
  mips): 16 824 BC textures, 3.66 GB - BC5 1.6 GB (4275, 3969 of them snorm normal maps), BC4 1.17 GB (7843
  masks), BC1 0.8 GB (3946), BC3 82 MB, BC7 8.5 MB (22 models: the village houses' interiors, the library, some
  enemies). No texture has an ASTC twin (the UI archives hold ASTC for some layout images and BC for others, never
  both of one). An overworld scene holds ~350 textures; the game loads per area and frees on leaving (texture pool
  slots emptied), and the CPU never writes into a BC texture after loading it (measured: the only CPU-written texture
  is the R8 glyph cache; GPU writes go to uncompressed render targets) - our own texpatch does, re-encoding the
  changed blocks to BC before upload.
  Branch `ios-bc-decode` (not merged) decodes BC on the CPU into RGBA8 at upload (`runtime/nvn/bcdec.c`, every
  format within 1 of the Apple GPU's own decoder, BC3 / BC4 / BC7 identical; the anchor tables and the 6-bit
  interpolation weights were read back from the hardware). It gets Lime past the title screen and is correct, and it
  is the wrong answer for the devices that need it: RGBA8 is 4-8x the BC size, ~6x over the game's mix, so the
  `MTLTexture` half of the footprint goes from ~465 MB to ~2.8 GB in the overworld - on a phone with ~2 GB for the
  app. The Android port does the same (`bcn.c` over third_party/bcdec, BC4 -> R8, BC5 -> RG8 snorm: 2-4x less than
  our RGBA8 on masks and normals, worth taking regardless) and has the same problem on Mali / PowerVR.
  What was measured for the compressed alternatives (all on real game textures, the Mac GPU as the reference):
  - BC1 -> ASTC 4x4 is a bit rearrangement (same endpoints, same per-texel index: both decoders use the 6-bit
    weights {0,21,43,64}), exact to +-1 - never 0, the two formats round the last step differently and the endpoints
    are also texels, so no choice of endpoints fixes it - for 97% of the BC1 textures (the rest have 3-colour blocks
    with the transparent index); 2x the BC1 size. BC4 8-step blocks the same way (42% of the BC4 textures are all
    such blocks); 2x. BC3, BC5, BC7 have no direct form (independent channel palettes, multi-partition modes).
  - astcenc 4x4 -medium: BC1 69.7 dB / max 1, BC3 71 dB / max 1, BC4 69.6 dB / max 2, BC5 59 dB / max 8 (normals
    are the weak point); 6x6 for BC1 / BC4 gives size parity with BC at 52-54 dB / max 11-12. 70 Mtexel/s per M-series
    core, so the whole game's 5.8 Gtexel is a minute on a phone - but BC5 snorm has no ASTC LDR form at all: an
    offset into unorm and a `*2-1` in every shader that reads a normal map (the translator could add it by the
    bound texture's format), which is the shader change decided against.
  - EAC: `EAC_R11Unorm/Snorm` and `EAC_RG11Unorm/Snorm` exist on Lime (Apple5-class paravirtual GPU) and on the 15
    Pro (probed, textures created and filled), 8 / 16 bytes a block like BC4 / BC5, signed natively - the way out for
    the 12 000 masks and normals without touching shaders. No encoder written yet; the format is one-dimensional
    (base, multiplier, one of 16 tables, 3-bit indices), the speed of a reasonable one is unknown and decides
    whether it runs at upload (JIT) or needs a disk cache.
  - Decoding BC in the shader (BC blocks in a uint texture, unpacked per fetch) keeps BC's size exactly, and costs
    8 fetches + 8 block decodes + the filtering by hand per sample, at 7.5 samples a fragment on average (75
    fragment shaders of an overworld run, 564 samples, peaks of 25-34); ruled out.
  - Combined, the best mix found: BC1 -> ASTC 4x4 by transcode, BC3 / BC7 -> ASTC by astcenc (90 MB in all), BC4 ->
    EAC R11, BC5 -> EAC RG11(Snorm): +17% GPU memory over BC, every format sampled in hardware, no shader change,
    and a content-hash cache (the hash `upload_texture` already computes) so nothing is encoded twice - JIT with a
    cache, an AOT pass being the same code run over romfs if first visits stutter. Vulkan would take the same module
    at textures.c's unsupported-format branch, so Android gets it too.
  Why the two don't fit yet: all of that reduces the `MTLTexture` copy, while the "4 GB devices" item above says the
  big saving is the *guest* copy - releasing the game's block-linear bytes once the texture is on the GPU (the
  other session's options: `madvise` / `PROT_NONE` with a fault handler that blits back on access, GPU-copy eviction,
  GPU-side unpacking off the guest heap buffer). On an A14+ device the guest copy is BC and the GPU copy is BC; both
  schemes stack and the footprint halves. On a pre-A14 device the GPU copy becomes the transcoded form, and then:
  (1) a cache miss or an eviction needs the BC source back - from guest memory that was released, or from the disk
  cache that must then hold *every* visited texture, not just the slow ones, which makes the cache mandatory rather
  than an optimisation and makes its hash key (computed from guest bytes) unavailable when the guest bytes are
  gone; (2) a fault-handler blit-back reconstructs BC from an `MTLTexture` that is now ASTC / EAC, which cannot be
  undone exactly, so the hash changes and texpatch's re-encode no longer sees the original blocks; (3) encoding
  at upload (JIT) holds BC source + decoded + encoded at once per texture for the duration, on the device with the
  least headroom. None of these is unsolvable - keep the key as (file, offset) from the loader instead of a content
  hash; evict only textures whose cache entry is on disk; encode from the file rather than from guest memory - but
  each pulls the design away from "a transcode step inside `upload_texture`" towards a texture manager that owns
  the source, and that is the design to make before writing the encoder: decide where the BC bytes of a loaded
  texture live on a 4 GB phone (guest memory, disk cache, or nowhere because the file is the source), and derive
  both the transcode and the guest-copy release from that. Until then the decoder branch stays unmerged and Lime
  runs it from a local stash ("lime: dumb BC->RGBA8 decoder …": `git stash show -p --include-untracked stash@{N} |
  patch -p1` over any tree, `patch -p1 -R` to take it off; three files, nothing else in it).
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
- `option_rows.mm` as cards on phones (`ae914a6`: the name, its description under it, the value and its arrows on
  the right, moved below when wide), and the tab bar scrolling sideways when the tabs don't fit.
- BRX reading only its jump table (upstream took it from the same source: #237, `brx_table_labels`), and the rscale
  table only for shaders that use it (Metal had it already: `uses_rscale`).

Still open, in order of value for iOS:

**Touch and phone UI**
- One shared condition instead of `PLAT_IOS`: the touch code is behind `PLAT_IOS` here and `__ANDROID__` there.
- Side bars filled with the picture's blurred edges (`4d234da`, Video > Side bars) for interiors at the phone's
  aspect; Metal side to write.

**Runtime performance (shared; measure on the phone before taking)**
- `rt_dispatch` dcache inside `Cpu` instead of `__thread` (`cpu.h` / `dispatch.c`): their motive was dlopen'ed-library
  TLS cost on Android; iOS loads the game as a dlopen'ed framework too. Upstream since took "no stack frame on the
  cached path" (`02146de`) - which on Apple doesn't hold: the `blr` to dyld's TLV getter keeps the `stp` / `ldp`
  frame (`otool -tv build/ios/lasrecomp`, `_rt_dispatch`). Measured (iPhone 15 Pro, Field, `xctrace` Time Profiler
  15 s): `rt_dispatch` 7.0% of the game thread's self time, the TLV descriptor load + `blr` + frame 1.5-3% of it;
  the game thread 4.1 ms of a 16.7 ms frame - so at most ~0.1 ms a frame, for a regeneration (`CPU_RESTRICT`) and a
  new library id. Not worth it while the thread has that headroom. Their slot hash is taken (`dcache_slot`,
  `(pc >> 4) ^ (pc >> 14)`: 98% of the functions start 16-byte aligned, so `pc >> 2` reached 2261 of the 4096
  slots): the misses' share of the game thread 2.7 -> 1.7% on the iPhone, 2.7 -> 2.0% on an M-series Mac.
- Game-speed guard (`framepace.c`: `rt_interval_choice`, `g_pace_slow`) and the "keep ~10 % GPU spare" dynamic
  resolution targets (`output_scale.c`): not needed on an A17 at 60 fps; right for older devices and throttling.
  Upstream's `28a68f8` (steady 30 fps on machines at the edge) covers part of this already.

**Widescreen**
- Interiors and dungeons rendered wide and filling the phone's aspect instead of a 16:9 box (`78f5a53`, `1bb9ea3`,
  and `40bff76` keeping their framing with the unlocked aspect): directly applicable to 19.5:9 iPhones; the Metal half
  of it is to write (`renderer.m` had +67 lines in #91).

**Shaders (Maxwell translator, shared)**
- BRX window labelling (`4988c41`, from the mamcjr port): a BRX whose table can't be read labels only the window
  between it and the join of the PBK / SSY around it, not every instruction (`brx_table_labels` returning false still
  does `memset(label, 1, end)`) - an SSAO pass 20 ms -> under 1 on Adreno; Metal compiles the same translated source,
  so check the same pass on the A17.

**Not applicable to iOS**: Vulkan / Turnip / SGSR, BC decode on the CPU, pre-rotated swapchain, frame generation (A17
holds 60 natively), on-device build and extraction (no spawn, no keys), root power modes, `PlayService`; the render
thread's pipeline-key cache and index maximum (`4988c41`, Vulkan `exec.c`): Metal hashes only the fields in use (~90
bytes a draw, 22-26 draws a frame on the phone) and binds index buffers in place, so it never scans them.
