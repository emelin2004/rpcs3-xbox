# RPCS3 for Xbox (UWP, Dev Mode)

Unofficial port of the RPCS3 PlayStation 3 emulator to a UWP app for Xbox Series X in Developer
Mode. Not supported by the RPCS3 team; don't ask them for help with it. This repository is private.

The port is one app package (`RPCS3Xbox`): a controller menu (games, installs, settings) plus the
full emulator. It is built on Linux with a zig/clang cross toolchain, needs no Qt and no Windows
PC, and is installed through the Xbox Device Portal.

## Current state (2026-10-16)

| What | Status |
| --- | --- |
| PS3 XMB (firmware 4.93) | Boots from the Games tab ("PS3 System Menu (XMB)") in ~20 s warm, steady 60 fps with PPU LLVM |
| Network and XMB web browser | Works; network is "Connected" by default, browser opens (lazily committed 256 MB `sys_vm` mapping) |
| Controller, audio | Windows.Gaming.Input pads 1-4, XAudio2 |
| Jetpack Joyride Deluxe | Runs, locked 60 fps |
| Minecraft PlayStation 3 Edition | Runs, ~55 fps in menus, ~30 fps in a new world (network off for it) |
| Watch Dogs (BLES01854) | Title screen at 30 fps; in-game loading runs out of the 5 GB limit at ~85%. Memory work in PR #9 |
| Plants vs. Zombies Garden Warfare (BLES02021) | Main menu at 5-7 fps with the PS3 network off (EA servers are gone). Auto network-off in PR #10 |
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
- PSN/RPCN is out of scope. The Xbox GPU only exposes D3D12 feature level 11_0 / SM 6.4 to apps.

## How it works

- `uwp/app`: CoreWindow shell. `uwp_main.cpp` sets Xbox defaults and test switches,
  `uwp_frontend.cpp` is the menu (drawn on the CPU), `uwp_input.cpp` the controllers,
  `uwp_firmware.cpp` firmware/package installs, `uwp_sysstubs.c` and `uwp_lazyimports.c` stand in for
  Win32 DLLs the Xbox doesn't have.
- Graphics: RPCS3's Vulkan renderer on Mesa **Dozen** (Vulkan over D3D12, `vulkan_dzn.dll`) with
  Microsoft's `dxil.dll`. There is no Vulkan loader: `uwp/vkshim` forwards `vk*` calls to Dozen.
  Frames are read back and shown through the shell's D3D11 swapchain (~1 ms).
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
  -DUSE_SYSTEM_OPENCV=OFF -DUSE_DISCORD_RPC=OFF -DUSE_GAMEMODE=OFF -DUSE_LIBEVDEV=OFF
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
2. Home > Add, pick the `.appx` (first time: also the signing `.cer`), install.
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
- **Settings**: resolution scale, frame limit, VSync, shader mode, network, stats overlay and more,
  saved to `config.yml` right away.

### Controls

| Xbox | PS3 |
| --- | --- |
| A / B / X / Y | Cross / Circle / Square / Triangle |
| Menu / View | Start / Select |
| Tap View + Menu | PS button |
| Hold View + Menu ~1 s | Stop the game, back to the app menu |

Menu: D-pad or left stick to move, A select, B back, LB/RB tabs, Y rescan. The Xbox button stays
with the system.

### LocalState switches (for testing)

Empty files in the app's `LocalState` folder (via Device Portal file explorer):

- `autoboot`: skip the menu; contents `xmb`, a title ID, or a full path (ISO). Delete it afterwards.
- `autopress`: scripted button presses for remote test runs.
- `storage`: `local` or a folder path for the cache location.
- `precompile`, `background_compile`: turn LLVM precompilation / background PRX compile back on.
- `no_network`, `null_audio`, `null_renderer`, `ext_host_mem`: debugging switches.
- `heap_guard`: full operator new guard (header and canary on every block, checked on delete,
  report in `heap_guard.log`). Off by default: it costs ~80 bytes per block, ~230 MB in Portal 2.
- `vram_limit`: Vulkan video memory cap in MB (default 1536; it comes out of the app's 5 GB).
- `compile_headroom`, `compile_job_mb`: a new PPU/SPU compile job starts only while
  headroom + job_mb per running job is left of the app's memory (defaults 1280 and 128 MB).
- `default_timer`: don't raise the system timer resolution (the app raises it to the finest the
  device allows, as desktop RPCS3 does; the log shows both).
- `rsx_idle_sleep`: microseconds the RSX thread sleeps per check once the game has given it
  nothing to do for 1 ms, instead of spinning (default 200; `0` spins as RPCS3 does).
- `dynamic_stencil_bias`: keep stencil masks and depth bias as Vulkan dynamic state. By default
  they are baked into the pipelines, because Dozen otherwise builds a D3D12 PSO for each new value
  at draw time on the RSX thread (hitches of 0.1-4 s in Portal 2).
- `sync_shader_compile`: compile new shaders (GLSL to SPIR-V) on the RSX thread as RPCS3 does.
  By default the pipeline compiler threads do it, since a room load in Portal 2 brings dozens of
  new shaders at ~12 ms each.

Per-game settings: `LocalState/rpcs3/config/custom_configs/config_<serial>.yml` (any subset of
config.yml) overrides everything, including the app's built-in game settings (Portal 2: SPU blocks Safe;
Minecraft PS3 Edition: PS3 network Disconnected, it hangs on "Finalizing" when Connected).

Logs: `LocalState/rpcs3/RPCS3.log` and `LocalState/mesa.log`; the previous run's are kept as
`RPCS3.prev.log` and `mesa.prev.log`, and the last run that booted a game also as
`RPCS3.game.log` (written when the game stops, or at the next launch). `LocalState/memory.log` (and `memory.prev.log`) keeps the
app memory trend even when RPCS3.log has hit its size cap. A fatal error shows its log line on the red screen.

## Branches

- `main`: console-tested work only.
- `xbox-builds`: separate history with built `.appx` packages and Xbox test logs/screenshots.
- `claude/*`: work in progress, reaches main through PRs.
