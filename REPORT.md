# RPCS3 Xbox telemetry report

Running notes from the panel checks. Installs are shown by the first 8 characters of their anonymous ID.
Nothing here is merged; every PR needs a console test first.

## Pass 1: 2026-10-24, 23:10–23:30 UTC (data from the last 7 days)

Panel totals: 8 installs (5 Series S, 3 Series X), 84 app starts, 64 game launches, 75 crash reports.
Versions in use: .77 ×4, .76 ×2, .73 ×2.

### Crashes

| # | What | Installs / reports | Builds | Cause | Fix | Confidence |
|---|------|--------------------|--------|-------|-----|------------|
| 1 | `PAT: Failed to load patch file .../patches/patch.yml: ... referenced anchor is not defined: 50cent_openspy_notes` | 1 install (`03552d8b`), ~20 reports, at every start | .75–.77 | **Not a crash.** The user's own patch.yml is broken. Upstream logs it as fatal, skips the file and runs on (XMB at 60 fps behind the error screen). The port turns any fatal line into the error screen and a crash report, which also blocks the real crash report for the rest of that run. | [emelin2004/rpcs3-xbox#38](https://github.com/emelin2004/rpcs3-xbox/pull/38): the PAT channel is ignored by the fatal listener. Port-wide. | High |
| 2 | 50 Cent BLUS30256: `RSX ... Out of video memory (VK_ERROR_OUT_OF_DEVICE_MEMORY)` in `vk::image::create_impl`, then `OUT_OF_HOST_MEMORY` on RSX workers | 1 install (`03552d8b`), ~10 reports | .75–.77 | **The GPU device was lost first** (`Present: 0x887a0005` DXGI_ERROR_DEVICE_REMOVED a few ms before the OOM, with the VRAM heap at 566/1024 MB). The OOM is Dozen reporting a dead device. The XAML render thread then spun on Present and wrote 170k–190k log lines in about 1 s. The reason for the device loss is still unknown. | [emelin2004/rpcs3-xbox#39](https://github.com/emelin2004/rpcs3-xbox/pull/39): logs `GetDeviceRemovedReason` once and stops the spin and log flood. Diagnostic, port-wide. The device loss itself is still open (game-specific so far). | High (spam); cause still open |
| 3 | Portal 2 BLES01222: long run ends with no error, memory at the 5 GB cap | 1 install (`ed2472f5`), 1 run (1 h 44 min) | .76 | **Shader mode "Shader Interpreter only"**, saved from Settings (the menu hint recommended the interpreter). The interpreter preload built about 6,700 PSOs at about 2 s each for **72 minutes** before the game started, and kept 1.1 GB of D3D12 PSOs. The OS ended the app at 5.3 GB committed, 30 minutes into play. This also explains the panel's "Memory pressure 69%" for Portal 2. | [emelin2004/rpcs3-xbox#40](https://github.com/emelin2004/rpcs3-xbox/pull/40): no interpreter modes in Settings, and an interpreter mode in config.yml or a custom config is replaced at boot (`config_override.yml` can still pick it). Port-wide. | High |
| 4 | Silent ends: no fatal line, the log stops mid-frame, memory about 2 GB | `c83fe870`: 6 runs (Snake Ball NPJA00022 ×5, Tank Battles NPUB30154 ×1). `3735122c`: XMB ×1 | .77 | Unknown. .77 already reports Xbox Home closes and updates as normal exits, so these look real. The port's heap guard `__fastfail`s on heap corruption or a failed `new` and leaves no line in RPCS3.log. Its own log was overwritten at the next start and never uploaded, and the OOM fail-fast wrote nothing. | [emelin2004/rpcs3-xbox#41](https://github.com/emelin2004/rpcs3-xbox/pull/41): keep `heap_guard.prev.log`, log the OOM fail-fast, upload it with `ended_during_game`. Diagnostic, port-wide. | Medium (cause unproven) |
| 5 | `Stop Watchdog: Stopping emulator took too long` on a normal game exit | 2 installs, 3 reports (GTA V, F1, Portal 2) | .73–.75 | Desktop RPCS3 offers "Keep waiting" here; the port shows the error screen at the 10 s mark. RSX or the port's `VK Pipeline Cache Saver` were still busy. Cache saves to the USB drive took 7 s and 34 s in F1. | [emelin2004/rpcs3-xbox#42](https://github.com/emelin2004/rpcs3-xbox/pull/42): wait up to 60 s for the stop before calling it a deadlock. Port-wide. | Medium-high |
| 6 | Portal 2: dies 3–6 s into a stop from the pause menu, `rsx::thread` waiting on `VK Pipeline Cache Saver`, no error line | `ed2472f5`, 2 runs | .77 | Same stop path as #5. The process ends before the watchdog fires, so it is likely a fail-fast or crash during shutdown. #41 should show which. | (see #41 / #42) | Low |
| 7 | `overlay_audio.cpp:18 init_audio` verification failed (boot music) | `c83fe870`, 6 reports | .76 | Already fixed by #37 in .77; none since on .77. | — | High |
| 8 | SAW 2 BLUS30548: `VK_ERROR_OUT_OF_HOST_MEMORY` in `descriptors.cpp:314` | `03552d8b`, 1 report | .76 | **Same as #2: the GPU device was lost first** (`Present: 0x887a0005` at 10:24.45). Right before it came a 249 ms frame with RSX CPU 203 ms, 92% in `SET_BEGIN_END` (a heavy draw, looks like a GPU hang). In one 50 Cent case the device loss followed a 37 s gap in the log (app suspended?). | #39 logs the reason | Medium |
| 9 | PVZ Garden Warfare BLES02021: SPU `Unknown STOP code: 0x0` | `ed2472f5`, 1 report | .75 | Game-specific SPU problem, probably upstream. | — | — |
| 10 | F1 BCES00005: `File.cpp:701 fs::windows_file::read_at` "The device is not ready" | `f3c163e1`, 1 report | .73 | The game's disc or USB drive stopped answering (the same drive had 34 s cache saves). Probably the drive, not code. | — | Low |
| 11 | PKGi: `VK_ERROR_INITIALIZATION_FAILED` in `memory.cpp:280` | `ed2472f5`, 1 report | .73 | Old build, one report. | — | — |
| 12 | `reason=true` crash reports | `f3c163e1`, `ed2472f5` | .73/.75 | Builds before .76 sent text values as `true` (fixed in #36). These are mostly the known fake crashes from closing via Xbox Home or updating. | — | High |

### Hiccups (stutters)

| Game | What the logs show | Cause | Fix | Confidence |
|------|-------------------|-------|-----|------------|
| All games (panel) | "Present" stutters: 50 Cent's long frames are 140–188 ms with `present wait` 106–148 ms, `present copy` ~1.5 ms, RSX `GPU wait` 0 | The app's `present_ms` is the wait on the **previous frame's GPU fence** (`swapchain_uwp.hpp`), so this is GPU time, not presenting. The server classed it wrong. | [emelin2004/rpcs3-xbox-telemetry#6](https://github.com/emelin2004/rpcs3-xbox-telemetry/pull/6) (server only) | High |
| All games (panel) | "Shader compile" stutters: SAW 2 shows 51%, but its "1 new pipeline" frames had RSX idle for 81–117 of 91–131 ms | Pipelines build asynchronously. One finishing in a frame was counted as the cause even when RSX was just waiting for the game. | [emelin2004/rpcs3-xbox-telemetry#7](https://github.com/emelin2004/rpcs3-xbox-telemetry/pull/7) (server only) | High |
| Portal 2 | 178 hitches in 4.5 min (.77, Async Recompiler), 174 of them with RSX idle. About one 50 ms frame per second, plus a 110–150 ms frame every 2–6 s. `main_thread` at 96–100%, 28–29 fps against a 30 fps target. No link to the port's memory poll, cache saves or perf reports. | (a) PPU-bound frames just missing the 33 ms deadline wait for the next vblank (50 ms), which looks like judder. (b) The long frames: about 105 ms with no RSX command from the game. Thread oversubscription is a guess (main_thread 100%, 6 SPU threads at 50–86%, RSX 50%, plus compiler threads, on the console's CPU share); not proven. | None yet. Ideas to test on the console: higher priority for the PPU main thread; fewer SPU threads (Max SPURS Threads 3). | Medium for (a), low for (b) |
| Minecraft BLUS31426 | 89 stutters in 7.5 min, nearly all "Game sync wait" at 75–125 ms. main_thread 60–80%, SPU about 30%. | First run on this install: `SPU Runtime ... from 0 cached functions`. 350–800 SPU functions built per minute while it played; new SPU code runs in the interpreter until built. Minecraft BLES01976 on an install with a cache: 4.4 stutters/min. | None needed if later runs are smooth. Check the next BLUS31426 session. | Medium |
| Snake Ball, 50 Cent, SAW 2 | Hitches overlap the 10 s memory poll 3–9× more often than chance (Snake Ball 7 of 28), but it's a small share overall | The poll walks the whole address space with VirtualQuery (up to 34 ms). | Not changed. Worth an A/B with `LocalState/no_mem_watch`. | Low |

| XMB (`72b5b9c3`, new install, .76) | Freezes of 4.2–8.9 s while browsing (`Frame gap of 7.809 s`, `8.924 s`, `6.240 s`, `4.205 s`). `ScePafJob` at 91–99%, 0 draws | First-use PPU LLVM compiles of XMB modules (`vmclib.sprx`, `x3_mdimp4/5/8.sprx`, `np_trophy_util.sprx`...; 69 `LLVM: Compiling module` lines in 5 min). This is the expected cost of keeping LLVM precompilation off (it took 36+ minutes on first boot). They're cached afterwards. | None. The "Compiling PPU code" overlay is meant to show after 1.5 s; worth a look on the console to check it really appears over the XMB. | High |

The timer resolution is already raised to 1 ms in every log (`UWP: timer resolution 1.00 ms`), so it is not the cause.

### Notes (not bugs yet)
- XMB sessions show 230–760 fps averages on the Performance page, with VSync off and frame limit Auto. Worth checking whether the XMB really runs unlimited, which would waste GPU time and power, or whether the server's average is off.
- GTA V now averages about 25 fps (SPU-bound, 95%), better than the ~10 fps in the notes.
- 50 Cent: stutters are "Present 45%" (the console's present/copy path).
- Minecraft BLUS31426: 37 fps average with 25 stutters/min, classed "Game sync wait".

### Next
- After a new build: check whether #41 brings in heap guard logs for the silent ends, and whether #39 shows why 50 Cent loses its GPU device.
- Portal 2 long frames: find what the game thread waits on (PPU thread sampling would help).
- Each Portal 2 frame logs `NV3089_IMAGE_IN: Operation writes memory onto itself ... Will ignore` (about 30 lines/s). Harmless, but it fills the live log.

- Installs `db91d4cb` and `86f9116c` only opened the app without firmware (0 launches, no errors). Only the app_start log is sent, so a failed firmware install would not show. Maybe log or report firmware install results.
