# Android

The Android port is coming in pieces, from [mamcjr's LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid)
(MIT), with parts from [Nycolazs's LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus)
(MIT). The player picks their game files folder with Android's own folder picker, the setup copies them in and builds
the game code on the device, and the game plays with touch controls or a controller. The latest APK is on the
[Experimental builds](https://github.com/peiikai/LASRecomp/releases/tag/experimental) page.

The floor is a Snapdragon 865 (Adreno 650) or as fast, 8 GB of RAM, at 720p. Older Adreno phones ship a Vulkan 1.1
driver and need a Turnip driver package (Settings > System); the README names the one we tested last.

Nothing from the game ships in the app. The game code is built on the device from the player's own extracted game
files, as on the desktop.

## Building

Needs the Android SDK with NDK 30.0.16248370, CMake 3.30.4 and platform 36, JDK 17 or newer, bash, curl and Python 3.

```
bash android/deps/build_deps.sh        # SDL3, shaderc, volk, Vulkan headers, FFmpeg for arm64 (once)
bash android/deps/build_gen_deps.sh    # the game code builder and seed generator: Python, its modules, zig (once)
cd android && ./gradlew assembleDebug  # android/app/build/outputs/apk/debug/app-debug.apk
```

`./gradlew assembleRelease` makes the release build. Android installs an update only over an app signed with the same
key, so the published APKs are all signed with one fixed key (`LARECOMP_KEYSTORE`, `LARECOMP_KEYSTORE_PASSWORD`,
alias `lasrecomp`; `.github/workflows/experimental.yml` takes it from the repository's secrets). Never make a new one:
every player would have to uninstall, and lose the app's data folder, to update. `LARECOMP_VERSION_NAME` and
`LARECOMP_VERSION_CODE` set the version; the code has to grow from build to build (the workflow uses minutes since
2026-01-01).

CMake's file lists are globs, read once: after a pull that adds a source file, delete `android/app/.cxx` or the link
fails (or, worse, Gradle quietly keeps the old library).

## How it differs from Linux

- `runtime/platform/linux.c` builds for Android too (`__ANDROID__` parts): the log goes to logcat (tag `LASRecomp`)
  as well as `lasrecomp.log`, backtraces come from the unwinder, and the process can't restart itself.
- The guest window moves (`LARECOMP_DYN_BASE`, `runtime/rt.h`): an app process is forked with its libraries already
  mapped, so `runtime/main.c` picks a free spot from `/proc/self/maps`.
- The data folder is `Android/data/com.lasrecomp.android/files`, in internal storage or on an SD card (Settings >
  Storage, and the first row of the setup window): the app's own folder on each, the only places it can use by path
  without a permission. The SD card's is named in `data_root.txt` as on the desktop, and the data moves with the
  desktop's move (`runtime/paths.c`); the activity starts the app again after it. The fonts and icon are copied from
  the APK to the app's internal storage at start, where `rt_resource` finds them.
- Every Vulkan function comes through volk: Android's loader exports only Vulkan 1.1 before API 33.
- The swapchain is pre-rotated: phone and handheld panels are portrait underneath, so the images are made in the
  panel's orientation and everything drawn into them is turned (`present.vert`, the menus, the menu glass, the
  screenshot readback). Otherwise the compositor turns every frame on the GPU (CLIENT composition with ROT_90 in
  `dumpsys SurfaceFlinger`). `LARECOMP_VK_PRETRANSFORM=0` restores the old way.
- The Vulkan driver can be the system's or a driver package the player picks (Settings > System), loaded through
  libadrenotools (`runtime/nvn/vulkan/android_driver.c`). Only Adreno has packages (Turnip): a replacement driver has
  to talk to the phone's own kernel driver. A package that offers no GPU falls back to the system's driver.
- Phones under 10 GB of RAM don't import the guest heap; shader memory goes through copies. The CPU and GPU still
  overlap on that path (the GPU's stores reach guest memory a frame later, and the CPU's writes win where both wrote).
- Some drivers list no BC6H / BC7 (Samsung's Xclipse): those textures are decoded on the CPU, as ASTC is in the
  emulator. The other per-GPU defaults are in [vulkan-gpu-defaults.md](vulkan-gpu-defaults.md).
- Dynamic resolution counts the GPU as paced from the start (`runtime/nvn/output_scale.c`): a phone's GPU clock drops
  until the frame just fits, so its GPU time reads about 90% of the frame at any resolution and can't predict a step.
  Dynamic goes by late frames instead, and the resolution that went late is a ceiling for a while.
  `LARECOMP_DYN_PACED=0` uses the desktop's rules. The thermal state that iOS feeds it (its heat rules) isn't read on
  Android yet.
- The game files folder comes from Android's folder picker (a `content://` folder, not a path): the setup copies
  exefs and romfs through the storage framework into the data folder.
- The game code is built on the device as on the desktop (`runtime/gamebuild.c`): the generator runs on Python for
  Android, and zig compiles for `aarch64-linux-musl` and links against `libmain.so`. Android runs programs only from
  the app's native library folder, so Python's launcher and zig ship there as `lib*.so`; the rest is unpacked from
  `assets/gen.zip` at the first start after an install or update. The library is loaded from a copy in internal
  storage, since shared storage is mounted noexec.
- The randomizer's seed generator runs the same way: Python runs `tools/rando/las_headless.py` from the unpacked files
  (PyInstaller, which makes the desktop's `rando-gen`, has no Android target). `build_gen_deps.sh` builds its compiled
  modules (oead, quicktex, Pillow's core, Keystone) for that Python; they load from internal storage like Python's own.
  Shared storage has no symlinks, so the room shuffle's scratch folder goes in internal storage
  (`LARECOMP_RANDO_SCRATCH`). A seed takes about five minutes on a Snapdragon 865.

## Testing in the emulator

An arm64 system image (API 35, Google APIs) with host GPU runs the start screen on a Mac with Apple Silicon:

```
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.lasrecomp.android/com.lasrecomp.LASRecompActivity
adb logcat -s LASRecomp
```

The desktop's debug switches ([debug-hooks.md](debug-hooks.md)) are passed with the launch (debug builds only):
`adb shell am start -n com.lasrecomp.android/com.lasrecomp.LASRecompActivity --es LARECOMP_LOG_SKIPS 1`.

The emulator draws the start screen and the movies, but not the game's world: its Vulkan goes to the Mac through
gfxstream and MoltenVK, whose translation of the game's shaders to Metal fails to compile ("call to 'fmin' is
ambiguous"), so those draws are skipped. A phone's GPU compiles them directly; the game itself runs there.

The emulator's keyboard can't hold a key: each key event from the host arrives as a press with its release a few
microseconds later (and again on every key repeat), so held keys such as WASD walk in tiny steps. A real keyboard on
a phone holds keys normally.

## Testing on a device

```
adb install -r -d android/app/build/outputs/apk/release/app-release.apk   # -d: allows an older version code
```

An install keeps the data folder (settings, saves, the built game code). The game code is rebuilt only when its code ID
changes (`android/app/.cxx/RelWithDebInfo/*/arm64-v8a/build.ninja` names it), so most updates start at once.

Launch switches work only in debuggable builds. A muted test launch with its own settings and saves:

```
adb shell am start -S -n com.lasrecomp.android/com.lasrecomp.LASRecompActivity \
  --es LARECOMP_SETTINGS <ini> --es LARECOMP_SAVE_DIR <dir> \
  --es LARECOMP_NO_AUDIO 1 --es LARECOMP_NO_MOVIE_AUDIO 1 --es LARECOMP_NO_LAUNCHER 1
```

`adb shell input keyevent` doesn't reach the game. `lasrecomp.log` is in the data folder (on an SD card:
`/storage/<card>/Android/data/com.lasrecomp.android/files/LAS Recomp`); its `[dynres]` and `[pace]` lines say what
Dynamic and the frame pacer did and why.

## Tested devices

Retroid Pocket 5 (Snapdragon 865, Adreno 650, Turnip 26.3.0 R6):

- 60 FPS target: Dynamic with a 360p minimum. It averages 540p-756p, and needs 360p in heavy areas such as the swamp.
- 30 FPS target: Dynamic. It averages 972p-1080p; Dynamic is needed because 1080p can't quite be held at times.
