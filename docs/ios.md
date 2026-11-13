# iPhone and iPad

## For players

### What you need

- iOS or iPadOS 16.4 or newer.
- iPhone 15 Pro or newer, iPad Pro (2021) or newer, iPad Air (2022) or newer, or iPad mini (2024) or newer.
- A Mac, Windows or Linux computer with LAS Recomp set up and your extracted game files.

A device whose graphics chip lacks the game's texture formats stops at start with "This iPhone or iPad can't run
LAS Recomp: its graphics chip has no support for the game's texture formats (BC)."

### Making the app

1. On your computer, download `LAS Recomp-<version>-iOS.laskit` from the same release as your LAS Recomp.
2. On the start screen, open Settings > System > Recomp for iOS..., choose the kit, then Export iOS app.
3. The app is saved to your Downloads folder as `LAS Recomp-<version>-iOS.ipa`.

The .ipa holds the game code built from your own files. Don't share it.

### Installing it

Install the .ipa with a sideloading tool. With a free Apple account the app has to be refreshed every 7 days. We
can't help with the tools themselves, since they're made by others.

#### Increased Memory Limit

LAS Recomp asks iOS for more memory than a normal app gets. iOS only grants it when the tool that signs the app also
turns on Increased Memory Limit for the app's App ID. Not every tool does. Without it, iOS may close the game when it
needs more memory.

| How the app is installed | Keeps it |
|---|---|
| AltStore Classic 2.2 or newer | Yes |
| [Impactor](https://github.com/claration/impactor), from a computer | Yes |
| Xcode, with a free (Personal Team) or paid Apple account | Yes |
| SideStore 0.7.0 with a free Apple account | No, see below |
| Sideloadly and other tools | Check the start screen |

If the limit is missing, the start screen says "Installed without the increased memory limit". The `[memory]` line
in `lasrecomp.log` also shows `increased-memory-limit yes` or `NO`.

**If your sideloader didn't add Increased Memory Limit:** you can add it to the app's App ID once with GetMoreRam.
SideStore with a free Apple account is one known case ([SideStore#1616](https://github.com/SideStore/SideStore/issues/1616)).
1. Install LAS Recomp with your sideloader as usual, so its App ID exists.
2. Sideload [GetMoreRam](https://github.com/hugeBlack/GetMoreRam) and sign in with the same Apple ID that your
   sideloader uses.
3. In GetMoreRam, go to App IDs, tap Refresh, tap LAS Recomp's App ID, then Add Increased Memory Limit.
4. Install LAS Recomp again with your sideloader. It updates in place, so your saves stay.

The limit stays on the App ID, so it survives the weekly refresh. If the start-screen warning comes back, repeat
steps 3 and 4. GetMoreRam takes one of a free account's three app slots, so delete it afterwards.

### Your game files

Copy your game files folder (with `exefs` and `romfs` in it) to the device:
- Mac: connect it with a cable, select it in Finder's sidebar, open the Files tab and drag the folder onto LAS Recomp.
- Windows: the same in the Apple Devices app (or iTunes), under Files > LAS Recomp.
- USB-C drive: put the folder on the drive and plug it in.
- iCloud Drive: upload the folder, then in the Files app move it to On My iPhone (or On My iPad). LAS Recomp won't
  use game files that are still on iCloud Drive.

Open the app and choose that folder when it asks. Use the same game version as on your computer.

### Where things are

Saves, seeds, settings and `lasrecomp.log` are in the app's folder in Files (On My iPhone / On My iPad > LAS Recomp).
You can move them to another folder or a drive in Settings > System > Data folder. For a bug report, attach
`lasrecomp.log` from that folder.

Long-press the app icon for Play Vanilla, up to two of your seeds, and the Metal HUD.

### Performance

Dynamic resolution is on by default. A phone that gets warm runs slower after a few minutes, so Dynamic lowers the
resolution to keep 60 fps and raises it again when the phone cools down. Frame Generation (Settings, Experimental)
works on 120 Hz displays.

## For developers

What is still open or worth knowing. Fixed items are in git history.

### Embedded Python (the seed generator)

- **A script is a process only as far as [runtime/platform/ios_pyproc.py](../runtime/platform/ios_pyproc.py) makes
  it one.** The generator's `os.chdir` and its re-run of itself through `subprocess` work unchanged, with these
  shortcuts:
  - The working directory is the thread's, through `pthread_chdir_np` / `pthread_fchdir_np` (in libSystem since
    iOS 10, in no SDK header). A thread already running when another calls `chdir` stays where it was, and a `chdir`
    from C code still moves the whole app.
  - A child runs on the calling thread to its end before `Popen` returns: no pipe to a running child, no two at once,
    and each waiting parent keeps its memory. Only this Python (`sys.executable`) with a script, `-m` or `-c` runs.
  - The environment is swapped through `os.environ._data`, a private field of CPython's `os` module.

  [tools/test_ios_pyproc.py](../tools/test_ios_pyproc.py) (CI) and the self-test's `process` step (CI's Simulator
  run) fail when one of these stops holding.
- **The generator's modules are reset by hand.** `ios_pyproc.py` drops every module under `rando-gen` (by real path)
  and the PySide6 stubs from `sys.modules` before a run; the C extensions stay loaded. A sub-interpreter per run was
  tried (b2013266) and taken back: oead and quicktex are pybind11 modules without `multiple_interpreters` support,
  their types and pybind11's internals are process-wide, and CPython tightens the rules with every version. The reset
  needs its two cases kept right (`/var` vs `/private/var` paths, the `RandomizerCore.Tools` namespace package); the
  self-test's kill check covers them. `build_deps_ios.sh` pins pybind11 at what oead vendors and fails when the two
  modules' `__pybind11_internals_vN` differ.
- The native packages (keystone, oead, quicktex, PyYAML, Pillow) still have their own recipes in
  `build_deps_ios.sh` with a hand-moved version.
- [tools/test_ios_python.sh](../tools/test_ios_python.sh) `<devicectl id> <udid>` runs the self-test on a device and
  compares a generated seed with the same seed generated on the computer. An incomplete romfs changes the output
  without an error (`loc_text.Names` falls back to English when a language's `Place.msbt` is missing).

### Address space

- **The window is one 5.25 GB piece, placed where the process has room.** Free address space differs by device and
  iOS version. On iOS 26 there is a gap above `0x3_0000_0000`. On iOS 27.0.1 (iPhone 15 Pro) the dyld shared cache
  spans `0x2_EA00_0000..0xF_C000_0000`, the gaps under it are under 4 GB, and the only room is above it. So the iOS
  build is a `DYN_BASE=1` build (`generated-dyn/`, the window laid out from `GUEST_LO`), and `plat_place_guest`
  ([runtime/platform/ios.c](../runtime/platform/ios.c)) takes the smallest free gap below 2^39 that holds it, 1 GB
  aligned. A failure logs the map into `lasrecomp.log`.
- Open: placing the regions separately. Only the heap's 3.25 GB must be contiguous; the modules and HLE memory, the
  1 GB of thread stacks and the 512 MB alias region could each go in a gap of their own. If gaps get smaller still,
  a smaller `GetInfo(6)` makes the game ask for a smaller heap; whether it plays on 2-2.5 GB is untested.
- **`increased-memory-limit` changes the address space too.** Signed without it, an iPhone 15 Pro on iOS 26.5 gives
  the window a 5.5 GB gap instead of 52 GB (the 5.25 GB layout fits with ~128 MB to spare) and plays the overworld at
  60 fps with a 1.8-1.9 GB footprint. `extended-virtual-addressing` doesn't change this on iOS 26 (the comment in
  [runtime/platform/ios.entitlements](../runtime/platform/ios.entitlements)); on iOS 27 it opens 507 GB but needs a
  paid team. On iOS 27.0.1 without `increased-memory-limit`, an iPad Pro M4 (8 GB) still placed the window above
  the shared cache (`0x70_0000_0000`), gave a 5120 MB limit instead of 8192, and played the overworld at 60 fps for
  4 minutes with a 2.7 GB peak footprint. An iPhone on iOS 27 without it is not measured.
- The start screen's notice (`plat_ios_memory_entitled`, `LARECOMP_TEST_NO_MEMORY_ENTITLEMENT=1` to see it on any
  desktop) covers the message, not a placement failure. `plat_reserve_guest` without the entitlement has no test.

### Memory and textures

- **4 GB devices don't fit; the heap size is not what decides it.** `phys_footprint` (the `mem` figure of the
  `[metal]` line, what iOS goes by) in the overworld: 1.8-1.9 GB on an iPhone 15 Pro, 2.4 GB on macOS. A 4 GB iPhone
  gives a foreground app ~2 GB with `increased-memory-limit`, ~1.4 GB without; a 6 GB one ~3 GB. The game asks
  `SetHeapSize` for `GetInfo(6)` minus 64 MB (3220 MB) and plays the same down to 1856 MB (1792 MB heap), with the
  same footprint at every size (2.39 GB median at 3220, 2048, 1920 and 1856). So a smaller heap only helps placing the
  window. The footprint is the textures twice (the game's copies in guest memory and ours as `MTLTexture`, ~465 MB in
  the overworld), render targets at the output size, shader caches and the game's own data. The biggest saving is
  releasing the guest copy of each texture once it is on the GPU; then rendering at the screen's own size on small
  devices.
- **Textures without BC, and the two copies of every texture, are one problem.** Metal has BC on every Apple9 GPU
  (A17 Pro, M3) and newer, on some Apple7 / Apple8 ones (`supportsBCTextureCompression` says which) and on none
  before. Which A14-A16 and M1 / M2 devices have it is not measured; nobody has confirmed an M1 iPad yet.
  - The game's textures (every BNTX, all mips): 16 824 BC textures, 3.66 GB. BC5 1.6 GB (4275, 3969 of them snorm
    normal maps), BC4 1.17 GB (7843 masks), BC1 0.8 GB (3946), BC3 82 MB, BC7 8.5 MB. No texture has an ASTC twin.
    An overworld scene holds ~350 textures; the game loads per area and frees on leaving, and the CPU never writes a
    BC texture after loading it (only texpatch does, re-encoding to BC before upload).
  - Branch `ios-bc-decode` (not merged) decodes BC to RGBA8 at upload (`runtime/nvn/bcdec.c`, within 1 of the Apple
    GPU's decoder). Correct, but RGBA8 is ~6x the game's mix: the `MTLTexture` half goes from ~465 MB to ~2.8 GB on a
    phone with ~2 GB for the app. The Android port's BC4 -> R8 and BC5 -> RG8 snorm are 2-4x smaller and worth
    taking regardless.
  - Measured alternatives: BC1 -> ASTC 4x4 by bit rearrangement, exact to +-1, for 97% of BC1 textures (2x size);
    BC4 8-step blocks likewise (42% of BC4 textures); astcenc 4x4 -medium: BC1 69.7 dB, BC3 71 dB, BC4 69.6 dB,
    BC5 59 dB / max 8 (normals are the weak point), 70 Mtexel/s per M-series core; BC5 snorm has no ASTC LDR form
    without a shader change (decided against). EAC R11 / RG11 (Unorm and Snorm) exist on the paravirtual GPU and the
    15 Pro, the size of BC4 / BC5 and signed natively; no encoder written yet. BC decode in the shader costs ~8 fetches
    and decodes per sample at 7.5 samples a fragment: ruled out.
  - Best mix found: BC1 -> ASTC 4x4 by transcode, BC3 / BC7 -> ASTC by astcenc, BC4 -> EAC R11, BC5 -> EAC RG11 Snorm.
    +17% GPU memory over BC, all sampled in hardware, no shader change, with a content-hash cache.
  - Why it isn't built: those cut the `MTLTexture` copy, while the big saving is releasing the guest copy. On a
    device without BC, a cache miss or eviction then needs the BC source from released guest memory or from a disk
    cache that must hold every visited texture; a blit-back from ASTC / EAC can't give the original BC; and encoding
    at upload holds three copies at once on the device with the least headroom. The design to make first: where the
    BC bytes of a loaded texture live on a 4 GB phone (guest memory, disk cache, or the file as the source). Both the
    transcode and the guest-copy release follow from that.
  - Testing without BC: the app stops at start unless launched with `LARECOMP_NO_BC_CHECK=1`.

### Dependencies

- **SDL 3.4.18 is patched for iOS 27**
  ([tools/patches/SDL3-3.4.18-ios27-orientation.patch](../tools/patches/SDL3-3.4.18-ios27-orientation.patch), SDL's
  ca97578 and 2ee7eb6): on iOS 27 `statusBarOrientation` is a no-op, so SDL kept a portrait view under a landscape
  window (SDL issue 16436). Drop the patch when `tools/build_deps_ios.sh` moves to SDL 3.4.20 or later.

### Data folder

- An iCloud Drive data folder gets a warning and a system box at launch when files are offloaded; a read of an
  offloaded file waits for its download (menu sounds 7.4 s instead of 10 ms). No `NSFileCoordinator` around the
  game's reads, and other providers (Google Drive, Dropbox) aren't told apart from On My iPhone.
- A move in the background freezes with the app (no `beginBackgroundTask`). A move killed halfway is untested.

### From the Android port

The Android port is [Nycolazs/LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus). Still worth
checking:
- `rt_dispatch` dcache inside `Cpu` instead of `__thread`: on Apple the TLV getter keeps a stack frame. Measured on an
  iPhone 15 Pro: at most ~0.1 ms a frame, for a regeneration and a new library id. Not worth it while the game thread
  has headroom (4.1 ms of 16.7).
- BRX window labelling (`4988c41`): an SSAO pass went from 20 ms to under 1 on Adreno. Check the same pass on an A17.

### Tried and declined

- Interiors at the phone's aspect and side bars of the picture's blurred edges: ported to Metal and checked on all
  three game versions, not worth it (the wide interiors cost a fifth of the render's lines at 19.5:9). Kept on the
  branch `ios-wide-interiors-bars`.
- Not applicable to iOS: Vulkan / Turnip / SGSR, a pre-rotated swapchain, on-device builds (no spawn), root power
  modes. Metal hashes only the pipeline fields in use and binds index buffers in place, so the Vulkan render thread's
  key cache and index scan don't apply.
