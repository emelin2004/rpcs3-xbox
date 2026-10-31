# RPCS3 for Xbox (UWP, Dev Mode)

Unofficial proof-of-concept port of the RPCS3 PlayStation 3 emulator to a UWP app for Xbox
Series X|S in Developer Mode. Not supported by the RPCS3 team; don't ask them for help with it.
For installing a release, see the [main README](../README.md); this page is the technical side.

The port is one app package (`RPCS3Xbox`): a XAML controller menu (games, installs, settings) plus
the full emulator. It is built on Linux with a zig/clang cross toolchain, needs no Qt and no Windows
PC, and is installed through the Xbox Device Portal.

## Current state (2026-10-24)

| What | Status |
| --- | --- |
| PS3 XMB (firmware 4.93) | Boots from the Games tab ("PS3 System Menu (XMB)") in ~20 s warm, steady 60 fps with PPU LLVM |
| Network and XMB web browser | Works; network is "Connected" by default, browser opens (lazily committed 256 MB `sys_vm` mapping) |
| Controller, audio | Windows.Gaming.Input pads 1-4, XAudio2 |
| Portal 2 (BLES01222) | Playable, ~28 fps in big rooms, no compile stalls; bound by the game's PPU main thread |
| Minecraft PlayStation 3 Edition | Playable, ~57 fps (PS3 network off for it automatically) |
| Jetpack Joyride Deluxe | Runs, locked 60 fps |
| PlayStation Home | Launches and works, ~20 fps; sometimes crashes in the main plaza when it runs out of memory |
| GTA V | Boots, ~10 fps, SPU-bound; not playable |
| Watch Dogs (BLES01854) | Title screen; in-game loading runs out of the 5 GB limit. Not playable |
| Plants vs. Zombies Garden Warfare (BLES02021) | Main menu at 5-7 fps with the PS3 network off (EA servers are gone). Not playable |
| Stats overlay | Settings > Stats overlay (FPS, CPU, GPU, top right) |

Cold first start of the XMB compiles its PPU code with LLVM (~8.5 min); later starts reuse the cache.

### Known issues
- ~3 s and ~2.8 s pauses right after the XMB appears (guest polling, not compile time). PR #7 has
  diagnostics; deprioritised behind Watch Dogs performance.
- Game mode gives the app 5 GB of RAM (App mode only 1 GB). Big games hit that limit; see PR #9.
- The XMB "Corrupted Data" entry is normal (same on RPCS3 for PC).
- The app's own storage (`LocalState`, drive Q:) has only a few MB free, so compiled code and caches
  go to the drive with the most free space (normally a USB drive, `E:/RPCS3/cache/`). A new or wiped
  USB drive means the caches are recompiled on the next start.
- If the app data is reset, re-check that the renderer is Vulkan (since b92d610 it is forced).
- RPCN (online play) is set up from Settings > RPCN account; not yet tested on the console.
- The Xbox GPU only exposes D3D12 feature level 11_0 / SM 6.4 to apps.

## How it works

- `uwp/app`: XAML shell (UWP XAML, `Windows.UI.Xaml`; WinUI 3 doesn't run in UWP/Xbox apps). `uwp_main.cpp`
  sets Xbox defaults and test switches, `uwp_xaml.cpp` is the UI (menu, progress and error screens, pause
  menu), `uwp_frontend.cpp` is the menu logic behind it (game scan, installs, settings, boot),
  `uwp_input.cpp` the controllers,
  `uwp_firmware.cpp` firmware/package installs, `uwp_sysstubs.c` and `uwp_lazyimports.c` stand in for
  Win32 DLLs the Xbox doesn't have.
- UI: the pages are real XAML in `uwp/app/xaml/*.xaml`, embedded into the exe at build time and parsed
  at run time with `XamlReader` (there is no XAML compiler on Linux). `uwp_xaml.cpp` finds the named
  elements, fills the lists from the menu model and handles events; focus and the D-pad, A and LB/RB
  tab switching are XAML's own. The WinRT ABI (vtables, IIDs) comes from the Windows SDK metadata:
  `uwp/toolchain/gen_winrt_abi.py` generates `uwp/app/winrt_xaml_abi.h` (checked in; the script's header
  says how to regenerate it). The game picture is a `SwapChainPanel`, behind the menu.
  Settings > RPCN account opens the account screen of `uwp_frontend.cpp` as a XAML page; its fields
  are XAML text boxes, so A on a field opens the Xbox keyboard.
  `LocalState/legacy_ui` (an empty file) starts the earlier software-drawn menu (`uwp_frontend.cpp`
  still has it) instead, as a way back while the XAML shell is new.
  The controls have the Windows 11 look of WinUI 2: the app's Application object hands XAML the
  WinUI 2 types (`IXamlMetadataProvider`) and uses `XamlControlsResources` as its resources, from the
  `Microsoft.UI.Xaml.2.8` framework package the manifest depends on. The pages use the Windows 11
  dark palette (#202020 base, #2B2B2B cards with rounded corners, content-dialog style popups).
- Graphics: RPCS3's Vulkan renderer on Mesa **Dozen** (Vulkan over D3D12, `vulkan_dzn.dll`) with
  Microsoft's `dxil.dll`. There is no Vulkan loader: `uwp/vkshim` forwards `vk*` calls to Dozen.
  Frames are read back and presented (~1 ms) by the shell's render thread into a D3D11 composition
  swapchain that the XAML `SwapChainPanel` shows.
- CPU: PPU LLVM recompiler, SPU LLVM recompiler (LocalState/spu_asmjit for ASMJIT), 6 LLVM threads. Precompilation and background PRX
  compilation are off by default (they starved the XMB).
- Memory: UWP can't map executable or sparse-file views, so shared memory uses pagefile sections;
  `sys_vm` memory is committed lazily in 64 KB granules on first touch.

## Building (Linux host)

Tools: `pip install ziglang cmake ninja meson`, plus nasm, llvm-dlltool, llvm-windres (symlink it from
`llvm-windres-18` if needed), llvm-nm, makemsix (github.com/microsoft/msix-packaging) and
osslsigncode. The toolchain targets `x86_64-windows-gnu` with `-mcpu=x86_64_v3`.

```sh
# 1. FFmpeg
git clone --depth 1 --branch n8.1 https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
uwp/toolchain/build-ffmpeg.sh ffmpeg-src ffmpeg-build ffmpeg-install

# 2. LLVM for the PPU/SPU recompilers (X86 only, static; about 2 h)
uwp/toolchain/build-llvm.sh llvm-build llvm-install

# 3. RPCS3 (Vulkan-Headers checkout for the headers)
cmake -S . -B build-uwp -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=uwp/toolchain/toolchain-uwp.cmake -DCMAKE_BUILD_TYPE=Release \
  -DRPCS3_UWP=ON -DRPCS3_UWP_FFMPEG_LIB_DIR=$PWD/ffmpeg-install/lib \
  -DUSE_VULKAN=ON -DRPCS3_UWP_VULKAN_INCLUDE_DIR=$PWD/Vulkan-Headers/include \
  -DWITH_LLVM=ON -DBUILD_LLVM=OFF -DSTATIC_LINK_LLVM=ON -DLLVM_DIR=$PWD/llvm-install/lib/cmake/llvm \
  -DUSE_NATIVE_INSTRUCTIONS=OFF -DUSE_PRECOMPILED_HEADERS=OFF \
  -DUSE_SDL=OFF -DUSE_FAUDIO=OFF -DUSE_SYSTEM_ZLIB=OFF -DUSE_SYSTEM_CURL=OFF -DUSE_SYSTEM_SDL=OFF \
  -DUSE_DISCORD_RPC=OFF -DUSE_GAMEMODE=OFF -DUSE_LIBEVDEV=OFF
ninja -C build-uwp rpcs3_uwp rpcs3_uwp_importcheck

# 4. Mesa Dozen (after step 3, which creates the import libraries it links against)
uwp/toolchain/build-mesa-dzn.sh mesa-26.2.5 build-dzn
```

`build-mesa-dzn.sh` applies `mesa-dzn-uwp.patch` (unsized UBO arrays, logging) and
`mesa-dzn-depth-typeless.patch` (sampled depth as typeless resources; without it Watch Dogs and
Garden Warfare crash). Release builds use LTO (`-DUSE_LTO=ON`) for the XMB's 60 fps.

## Packaging

```sh
export UWP_EXTRA_FILES=build-dzn/src/microsoft/vulkan/vulkan_dzn.dll:/path/dxil.dll:/path/dxcompiler.dll:/path/DXIL-LICENSE.txt
MAKEMSIX=/path/to/makemsix uwp/package/package.sh build-uwp RPCS3Xbox.appx cert.pfx <password>
```

`dxil.dll`/`dxcompiler.dll` come from the NuGet package Microsoft.Direct3D.DXC. Bump `Version=` in
`uwp/package/AppxManifest.xml` for every build you install (builds are numbered 0.1.34.x now).

## Installing on the Xbox

1. Put the console in Developer Mode and open the Device Portal (`https://<xbox-ip>:11443/`).
2. Home > Add, pick the `.appx` (first time: as a dependency also
   `Microsoft.UI.Xaml.2.8.appx` (x64) from the Microsoft.UI.Xaml 2.8.7 NuGet package,
   `tools/AppX/x64/Release`), install.
3. In Dev Home, select RPCS3Xbox > View details and set **App type: Game** (5 GB RAM instead of 1 GB).
   A reinstall can reset this.
4. Start the app. Install firmware from the Install tab (`PS3UPDAT.PUP` on a USB drive), then boot
   the XMB or a game from the Games tab.

## Using the menu

- **Games**: the XMB, game folders and `.iso` files in `LocalState/games/`, games installed into the
  emulated HDD, and folders/ISOs in `GAMES/`, `PS3ISO/` or the root of a USB drive. Encrypted
  (redump) ISOs need their `.dkey` next to them. Opens on the last played game.
- **Install**: firmware (`PS3UPDAT.PUP`), packages (`.pkg`) and licenses (`.rap`) from `LocalState/`,
  `LocalState/firmware/`, `LocalState/packages/` or a USB drive (root, `PS3/UPDATE/`, `PKG/`,
  `packages/`, `exdata/`). Installing a package also installs the `.rap` files next to it.
- **Settings**: resolution scale, frame limit, VSync, shader mode, network, PSN mode, stats overlay
  and more, saved to `config.yml` right away.
- **Settings > RPCN account**: RPCN, RPCS3's PSN replacement for online play. Create an account
  (username, password, email for the activation token) or log in with one made in desktop RPCS3,
  enter the token, check the login, reset the password, or pick another server. Text fields open
  the Xbox system keyboard (Enter or Menu confirms, A reopens it, B cancels; a USB keyboard works
  too). If the system keyboard can't be shown, the menu draws its own (A type, B delete, X shift,
  Y symbols, Menu done, View shows a password).
  A successful login sets PSN to RPCN. The login is saved in `LocalState/rpcs3/rpcn.yml` (the
  same file as desktop RPCS3's, so one can be copied over), with the password stored hashed.

- **Settings > Send logs to the developer** (telemetry, asked once on the first start, Yes focused):
  see "Telemetry" below.

### Telemetry

`uwp/app/uwp_telemetry.cpp` sends events and gzip-compressed logs to `https://rpcs3.emelinhabibovic.de`
(API: [TELEMETRY_API.md](TELEMETRY_API.md); the server is a separate repository) on one low-priority
thread with libcurl, only while the user's answer is Yes:

| When | Event | Logs |
| --- | --- | --- |
| The user says Yes (first time per install) | `install`, `app_start` | RPCS3.log so far (`app_start`) |
| Every app start | `app_start` (console model from `AnalyticsInfo.DeviceForm`, OS, firmware, build) | |
| A game, homebrew or the XMB starts | `game_launch` (title, kind, renderer, decoders, scale, frame limit, shader mode, build) | |
| Every 10 s while it runs (sent once a minute) | `perf` (fps, longest frame, RSX/GPU/present time, draws, new shaders, busiest PPU threads, SPU load, app memory) and up to 5 `stutter` events (frames of 100 ms or more and 2.5x the average, with the frame's own breakdown and the 1 ms RSX sampler's phases) | |
| Every minute while it runs | | the new lines of RPCS3.log (`rpcs3_live`, `periodic`) |
| It stops | `game_exit` (duration, avg/min fps, stutters) | the rest of RPCS3.log (`rpcs3_live`, `game_exit`); the whole RPCS3.log if the server refuses live logs |
| A fatal error | `crash` (scrubbed message) | the rest of RPCS3.log (`rpcs3_live`, `crash`), or the whole log as above |
| Start after a run that ended during a game | `crash` (`ended_during_game`) | RPCS3.prev.log, mesa.prev.log, memory.prev.log |

LocalState files: `telemetry_id` (the anonymous UUID v4, shown above the firmware line),
`telemetry` (`on`/`off`; missing = ask), `telemetry_installed` (the install event was sent) and
`telemetry_session` (exists while a game runs). Every log line goes through
`uwp_telemetry_scrub.cpp` first: the RPCN name/password hash/token from `rpcn.yml` and XMB user names
are replaced wherever they appear, and user folders in paths, emails, IPv4/IPv6/MAC addresses,
`password=`/`token:`-style values and other players' names in RPCN/NP lines are replaced by
`<labels>`. Host test: `c++ -std=c++20 -I uwp/app uwp/app/uwp_telemetry_scrub.cpp uwp/app/test/telemetry_scrub_test.cpp`.
Patch server JSON reader: `c++ -std=c++20 -I uwp/app uwp/app/uwp_patches_json.cpp uwp/app/test/patches_json_test.cpp`.
Logs over 112 MB are sent as their first 16 and last 96 MB. Failed uploads are retried with backoff
while the app runs (429 and 5xx; other 4xx are dropped); nothing is queued on disk. The TLS roots
for curl are compiled in (`uwp_telemetry_roots.inc`), since the Xbox gives apps no system store.

### Version and update notice

The menu header shows the app's version left of the firmware line. On every app start
`uwp/app/uwp_update.cpp` reads `https://api.github.com/repos/emelin2004/rpcs3-xbox/releases?per_page=10`
(no login; pre-releases count, unlike `/releases/latest`) and compares the highest `vX.Y.Z.W` tag with
the package version: green = current, orange with an "Update ... available" line = a newer release
exists, grey = no answer (offline). It runs whether or not telemetry is on and sends nothing else.
Publishing a GitHub release tagged `v0.1.34.xx` is all an update notice needs.

### Controls

| Xbox | PS3 |
| --- | --- |
| A / B / X / Y | Cross / Circle / Square / Triangle |
| Menu / View | Start / Select |
| View + Menu (PS button) | Pause menu: resume, restart, trophies, RPCN friends, screenshot (PNG in `<drive>/RPCS3/screenshots/`), save / load state (in `<drive>/RPCS3/savestates/`), stop the game (back to the app menu), and the settings of RPCS3's home menu (video, audio, stats overlay, notifications, input, advanced, debug) that apply to the running game. In a game it replaces RPCS3's own home menu; on the XMB a tap is the PS button and holding ~1 s opens the pause menu |

Menu: D-pad or left stick to move, A select, B back, LB/RB tabs, Y rescan, Menu on a game in the
Games tab opens that game's own settings. The Xbox button stays with the system. In the pause menu
the game is paused; B resumes it.

### LocalState switches (for testing)

Empty files in the app's `LocalState` folder (via Device Portal file explorer):

- `autoboot`: skip the menu; contents `xmb`, a title ID, or a full path (ISO). Delete it afterwards.
- `autopress`: scripted button presses for remote test runs.
- `storage`: `local` or a folder path for the cache location.
- `system_styles`: keep the standard (Windows 10) control styles instead of WinUI 2's.
- `legacy_ui`: use the earlier software-drawn menu instead of the XAML one (no pause menu; hold View + Menu stops the game).
- `precompile`: turn LLVM precompilation back on.
- `no_background_compile`: don't compile the firmware's PRX modules in the background once the XMB
  has run for 30 s (on by default; `background_compile_done` remembers a finished run per app
  version, firmware and cache folder; delete it to run again).
- `no_network`, `null_audio`, `null_renderer`, `heap_guard`, `ext_host_mem`: debugging switches.
- `compile_headroom`, `compile_job_mb`: a new PPU/SPU compile job starts only while
  headroom + job_mb per running job is left of the app's memory (defaults 1280 and 128 MB).
- `default_timer`: don't raise the system timer resolution (the app raises it to the finest the
  device allows, as desktop RPCS3 does; the log shows both).
- `rsx_idle_sleep`: microseconds the RSX thread sleeps per check once the game has given it
  nothing to do, or kept it waiting on a semaphore, for 1 ms, instead of spinning (default 200;
  `0` spins as RPCS3 does).
- `spu_workers`: number of SPU LLVM background compile threads (default 4; RPCS3 picks 2 for the
  Xbox's 8 threads; `0` uses RPCS3's count). More threads shorten the slow stretch while a game's
  new SPU code compiles (GTA V new areas, WipEout HD's first race).
- `dynamic_stencil_bias`: keep stencil masks and depth bias as Vulkan dynamic state. By default
  they are baked into the pipelines, because Dozen otherwise builds a D3D12 PSO for each new value
  at draw time on the RSX thread (hitches of 0.1-4 s in Portal 2).
- `sync_shader_compile`: compile new shaders (GLSL to SPIR-V) on the RSX thread as RPCS3 does.
  By default the pipeline compiler threads do it, since a room load in Portal 2 brings dozens of
  new shaders at ~12 ms each.

Per-game settings: Menu on a game in the Games tab opens the pause menu's settings plus SPU block
size, SPU loop detection, write color buffers and network, for that game only. They are saved to
`LocalState/rpcs3/config/custom_configs/config_<serial>.yml` with only the settings that differ from
the app's (the file can also be written by hand, as any subset of config.yml). It overrides everything,
including the app's built-in game settings (Portal 2: SPU blocks Safe; Minecraft PS3 Edition: PS3
network Disconnected, it hangs on "Finalizing" when Connected), which the first change copies into it.
"Use the app's settings" in that menu removes the file. Its Patches section holds RPCS3's community
patches (as the desktop Patch Manager): "Download patches" fetches `patch.yml` from rpcs3.net into
`LocalState/rpcs3/config/patches/` (checked against its SHA-256 and by the patch engine first; later
the menu checks for a newer list once per app start), and each patch listed for the game's serial and
version (a disc game's installed update counts) has a switch. Switches are saved to
`patch_config.yml` and apply from the game's next start; turning one on turns off the others of its
patch group. Patches for all games (serial "All") and adjustable patch values are not shown here (their
defaults apply); `imported_patch.yml` and `<serial>_patch.yml` files in that folder are listed too. A setting changed in the pause menu goes to the
game's file when the game has that setting of its own, otherwise to config.yml.

Logs: `LocalState/rpcs3/RPCS3.log` and `LocalState/mesa.log`; the previous run's are kept as
`RPCS3.prev.log` and `mesa.prev.log`, and the last run that booted a game also as
`RPCS3.game.log` (written when the game stops, or at the next launch). `LocalState/memory.log` (and `memory.prev.log`) keeps the
app memory trend even when RPCS3.log has hit its size cap. A fatal error shows its log line on the red screen.

## Branches

- `main`: console-tested work only. Packages are published on the Releases page.
- `claude/*`: work in progress, reaches main through PRs.
