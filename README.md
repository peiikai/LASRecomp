# LAS Recomp

> [!IMPORTANT]
> **This project was made almost exclusively with AI.** Nearly all of the code, tooling, reverse engineering and
> documentation in this repository was written by and will be maintained by an AI (Anthropic's Claude, through Claude Code),
> directed, tested and reviewed by a human. As a huge fan of this game, I made this for myself. Sharing it is a courtesy.
> If you don't agree with the use of AI or don't trust code written by AI, that is your right, but I'm not interested
> in the controversy in relation to this project. You may simply ignore this project and its code.
> As with anything on the internet, you run the code at your own discretion.

**LAS Recomp** is a static recompilation of *The Legend of Zelda: Link's Awakening* (Nintendo Switch, 2019) into a
native Windows, macOS, Linux, Android and iOS application.

> [!NOTE]
> Currently supported platforms:
> - macOS - Apple Silicon only, Metal graphics backend + MetalFX
> - Windows - x86-64 only, Vulkan graphics backend
> - Linux - x86-64, Vulkan graphics backend (AppImage)
> - Android - arm64, Vulkan graphics backend - **experimental**: the latest APK is on the [Experimental builds](https://github.com/peiikai/LASRecomp/releases/tag/experimental) page
> - iPhone and iPad - arm64, Metal graphics backend (sideloaded: see [iPhone and iPad](#iphone-and-ipad))
> - Linux aarch64 (ARM) - **experimental**: the latest build is on the [Experimental builds](https://github.com/peiikai/LASRecomp/releases/tag/experimental) page; reports welcome
>   ([docs/linux.md](docs/linux.md) lists what to include)

## System requirements

These are the minimums during the beta and may change as development continues.

**macOS:** any Apple Silicon Mac (M1 or newer) on macOS 15 or newer. Intel Macs aren't supported.

**Windows and Linux:**

| | Minimum |
|---|---|
| OS | Windows 10 or 11 (64-bit), or 64-bit Linux from about 2022 or newer (including the Steam Deck) |
| CPU | x86-64 with AVX2 and FMA: Intel 4th gen (Haswell) or newer, or AMD Ryzen |
| GPU | Vulkan 1.3 with an up-to-date driver, about as fast as a Switch or faster. For example: NVIDIA GTX 900 series or newer, AMD Radeon RX 400 series or Ryzen integrated graphics or newer, Intel Iris Xe or newer |
| RAM | 8 GB (16 GB recommended) |
| Disk | About 12 GB free for setup |

**Android:**

| | Minimum |
|---|---|
| OS | Android 11 or newer, 64-bit (arm64) |
| Chip | Snapdragon 865 (Adreno 650) or as fast, for example the Retroid Pocket 5 |
| GPU driver | Vulkan 1.3. Older Adreno phones ship a Vulkan 1.1 driver: pick a Turnip driver package in Settings > System. The latest we've tested is [Turnip 26.3.0 R6](https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/tag/v37) (`Turnip_v26.3.0-R6.zip`) from StevenMXZ's Adreno-Tools-Drivers |
| RAM | 8 GB |
| Storage | About 15 GB free |

> [!TIP]
> We recommend turning on Dynamic resolution and choosing an FPS target. The resolution then adjusts to what your device
> can handle, and drops when the chip heats up and slows down, which most phones and tablets do over longer play.

**iPhone and iPad:**

| | Minimum |
|---|---|
| OS | iOS or iPadOS 16.4 or newer |
| Device | iPhone 15 Pro or newer<br>iPad Pro (2021) or newer<br>iPad Air (2022) or newer<br>iPad mini (2024) or newer |
| Storage | About 15 GB free |

> [!TIP]
> We recommend turning on Dynamic resolution and choosing an FPS target. The resolution then adjusts to what your device
> can handle, and drops when the chip heats up and slows down, which most phones and tablets do over longer play.

![Mabe Village in a 21:9 window: the unlocked aspect ratio shows more of the island](docs/ultrawide.jpg)

> [!NOTE]
> **No game files are included.** LAS Recomp needs your own legally obtained copy of the extracted game files of
> *Link's Awakening* **v1.0.0, v1.0.1 or v1.1.0**. When setting up, choose a folder that contains the `exefs`
> and `romfs` folders, or their contents. Instructions on how to obtain these files will **NOT** be provided.
> On first launch the app sets up what it needs, including the actual recompilation locally. No game files, assets or fonts are included.
> The screenshot shows the start screen after setup; no Nintendo artwork is included until provided by the user.

![The start screen: choose vanilla or a randomizer seed, set up your game files, settings](docs/start_screen.jpg)

## Project Discord
https://discord.gg/DhrgdYqdcY

## Status

This is a beta release. That means bugs are likely, crashes could occur, randomizer seeds could possibly be unsolvable (dungeon room shuffle modes are especially experimental at this point) and some graphics issues remain (especially in wide and ultrawide aspect ratios). This is a work in progress, hence the beta designation. To report a bug or a possible randomizer logic issue, open a [GitHub issue](https://github.com/peiikai/LASRecomp/issues) - see [CONTRIBUTING.md](CONTRIBUTING.md#reporting-bugs) for what to include.

### Known issues

Known issues and their status are on the [GitHub issues page](https://github.com/peiikai/LASRecomp/issues).

## Steam Deck

- Add the AppImage to Steam as a non-Steam game and play it from there. In Desktop Mode, games Steam didn't launch
  get Steam's desktop controller layout, which sends extra keyboard presses.
- If it won't start, add this to the game's launch options in Steam: `APPIMAGE_EXTRACT_AND_RUN=1 %command%`

## iPhone and iPad

The iOS app is made on your computer from your own game files, then sideloaded.

1. Set up LAS Recomp on a Mac, Windows or Linux computer as usual.
2. From the same release, download `LAS Recomp-<version>-iOS.laskit`.
3. On the start screen, open Settings > System > Recomp for iOS..., choose the kit, then Export iOS app.
   The app is saved to your Downloads folder as `LAS Recomp-<version>-iOS.ipa`.
4. Install the .ipa with a sideloading tool such as SideStore, AltStore or Sideloadly. A free Apple account works,
   but the app then has to be refreshed every 7 days. LAS Recomp only makes the .ipa. Installing it is up to you,
   and we can't help with these tools since they're made by others. The extra steps come from Apple's limits on
   installing apps, not from LAS Recomp.
5. Copy your game files folder (with `exefs` and `romfs` in it) to the phone or iPad:
   - Mac: connect it with a cable, select it in Finder's sidebar, open the Files tab and drag the folder onto
     LAS Recomp.
   - Windows: the same in the Apple Devices app (or iTunes), under Files > LAS Recomp.
   - USB-C drive: put the folder on the drive and plug it in (iPhone 15 or newer, USB-C iPads).
   - iCloud Drive: upload the folder, then in the Files app move it to On My iPhone (or On My iPad). LAS Recomp
     won't use game files that are still on iCloud Drive.
6. Open the app and choose that folder when it asks for your game files. Use the same game version as the files
   on your computer.

## Features

**Runs natively**
- **This is not emulation.** It runs natively on Windows, macOS and Linux.
- Works with game versions 1.0.0, 1.0.1 and 1.1.0.
- Wider screens show more of the island, up to ultrawide. You can switch back to the original 16:9.
- Higher resolutions, a "match your display" option and dynamic resolution, with MetalFX (Mac) or FSR 1 (Windows and Linux) upscaling and SMAA anti-aliasing.
- Shaders are prepared ahead of time, so new areas don't stutter.
- A start screen where you pick vanilla or a randomizer seed, set up your game files and choose your settings.
- In-game menu: F1, or Home or Minus + Plus on a controller. The controller buttons can be changed in Controls.
- Switch between vanilla and any randomizer seed without restarting.
- Keyboard and controller support, with every button remappable. Button prompts match your controller (Xbox, PlayStation or Nintendo).
- Controller rumble, and the light bar on PlayStation controllers shows Link's health.
- Controllers that aren't recognised can be added with a mapping file (see [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)).
- Play in English, Japanese, Spanish, French, German, Italian, Dutch, Russian, Korean or Chinese (Simplified or Traditional). The menus, randomizer, tracker and spoiler log are in your language too, and Auto follows your system language.
- Optional extras: 360-degree movement, blur removal, mirror mode, faster walking and hiding the Hero Mode icon.
- Cosmetics: change Link's tunic, hair, skin and shield colours, the colour of the grass, leaves and sand (with season presets), the water, fences, walls and houses, give enemies random colours, or play with a Game Boy, Game Boy Color or grayscale filter.
- Optional run timer, with LiveSplit support.

**Randomizer**
- In-game seed generator, with seed IDs for sharing and separate saves for each seed.
- Shuffles all items, the trade quest, bombs and Magic Powder.
- Place as many bombs at once as you like.
- Hint Sheet: every seed starts with one that points you to your next important item.
- Escape Bottle: every seed starts with it. Use it to warp to the dungeon entrance, or to Marin and Tarin's house outside dungeons.
- Pickup text says which dungeon a key or dungeon item belongs to.
- Extra places items can hide:
  - Potsanity: every pot. Unchecked pots are gold.
  - Bushsanity: every bush. Unchecked bushes are gold.
  - Digsanity: every diggable spot on the overworld, about 5,000. Holes stay dug.
  - Shellsanity: every Secret Seashell hiding spot.
  - Underwater items: sunken treasures you dive for.
  - Shopsanity: the shop's items. Buy them or steal them.
  - Trendsanity: every Trendy Game prize.
  - Keysanity: keys for any dungeon can be anywhere.
- Small items from bushes, pots and digging don't stop you with the item scene; important items still do.
- Item placement is paced, so big seeds don't hand you every key item right away.
- Pets: companions are items you find. The Rooster helps with Feather checks, Marin opens the desert, and the Ghost unlocks his grave. Switch or send them away from the pause menu.
- Boss souls and mini-boss souls: a boss won't appear until you find its soul.
- Triforce Hunt: collect Triforce pieces instead of waking the Wind Fish.
- Enemy Randomizer: shuffles enemies, optionally with mini-bosses (including Shadow Link) on the overworld. Enemies can change every time you enter an area.
- Hookshot anything: the hookshot grabs onto any wall or object.
- Environment and music randomizers. Acorn and Piece of Power drops can be turned off.
- Entrance Randomizer: shuffles dungeons, houses, caves and connectors, with a decoupled option.
- Dungeon Room Shuffle: every dungeon is rebuilt from its own rooms each seed.
  - Coupled: going back through a door takes you back where you came from.
  - Decoupled: going back through a door takes you somewhere else.
  - The map can show how rooms connect as you explore.
- A tracker on the pause menu's map shows how many checks are left in each area and whether you can reach them.
- The inventory screen shows your keys, dungeon items, trade items and souls for each dungeon.
- Race mode: the spoiler log stays locked until the credits, and the title screen shows a seed hash so racers can check they have the same seed.

## Building from source

Every platform needs your own game files (v1.0.0, v1.0.1 or v1.1.0): build first, then use the start screen's game-file setup.
`make gen` then recompiles the game code from your own game files into `generated/`, and `make` builds it into its own
library next to the app (`build/lasgame.*`); without it the start screen builds that library itself, as it does for
players. Release builds hold none of the game's code: the start screen builds it on each player's machine from their
own game files, with the bundled generator (`make gen-helper`) and compiler (`tools/fetch_zig.sh`).

### macOS

Requirements: Apple Silicon, Xcode command line tools (clang), [Homebrew](https://brew.sh) `sdl3` and `ffmpeg`,
Python 3.12.

1. Build: `make -j10` -> `build/lasrecomp`. Set up your game files (above), then `make gen` and `make -j10` again.
2. Randomizer: create the Python environment for LAS-Randomizer, from `third_party/LAS-Randomizer`:
   `python3.12 -m venv venv && venv/bin/pip install -r requirements.txt`.
3. Run: `./build/lasrecomp extracted` (start screen) or `./build/lasrecomp extracted --play` (straight into the game).
4. Package: `make deps` (once: SDL3, FFmpeg, keystone and Python from pinned sources, built for macOS 15),
   `make rando-helper` (the seed generator as a standalone helper, so players need no Python), `make gen-helper` and
   `tools/fetch_zig.sh` (what the start screen builds the game code with), then `make app` ->
   `build/LAS Recomp.app` (macOS 15+, Apple Silicon) and `make dmg` -> `build/dist/LAS Recomp-<version>-macOS.dmg`.

### Windows

Requirements: Windows 10+ x86-64, a CPU with AVX2 and a GPU with Vulkan 1.3. More detail in
[docs/BUILD-windows.md](docs/BUILD-windows.md). Windows on ARM (experimental) builds with the same setup script; see
that page.

1. One-time setup, in PowerShell from the repo root: `powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1`.
   It installs MSYS2 (UCRT64: clang, make, SDL3, FFmpeg, Vulkan, shaderc), Python 3.12 for the tools and the
   randomizer (`third_party\LAS-Randomizer\venv`) and switches the clone to LF line endings.
2. Build, from an MSYS2 UCRT64 shell (or Git Bash with `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`):
   `make -j` -> `build/lasrecomp.exe`. Set up your game files (above), then `make gen` and `make -j` again.
3. Run: `./build/lasrecomp.exe extracted`, from a shell with `C:\msys64\ucrt64\bin` on PATH (for the DLLs).
4. Package: `make deps-windows` (once: LGPL FFmpeg), `make rando-helper RANDO_PY="<path to python.org Python 3.12>"`
   (MSYS2's Python has no oead wheel), then `make dist` -> `build/dist/LAS Recomp-<version>-windows.zip`.

### Linux

Requirements: x86-64 with AVX2 (or aarch64, experimental), a GPU with Vulkan 1.3, clang, SDL 3, FFmpeg, shaderc and
Vulkan Memory Allocator from your distribution, Python 3.12. Packages, cross builds and details in
[docs/linux.md](docs/linux.md).

1. Build: `make -j` -> `build/lasrecomp`. Set up your game files (above), then `make gen` and `make -j` again.
2. Run: `./build/lasrecomp extracted`.
3. Package: `make appimage` (this machine's arch; `ARCH=aarch64` cross-compiles) or `make appimages` (both) ->
   `build/dist/LAS Recomp-<version>-linux-<arch>.AppImage`, built in Ubuntu 22.04 containers (podman). `make smoketest`
   checks the x86-64 AppImage before a release.

Where files live (game files, saves, seeds, settings and logs):
- macOS: `~/Library/Application Support/com.peiikai.lasrecomp/`
- Windows: next to `lasrecomp.exe`, so keep the `LAS Recomp` folder somewhere writable (not Program Files).
- Linux: `~/.local/share/lasrecomp/`
- Android: `Android/data/com.lasrecomp.android/files` in the phone's storage.
- iPhone and iPad: the app's folder in Files (On My iPhone / On My iPad > LAS Recomp).
- You can move it to another folder or drive in Settings > System > Data folder.
- `LARECOMP_DATA_ROOT=<dir>` overrides all of these.
- For a bug report, attach `lasrecomp.log` from that folder.

### Android

Requirements: the Android SDK with NDK 30.0.16248370, CMake 3.30.4 and platform 36, JDK 17 or newer, bash, curl and
Python 3. Details in [docs/android.md](docs/android.md).

1. Dependencies (once): `bash android/deps/build_deps.sh` and `bash android/deps/build_gen_deps.sh`.
2. Build: `cd android && ./gradlew assembleDebug` -> `android/app/build/outputs/apk/debug/app-debug.apk`.
3. Install it on the phone and set up your game files from the start screen. The game code is built on the phone.

### Resuming game-code builds

If the game-code build is cancelled or interrupted, **Resume game code** continues where it stopped. It also resumes
by itself the next time LAS Recomp starts. **System > Clear build cache** removes the build's leftover files
without touching the installed game, your game files or your saves.

## Credits

**Contributors** - thank you to everyone who has sent code (GitHub handles, in order of first contribution):
- **[@dvilelaf](https://github.com/dvilelaf)** - the System language setting (#15), a packaging fix (#10) and the
  level-transition crash fix (#34)
- **[@Tensorlong](https://github.com/Tensorlong)** - the fix for the white Link on M1 Macs (#18)
- **[@mansarip](https://github.com/mansarip)** - support for unrecognised controllers (#26)
- **[@we11adam](https://github.com/we11adam)** - the right font for each language (#27) and the Chinese language
  codes (#25)
- **[@Maganra](https://github.com/Maganra)** - faster graphics checks on Linux (#42)
- **[@hjqcan](https://github.com/hjqcan)** - game addresses in one place (#57), settings and Traditional Chinese fixes
  (#53), much less memory used on Windows (#107), Vulkan fixes (#108, #127, #128), an x86 rounding fix (#115), movie
  memory fixes (#112), Windows logs (#114), resumable game-code builds (#56), and save fixes (#169, #170, #182)
- **[@gitarre1234rr5](https://github.com/gitarre1234rr5)** - controller rumble (#104) and a regression test (#88)
- **[@rkammoun](https://github.com/rkammoun)** - the fix for the black walls in the Ukuku Prairie cave on Mac (#215, #216)
- **[@NevesHenry](https://github.com/NevesHenry)** - a clear message on Linux when no file window can open (#298),
  the rumble test running in CI (#299), Linux builds finding their own SDL3 (#437), font re-imports that no longer
  empty the font (#490), a message box on Linux for a CPU without AVX2 (#491), the build and system in the log bot's
  summary (#582), an FFmpeg licence check that really checks (#583), the GPU driver in the log bot's summary (#588),
  each attached log counted once (#589), the game files check in the log bot's summary (#600), summaries for
  logs attached by editing a comment (#601), a Linux AppImage build that stops when its container fails (#653), and
  a zig download that starts over after being cut off (#667)
- **[@ky1vstar](https://github.com/ky1vstar)** - a much faster Linux ARM release build (#319), touch menus and on-screen controls for phones (#634), the frame interpolation presenter on CADisplayLink (#649), one shared macOS and iOS host layer (#685), the iOS cleanup after #238 (#697), iOS fixes for saves, errors and Files (#690), a safer, simpler iOS kit export (#704), and seed generation on iOS that shows its progress and handles room shuffle re-runs (#735)
- **[@theb0rg](https://github.com/theb0rg)** - the fix for AMD integrated graphics on Windows (#335)
- **[@gtf-dot](https://github.com/gtf-dot)** - the game-text font on Linux (#330)
- **[@cfcosta](https://github.com/cfcosta)** - full speed on Linux AMD handhelds like the Steam Deck and ROG Ally (#392)
- **[@gsel001](https://github.com/gsel001)** - a check in CI that the Mac app starts (#420), and rupees up to 9,999 in the
  cheats and save editor (#453)
- **[@wimw85](https://github.com/wimw85)** - much faster game code setup on the Steam Deck and other PCs (#434)
- **[@missionfloyd](https://github.com/missionfloyd)** - a much smaller game code build, without debug info (#487)
- **[@wolfchimneyrock](https://github.com/wolfchimneyrock)** - full speed on ARM Mali graphics (#527), the CPU and GPU working in parallel on Mali (#529), and faster render passes on Mali (#528, #534), a cheaper FSR upscale (#532), the game on the big cores of ARM and Intel hybrid CPUs on Linux (#533), no more flickering grass and shadows on Mali (#546), the game starting on Snapdragon (Adreno) devices (#561), much faster shaders on Mali and Adreno (#562, #563, #579), faster graphics memory on Mali with Mesa (#564), GPU tuning tools for Vulkan (#565), no more memory leak on Arm's Mali driver (#607), smaller shader stacks that keep 4 GB Mali boards from running out of memory (#645), and less shader scratch memory on PanVK (#646)
- **[@bgevko](https://github.com/bgevko)** - an option to keep the area's music during Guardian Acorns and Pieces of Power (#642)

**Mobile ports** - special thanks to the people who brought the game to phones and tablets:
- Android: **[@Nycolazs](https://github.com/Nycolazs)** ([LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus))
  and **[@mamcjr](https://github.com/mamcjr)** ([LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid)), the
  Android ports our Android app is built from
- iOS: **[@ky1vstar](https://github.com/ky1vstar)** - the game on iPhone and iPad (#238)

Thanks also to everyone who reports issues and tests builds. Your logs and screenshots find most of the bugs.

**LAS-Randomizer** - the randomizer at the heart of this project's randomizer features, vendored (with local
changes) in `third_party/LAS-Randomizer` under the MIT license. Huge thanks to its authors and community:
- **Owen_Splat** - LAS-Randomizer (https://github.com/Owen-Splat/LAS-Randomizer)
- **Glan** - created the original early builds of the randomizer (https://github.com/la-switch/LAS-Randomizer)
- **ProfessorLaw** - additional programming
- and everyone who has reported bugs, given feedback or contributed to LAS-Randomizer

**Community mods** - some options re-implement ideas from these GameBanana mods (re-implemented in the recomp;
none of their files are used):
- **360 Controls (And More!)** by Owen_Splat - 360-degree movement, blur removal (blur removal patch by theboy181)
  (https://gamebanana.com/mods/659802)
- **XBOX UI** by fellow_demo - Xbox button prompts (https://gamebanana.com/mods/540069)
- **PS4 UI Buttons** by messiahgov - PlayStation button prompts (https://gamebanana.com/mods/324458)
- **No Hero Mode Indicator** by Boo/Calla - hide the Hero Mode icon (https://gamebanana.com/mods/441172)

**Art**
- **[Kenney](https://kenney.nl)** - the D-pad on the HUD for D-pad items, from the Input Prompts pack (CC0,
  `third_party/kenney`)

**Libraries and tools**
- [SDL 3](https://libsdl.org) - windowing, input, audio (zlib license)
- [FFmpeg](https://ffmpeg.org) - movie playback (LGPL)
- [Dear ImGui](https://github.com/ocornut/imgui) - overlay menu and start screen (MIT)
- [Baloo 2](https://github.com/EkType/Baloo2) by Ek Type - the start screen's font (SIL Open Font License 1.1, `third_party/fonts/Baloo2/OFL.txt`)
- [BIZ UDPGothic](https://github.com/googlefonts/morisawa-biz-ud-gothic) by Morisawa - the game's text on Linux when you haven't imported the Switch fonts: a free font from the same universal-design gothic family as the Switch's UD Shin Go (SIL Open Font License 1.1, `third_party/fonts/BIZUDPGothic/OFL.txt`)
- [DejaVu Sans](https://dejavu-fonts.github.io) - the in-game menu and button letters when no system font fits, and the game text's last resort (Bitstream Vera / DejaVu license, `third_party/fonts/DejaVu/LICENSE`)
- [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) - Windows and Linux renderer (MIT)
- [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego
  Gutierrez - the anti-aliasing option (MIT, `third_party/smaa/LICENSE.txt`)
- [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) - the Windows and Linux upscaler
  option, ported to GLSL (MIT)
- [oead](https://github.com/zeldamods/oead) and [evfl](https://github.com/zeldamods/evfl) - Nintendo file formats, used by LAS-Randomizer
- [PyInstaller](https://pyinstaller.org) - packs the seed generator into a standalone helper (GPL 2.0+ with the
  bootloader exception, which allows distributing the packed program). The helper bundles CPython (PSF License),
  oead / evfl (GPL 2.0+), [Keystone](https://www.keystone-engine.org) (GPL 2.0; Python bindings BSD), quicktex
  (LGPL 3), Pillow (MIT-CMU), PyYAML (MIT) and appdirs (MIT)
- The iOS app has no helper: it embeds [BeeWare's](https://beeware.org) build of CPython 3.13 for iOS (PSF License;
  with bzip2, libffi, mpdecimal, OpenSSL and XZ for the standard library) and the same generator packages, built for
  iOS, inside the app, and links SDL 3 and FFmpeg statically. Their licence texts ship in the app (`LICENSES/`).
- [Zig](https://ziglang.org) - its C compiler (`zig cc`: Clang and LLD) builds the game code on the player's
  machine during setup (MIT; LLVM, Clang, LLD: Apache 2.0 with LLVM exceptions)
- The game code generator (`tools/gen.py`) is packed by PyInstaller into a standalone helper too. It bundles CPython,
  [Capstone](https://www.capstone-engine.org) (BSD 3-clause; disassembly), [Unicorn](https://www.unicorn-engine.org)
  (GPL 2.0; Python bindings BSD; runs small pieces of code to resolve jump tables) and
  [python-lz4](https://github.com/python-lz4/python-lz4) (BSD 3-clause, with the LZ4 library: BSD 2-clause)
- [shaderc](https://github.com/google/shaderc) - compiles the translated shaders for Vulkan on Windows, Linux and
  Android (Apache 2.0; with glslang and SPIRV-Tools)
- The Android app, ported from [LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid) by mamcjr (MIT), also
  ships [Python](https://www.python.org) 3.14 for Android (PSF License) to run the game code generator on the device,
  [volk](https://github.com/zeux/volk) by Arseny Kapoulkine (MIT) to load Vulkan, and
  [libadrenotools](https://github.com/bylaws/libadrenotools) and
  [liblinkernsbypass](https://github.com/bylaws/liblinkernsbypass), Copyright (c) 2021, Billy Laws (BSD 2-Clause; their
  licence text ships in the app), to load a custom Vulkan driver such as Turnip (not included)
- [envytools](https://github.com/envytools/envytools) - NVIDIA GPU documentation used for the shader translator
- An MIT-licensed open-source project's audio renderer and shader instruction decoders (license and copyright notice:
  `third_party/ryujinx/LICENSE.txt`) - the audio renderer's reverb is a port of its reverb code and the mixer is
  modelled on its renderer; its instruction decoders were the bit-layout reference for parts of the shader translator
- [XC2-Recomp-Contributions](https://github.com/circerio/XC2-Recomp-Contributions) by circerio (MIT) - the
  branch-phi test shader in the shader translator's control-flow test (`tools/test_translate_flow.c`)
- **Dusklight** by the TwilitRealm team (https://github.com/TwilitRealm/dusklight) - design inspiration for the start
  screen (Play / Settings / System layout)
- **SuperMarioWonderRecomp** by BelmanteGu (https://github.com/BelmanteGu/SuperMarioWonderRecomp) - its
  ahead-of-time shader approach is what the first-launch shader preparation follows (a shipped list of shader variants,
  built on the player's machine from their own game files)
- The Switch homebrew community's documentation ([Switchbrew](https://switchbrew.org))

## Legal

LAS Recomp is a fan project. It is not affiliated with, endorsed by or sponsored by Nintendo. *The Legend of Zelda*
and *Link's Awakening* are trademarks of Nintendo. No game files, game code, assets or fonts are included - not in
this repository and not in release builds. You must provide your own legally obtained copy of the game files to play.

## License

LAS Recomp's own code (the recompiler tools, the runtime, the randomizer additions) is open source under the
[MIT License](LICENSE). Third-party components keep their own licenses: LAS-Randomizer (MIT), ported audio renderer code (MIT, see `third_party/`), Dear ImGui (MIT),
Vulkan Memory Allocator (MIT), SMAA (MIT), AMD FSR 1 (MIT), the Baloo 2 and BIZ UDPGothic fonts (SIL OFL 1.1), the DejaVu Sans font (Bitstream Vera / DejaVu license), SDL 3 (zlib), FFmpeg (LGPL 2.1+, linked dynamically, and statically in the iOS app), the
Zig compiler (MIT; its LLVM / Clang / LLD: Apache 2.0 with LLVM exceptions; its C library headers keep their own
licenses), the iOS kit's Mach-O linker (LLD as built by the Rust project: Apache 2.0 with LLVM exceptions), miniz
(MIT: the iOS kit and the .ipa it makes), and the generator helpers' bundled libraries listed under Credits (oead / evfl / Keystone in the seed
generator and Unicorn in the game code generator are under the GPL, versions in the table below: each helper is a separate program the game runs, and its
sources are this repository plus those projects' published sources). The iOS app is the exception: iOS runs no helper
processes, so it embeds CPython and the seed generator's packages - oead, evfl and Keystone (GPL 2.0+ / GPL 2.0),
quicktex (LGPL 3), Pillow, PyYAML, appdirs - and links SDL 3 and FFmpeg (LGPL 2.1+) statically; the app's `LICENSES/`
folder carries each one's licence text, its sources are this repository plus the versions in the table (the LGPL
libraries can be relinked: `make deps-ios` builds them from source, `make ipa` the app), and the game code library it
embeds is built from the player's own game files and never shared. The license covers LAS Recomp's code only - it grants
nothing for Nintendo's game, which you must supply yourself.

### Source code of bundled third-party components

Release builds include these components; their source code for the exact versions shipped is available here. The
seed generator's packages are in the desktops' helper and inside the iOS app alike:

| Component | Version | License | Source |
|---|---|---|---|
| Keystone (seed generator; on iOS built with `tools/patches/keystone-0.9.2.patch`) | 0.9.2 | GPL 2.0 (Python bindings: BSD) | https://github.com/keystone-engine/keystone/releases/tag/0.9.2 |
| oead (seed generator) | 1.3.0 | GPL 2.0+ | https://github.com/zeldamods/oead/releases/tag/v1.3.0 |
| evfl (seed generator) | 1.2.0 | GPL 2.0+ | https://github.com/zeldamods/evfl/releases/tag/v1.2.0 |
| quicktex (seed generator) | 0.2.1 | LGPL 3.0 | https://github.com/drewcassidy/quicktex/releases/tag/v0.2.1 |
| Pillow (seed generator; on iOS BeeWare's wheel) | 10.4.0 | MIT-CMU | https://github.com/python-pillow/Pillow/releases/tag/10.4.0, https://pypi.anaconda.org/beeware/simple/pillow/ |
| PyYAML (seed generator) | 6.0.2 | MIT | https://pypi.org/project/PyYAML/6.0.2/ |
| appdirs (seed generator) | 1.4.4 | MIT | https://pypi.org/project/appdirs/1.4.4/ |
| pybind11 (oead's and quicktex's Python bindings, built into them on iOS) | 3.1.0 | BSD 3-clause | https://github.com/pybind/pybind11/releases/tag/v3.1.0 |
| PyInstaller (packs the seed and game code generators) | 6.22.3 | GPL 2.0+ with bootloader exception | https://github.com/pyinstaller/pyinstaller/releases/tag/v6.22.3 |
| CPython (seed and game code generators) | 3.12.14 (macOS), 3.12.10 (Windows), 3.12.15 (Linux), 3.14.8 (Android) | PSF | https://github.com/python/cpython/releases/tag/v3.12.14, https://github.com/python/cpython/releases/tag/v3.12.10, https://github.com/python/cpython/releases/tag/v3.12.15, https://github.com/python/cpython/releases/tag/v3.14.8 |
| CPython for iOS, BeeWare's Python-Apple-support build (the iOS app's seed generator; with bzip2 1.0.8, libffi 3.4.7, mpdecimal 4.0.0, OpenSSL 3.5.9, XZ 5.6.4) | 3.13.16 (b16) | PSF; bzip2, libffi (MIT), mpdecimal (BSD 2-clause), OpenSSL (Apache 2.0), XZ (0BSD / public domain) | https://github.com/beeware/Python-Apple-support/releases/tag/3.13-b16, https://github.com/python/cpython/releases/tag/v3.13.16 |
| Zig (builds the game code during setup; includes LLVM, Clang, LLD) | 0.17.0 | MIT; LLVM: Apache 2.0 with LLVM exceptions | https://ziglang.org/download/0.17.0/zig-0.17.0.tar.xz |
| LLD, as rust-lld (iOS kit: links the iOS game code on each desktop) | Rust 1.91.0 (LLVM 21.1.2) | Apache 2.0 with LLVM exceptions | https://static.rust-lang.org/dist/rustc-1.91.0-src.tar.xz, https://github.com/rust-lang/llvm-project |
| miniz (reads the iOS kit, writes the .ipa) | 3.0.2 | MIT | https://github.com/richgel999/miniz/releases/tag/3.0.2 |
| Capstone (game code generator) | 5.0.9 | BSD 3-clause | https://pypi.org/project/capstone/5.0.9/ |
| Unicorn (game code generator) | 2.1.4 | GPL 2.0 (Python bindings: BSD) | https://pypi.org/project/unicorn/2.1.4/, https://github.com/unicorn-engine/unicorn |
| python-lz4 (game code generator; includes LZ4) | 4.4.5 | BSD 3-clause; LZ4: BSD 2-clause | https://pypi.org/project/lz4/4.4.5/ |
| FFmpeg (LGPL configuration; dynamic on macOS, Windows and Linux, in the Android builds, static in the iOS app: `make deps-ios` relinks it from this source) | 9.0.2 | LGPL 2.1+ | https://ffmpeg.org/releases/ |
| SDL 3 | 3.4.18 (macOS, Windows; iOS, static, with `tools/patches/SDL3-3.4.18-ios27-orientation.patch`), 3.4.16 (Linux, Android) | zlib | https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18, https://github.com/libsdl-org/SDL/releases/tag/release-3.4.16 |
| shaderc (Windows, Linux, Android: GLSL to SPIR-V; Linux, Android: with glslang, SPIRV-Tools inside) | 2026.4 (Windows), 2026.3 (Linux, Android) | Apache 2.0 | https://github.com/google/shaderc |
| libiconv (Windows: used by SDL 3) | 1.19 | LGPL 2.1+ | https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.19.tar.gz |
| GCC runtime: libgcc, libstdc++ (Windows) | 16.2.0 | GPL 3.0 with the GCC Runtime Library Exception | https://gcc.gnu.org/ |
| winpthreads (Windows, mingw-w64) | 14.0.0 | MIT / BSD | https://www.mingw-w64.org/ |
| Baloo 2 font (SemiBold) | 1.640 | SIL OFL 1.1 | https://github.com/EkType/Baloo2 |
| BIZ UDPGothic font (Regular, Linux) | 1.05 (Google Fonts) | SIL OFL 1.1 | https://github.com/googlefonts/morisawa-biz-ud-gothic, https://github.com/google/fonts/tree/main/ofl/bizudpgothic |
| DejaVu Sans font (Book, Bold) | 2.37 | Bitstream Vera license (DejaVu changes public domain) | https://github.com/dejavu-fonts/dejavu-fonts/releases/tag/version_2_37 |
| SMAA (shader and lookup textures, built in) | 2013 (repository master) | MIT | https://github.com/iryoku/smaa |

The exact build scripts that fetch and build these (with checksums) are in this repository: `tools/build_deps_macos.sh`,
`tools/build_deps_windows.sh`, `tools/linux/build_deps.sh`, `tools/build_deps_ios.sh` and `tools/rando/rando_gen.spec`. The Windows build takes SDL 3, shaderc, libiconv and the
compiler runtime from MSYS2's UCRT64 packages (https://packages.msys2.org, which links each package's source).
