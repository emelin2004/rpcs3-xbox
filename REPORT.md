# RPCS3 Xbox telemetry report

Running notes from the panel checks. Installs are shown by the first 8 characters of their anonymous ID.
Nothing here is merged; every PR needs a console test first.

## Morning summary: what to test, in this order

None of the app PRs were built in the cloud session (no UWP toolchain there); each has a console check in its description. All are branched from main (0.1.34.77) and independent, and the first ten were checked to merge together without conflicts (11 files, +180/−15; #48 touches only `uwp_telemetry.cpp`'s `on_fatal`); a test build with several of them needs a version of .78 or higher.

| # | PR | What it should show on the console | Why first |
|---|----|------------------------------------|-----------|
| 1 | [#41](https://github.com/emelin2004/rpcs3-xbox/pull/41) heap-guard log upload | After a silent end, a `crash` log with the heap guard's text in the panel | Explains the largest unexplained group: silent ends and deaths during a stop (Portal 2, PS Home, Snake Ball, first-boot XMB) |
| 2 | [#45](https://github.com/emelin2004/rpcs3-xbox/pull/45) pipeline cache saver at stop | `stopping the pipeline cache saver (...)` at every stop; no 10 s watchdog with an idle saver; saves timed in two parts | 4 Stop Watchdog fatals on .77 tonight all wait on this thread |
| 3 | [#42](https://github.com/emelin2004/rpcs3-xbox/pull/42) slow stop isn't a deadlock | No error screen for stops under 60 s | Pairs with #45 |
| 4 | [#38](https://github.com/emelin2004/rpcs3-xbox/pull/38) broken patch.yml isn't fatal | No error screen / crash report from a user patch.yml error | ~25 false crash reports from one install |
| 5 | [#39](https://github.com/emelin2004/rpcs3-xbox/pull/39) GPU device lost as the fatal line | `UWP: the GPU device was lost (<reason>)` instead of "Out of video memory"; no 170k-line log flood | 6 device losses (50 Cent, SAW 2, PS Home); the reason decides the next fix |
| 6 | [#44](https://github.com/emelin2004/rpcs3-xbox/pull/44) pause on suspend | `UWP: suspending (game paused)` / `resuming`; game continues after a minute in Xbox Home | 2 of the device losses came right after a suspend |
| 7 | [#40](https://github.com/emelin2004/rpcs3-xbox/pull/40) no shader interpreter | Settings shows two shader modes; an old interpreter config is replaced at boot | 72-minute Portal 2 boot and memory-cap death; a player switched to it again tonight |
| 8 | [#46](https://github.com/emelin2004/rpcs3-xbox/pull/46) RSX semaphore wait sleeps | Lower `RSX busy` while waiting; compare fps with `LocalState/rsx_idle_sleep` = 0 | Timing change: needs an A/B (GTA V, Wolverine, Portal 2) |
| 9 | [#43](https://github.com/emelin2004/rpcs3-xbox/pull/43) cellMicIsAttached | No `Inserting headset` / `cellMicOpenEx` loop in 50 Cent | Log size and wasted game time |
| 10 | [#48](https://github.com/emelin2004/rpcs3-xbox/pull/48) Dozen + memory logs with a fatal error | `mesa` and `memory` logs next to every fatal crash in the panel | Dozen's own failure reason (e.g. `CommandList::Close failed`) never reached the panel |
| 11 | [#47](https://github.com/emelin2004/rpcs3-xbox/pull/47) sys_spu_image_close wait flag | `spurs_test.self` runs to its end | Small upstream fix |
| — | [telemetry#6](https://github.com/emelin2004/rpcs3-xbox-telemetry/pull/6), [telemetry#7](https://github.com/emelin2004/rpcs3-xbox-telemetry/pull/7) | Panel classes "present wait" as GPU, and shader stutters only when RSX was busy | Server only; `npm test` passes (17/17) |

**Console A/Bs that need no build** (`LocalState/config_override.yml`, applies to every game, delete afterwards):
- Minecraft microstutter (RSX at the 60 fps limit): `Video:` / `  Multithreaded RSX: true`, then separately `Core:` / `  RSX FIFO Fetch Accuracy: Fast`.
- Wolverine memory cap: a lower `LocalState/vram_limit` (default 1536), or Resolution Scale below 100.
- PS Home device loss at ~45 s after boot: try with RPCN signed out.

**Security:** the panel tunnel (the `…ehcss.de` link) served the admin pages without a password. Put Cloudflare Access or the panel password in front of it.

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
| Minecraft (BLES01976 with a full SPU cache; BLUS31426) | **In-game microstutter:** 93 hitches in 148 s, mostly 25–50 ms frames in a ~58 fps game. They come in bursts that follow gameplay, not a fixed rhythm. In the hitch frames RSX was **busy for 16–23 ms**, against a 16.7 ms budget at 60 fps. Per frame: ~1,716 draws, ~61,500 FIFO commands (1 MB), vertex upload 4.4 ms, draws 1.35 ms, setup 0.7 ms; RSX thread 74–79% busy. main_thread is only 60–77% and SPU ~30%. The BLUS31426 run was also the first run on its install (empty SPU cache, 350–800 SPU functions built per minute), which adds game-side waits on top. | **The RSX thread is at the limit of the 60 fps frame time.** Busier frames (more chunks, more draws) go just over it, and the frame waits for the next vblank. The game's network/SSL init (78 CA certificate opens) caused one hitch at boot only. The port's FIFO counters cost nothing. | No code change yet. **A/B on the console without a build**: put `LocalState/config_override.yml` with `Video:\n  Multithreaded RSX: true` (offloads vertex copies over 3.5 KB to the "RSX Offloader" thread), then separately `Core:\n  RSX FIFO Fetch Accuracy: Fast` (no atomic re-reads on FIFO fetch: ~1,250 refills and ~520 line retries per frame now). Compare `UWP perf: ... RSX busy` and the stutters/min. Remove the file afterwards; it applies to every game. | High (RSX-bound); fix untested |
| Snake Ball, 50 Cent, SAW 2 | Hitches overlap the 10 s memory poll 3–9× more often than chance (Snake Ball 7 of 28), but it's a small share overall | The poll walks the whole address space with VirtualQuery (up to 34 ms). | Not changed. Worth an A/B with `LocalState/no_mem_watch`. | Low |
| XMB (`72b5b9c3`, new install, .76) | Freezes of 4.2–8.9 s while browsing (`Frame gap of 7.809 s`, `8.924 s`, `6.240 s`, `4.205 s`). `ScePafJob` at 91–99%, 0 draws | First-use PPU LLVM compiles of XMB modules (`vmclib.sprx`, `x3_mdimp4/5/8.sprx`, `np_trophy_util.sprx`...; 69 `LLVM: Compiling module` lines in 5 min). This is the expected cost of keeping LLVM precompilation off (it took 36+ minutes on first boot). They're cached afterwards. | None. The "Compiling PPU code" overlay is meant to show after 1.5 s; worth a look on the console to check it really appears over the XMB. | High |

The timer resolution is already raised to 1 ms in every log (`UWP: timer resolution 1.00 ms`), so it is not the cause.

## Pass 2: 2026-10-24, 23:40–00:00 UTC

| What | Installs / reports | Cause | Fix | Confidence |
|------|--------------------|-------|-----|------------|
| 50 Cent BLUS30256 VRAM-OOM again (23:43, .77) | `03552d8b`, 1 | GPU device lost **right after a 55 s gap with no log line at all** (process frozen = Xbox suspend). Also seen in log `5186ff71` (37 s gap). 2 of the 3 50 Cent device losses come straight after a suspend; the third (`498504dc`) didn't. The port doesn't pause or log on suspend. | [emelin2004/rpcs3-xbox#44](https://github.com/emelin2004/rpcs3-xbox/pull/44): pause the game on Suspending, resume on Resuming, log both. Port-wide. If the device is still lost, #39's reason line shows the next step. | Medium (cause), low-medium (that pausing is enough) |
| 50 Cent log flood: `Inserting headset [0..7]` + `cellMicOpenEx` → `DEVICE_NOT_FOUND`, every frame | `03552d8b` | `cellMicIsAttached` was a stub that always returned 1, so with no microphone (Null) the game sees 8 headsets and keeps opening them. 186k of 678k lines in one log; it's why the logs reach 20–88 MB and get cut. | [emelin2004/rpcs3-xbox#43](https://github.com/emelin2004/rpcs3-xbox/pull/43): attached only when a device is registered. Port-wide code, seen in one game. | Medium-high |
| 50 Cent silent end at 23:44, 8 s after boot | `03552d8b`, 1 | The user had just switched **Shader mode to "Shader Interpreter only"** in Settings (13 s before booting), apparently to work around the crash above. The run sat in the interpreter preload (memory +100 MB/s, frame gaps of 0.4–0.7 s), then the log ends. | Covered by #40 (no interpreter modes) | Medium |
| patch.yml false alarm | `03552d8b`, 3 more | Same as pass 1 #1 | #38 | High |
| PS Home NPIA00005: `VK_ERROR_OUT_OF_HOST_MEMORY` (23:50, .77) | `3735122c`, 1 | **Also a GPU device loss.** Dozen's `dzn_descriptor_heap_init` → `CreateDescriptorHeap` failed because the device was already removed (`Present 0x887a0005` in the same millisecond). App memory was 1.79 GB. Came 1 s after a 2 s "no new frame" stall during `sceNp2Init` (RPCN login), with the RSX FIFO empty, so not a GPU hang from rendering. That makes 5 device losses in 3 games (50 Cent ×3, SAW 2, PS Home); 2 after a suspend, 3 mid-run. | [#39](https://github.com/emelin2004/rpcs3-xbox/pull/39) updated: the loss is now **the** fatal line with its reason (hung / reset / driver error), so the next one tells us which. | High that it's a device loss; cause open |
| PS Home silent end after 21 min (23:49) | `3735122c`, 1 | No error and no device loss; memory 3.2 GB. Unknown. | (#41 heap-guard log) | — |
| Portal 2: dies 3–6 s into a stop from the pause menu (third time, 23:51) | `ed2472f5`, 3 runs on .77 | Same pattern every time: RSX waits on `VK Pipeline Cache Saver`. Cache saves normally take 0.1–0.3 s but took **254 s** once in this same run (19 MB, cache on the USB drive `E:`), and 7 s / 34 s in F1. Also, a stop always saved once more, even right after a save. Why the process then dies is still not visible. | [emelin2004/rpcs3-xbox#45](https://github.com/emelin2004/rpcs3-xbox/pull/45): no redundant save at stop; saves timed as "getting" vs "writing". | Medium |

## Pass 3: 2026-10-25, 00:00–00:15 UTC

| What | Installs / reports | Findings | Fix / next step | Confidence |
|------|--------------------|----------|-----------------|------------|
| PS Home NPIA00005: `VK_ERROR_OUT_OF_HOST_MEMORY` again (00:01) | `3735122c`, now 2 (23:50, 00:01) | Identical both times: RSX at FIFO **0x34500**, about 45–50 s after boot, **0.3–0.7 s after `rpcn: You are now logged in RPCN`**. Dozen `CreateDescriptorHeap` fails (`dzn_descriptor_set.c:959`) and Present reports the device lost in the same ms. Just before it, a 2 s "no new frame" stall with RSX idle (get == put) and `main_thread` in `sceNp2Init`. Not a rendering hang: the GPU had nothing to do. | Other PS Home runs logged in fine (one did 4 times in 21 min) and their descriptor heap counts were higher, so neither the login nor a heap limit is the trigger. More likely the scene the game loads right after it. #39 will name the reason. An A/B with RPCN signed out would still show whether that path matters. | Medium (repeatable spot); cause open |
| PS Home silent ends (23:55, 23:59) | `3735122c`, 2 | No error, no device loss, no RPCN login nearby. The log just stops. (The `sys_rsx_context_attribute 0x101` calls near the end are per-frame vsync setup, 3,929 in one run, so not a trigger.) | #41 (heap-guard log) should show whether the heap guard ended them | — |
| Minecraft BLES01976 (.77, 23:56) | `ed2472f5` | 56.7 fps, 4.5 stutters/min, same as before (RSX at the 60 fps limit) | A/B configs in the hiccups table | — |

## Pass 4: 2026-10-25, 00:43–01:00 UTC

| What | Installs / reports | Findings | Fix / next step | Confidence |
|------|--------------------|----------|-----------------|------------|
| **New install `39717ffa`** (.77, Series S): first XMB run at 2.2 fps, then `ended_during_game` after 9 min | 1 | The 2.2 fps is the first-boot PPU compile of the system menu (176 `LLVM: Compiling module`), as expected. Then the XMB itself asked to restart (`sysconf_plugin` → `sys_sm_shutdown(op=0x200)` "reboot request", typical after first-time settings) → `lv2_exitspawn` → stop. **The app died during that stop** (join waiting on `Network Thread`), so the XMB never came back. For a new user, the app just closes after setup. | Same cluster as below | High that it died in the stop |
| **Deaths during an emulator stop** (cluster) | 7 runs, 4 installs: Portal 2 ×3 (.77) + 1 (.73), F1 (.73), GTA V (.75), XMB reboot (.77) | Each log ends 0.4–6 s into `Stopping emulator...`, with `Emulation Join Thread` waiting on whatever is slow: `VK Pipeline Cache Saver` ×4, `rsx::thread` ×2, `Network Thread` ×1. **No fatal line and no crash event in any of them.** An unhandled exception or `std::terminate` would have produced both (RPCS3's handlers → the port's `report_fatal_error`), so a fail-fast that skips them is the likely end. The port's heap guard does exactly that on heap corruption, and a stop frees a lot of memory. | **#41 is the one to test first.** It keeps and uploads `heap_guard.log`, which would name the corrupted block and stack. #42 / #45 make the stop shorter but don't explain the death. | Medium |
| PS Home silent end 25 s after boot (00:06) | `3735122c`, 1 | Mid SPU compile, no error, no stop. Nothing more in the log. | (#41) | — |

## Pass 5: 2026-10-25, 01:25–01:45 UTC

| What | Installs / reports | Findings | Fix / next step | Confidence |
|------|--------------------|----------|-----------------|------------|
| **Stop Watchdog fatals on .77** (PS Home 01:02 and 01:10, 50 Cent 01:08) | `3735122c` ×2, `03552d8b` ×1 | Here the app lived through the full 10 s and sent its log. All three are the same chain: `Emulation Join Thread` → `rsx::thread` → **`VK Pipeline Cache Saver`**, which didn't stop for >10–13 s. In the 50 Cent case its last save (1:19:20) took 67 ms and there was **no new program** after it, yet the join waited more than 12.9 s, longer than its 10 s idle wait could last. So the saver is blocked somewhere the logs don't show. One PS Home save earlier in that run took **102 s**. The saver is the common thread in 7 of the 10 slow or fatal stops so far. | [#45](https://github.com/emelin2004/rpcs3-xbox/pull/45) updated: the saver records its phase (waiting / getting the data / writing the file); at stop RSX logs the phase and how long it has been in it, and warns if the join takes over 1 s. The next slow stop will say where it's stuck. With #42, a slow stop no longer becomes an error screen before 60 s. | High (the saver blocks the stop); cause open |
| 50 Cent VRAM-OOM again (01:15) | `03552d8b` | Device loss mid-run (no suspend gap): right after `Add program` + a compute pipeline, `dzn_image.c:870` (`vkBindImageMemory` → `CreatePlacedResource` failed, reported as OOM) and `Present 0x887a0005` 2 ms later. 6 device losses now. | #39 (reason line) | — |
| 50 Cent mic loop | `03552d8b` | Still there (59,200 `cellMicOpenEx` lines in one 4-minute log) | #43 | — |

- Install `03552d8b` keeps its games inside LocalState and is down to **1,175 MB free of 30 GB** (3 GB at 22:00). If it fills up, saves, caches and logs will start failing on that console. Not a code problem, but worth telling the player.

## Pass 6: 2026-10-25, 02:06–02:30 UTC

| What | Installs / reports | Findings | Fix / next step | Confidence |
|------|--------------------|----------|-----------------|------------|
| X-Men Origins: Wolverine BLUS30268: Stop Watchdog at game exit (01:50) | `03552d8b`, 1 | 4th watchdog on .77 with the same chain: `rsx::thread` → `VK Pipeline Cache Saver`. Its last save took **177,605 ms** for 9 MB while the pipeline count rose 79 → 554. **Every slow save so far (254 s, 177 s, 102 s, 34 s, 7 s) fell in a compile burst.** In the 50 Cent case an *idle* saver still held a stop >12.9 s, longer than its own 10 s wait. | [#45](https://github.com/emelin2004/rpcs3-xbox/pull/45) extended: the saver checks for a stop every 250 ms (by the clock), plus the phase logging. If idle-saver stops get fast, a lost wake-up was the cause; if a save is running, the log now says whether it's getting the data or writing the file. Checked: no Dozen lock spans a PSO compile, the port's `mesa_sysstubs.c` only stubs GDI, and thread priorities are normal. | Medium |
| **RSX spins in `NV406E_SEMAPHORE_ACQUIRE`** (hiccups, all games) | GTA V, Wolverine, PS Home, Portal 2 | Hitch frames with ≥30% in this wait: Wolverine 690/930, GTA V 110/160, PS Home 84/293, Portal 2 63/782. Wolverine's menu: `RSX busy 98% (idle 91%)`, `RSX CPU 125 ms` in a 118 ms frame. The wait is a pure spin, so RSX holds a core the PPU/SPU threads need (GTA V is SPU-bound). The FIFO idle wait already sleeps after 1 ms; this one didn't. | [emelin2004/rpcs3-xbox#46](https://github.com/emelin2004/rpcs3-xbox/pull/46): same 1 ms → 200 µs sleep rule, same `rsx_idle_sleep` switch. Needs a console A/B. | High (CPU), medium (fps gain) |
| Wolverine stutters: 606 hitches in 14 min (22–24 fps) | `03552d8b` | First run (`0 cached functions`): mostly 100–125 ms game-side frames during SPU/PPU compiles, plus the semaphore spin above. | Check the next run with a warm cache | Medium |

## Pass 7: 2026-10-25, 02:49–03:05 UTC

| What | Installs / reports | Findings | Fix / next step | Confidence |
|------|--------------------|----------|-----------------|------------|
| Wolverine BLUS30268: `Verification failed (object: 0x0)` at `VKGSRender.cpp:1096` (`on_vram_exhausted`), then an access violation (02:28) | `03552d8b`, 1 | **Memory pressure at the cap**: after 32 min, app memory 4.73 GB of 5 GB, committed 5.27 GB (private 4.1 GB). The VRAM heap was over its 1 GB budget (blocks 1,122 MB, 1,683 descriptor pools and growing). RSX hit the exhaustion inside an uninterruptible section, where it may not evict, so the `ensure` fired. Game-specific, like PS Home's plaza OOM. | No code change. Worth a test with a lower `LocalState/vram_limit`, or Resolution Scale below 100, to see whether it stays under the cap | Medium |
| New install `e541b716` (.77): XMB ended silently after 8 min (02:36) | 1 | First boot (146 PPU modules compiled), then idle in the XMB at 1.7 GB. The log just stops, with no stop and no error. The user then started Minecraft. Not the reboot-stop death of `39717ffa` (no `sys_sm_shutdown` here). | (#41 heap-guard log) | — |

## Pass 8: 2026-10-25, 03:31–03:45 UTC

| What | Installs / reports | Findings | Fix / next step | Confidence |
|------|--------------------|----------|-----------------|------------|
| `spurs_test.self`: `vm::writer_lock is being used without cpu_flag::wait set by the caller!` in `sys_spu_image_close` (03:24) | `ee91f95f` (new install, no firmware), 1 | The HLE `sys_spu_image_close` frees a **user** SPU image with `vm::dealloc` without setting `cpu_flag::wait`, which the writer lock requires. The syscall version for kernel images sets it. Upstream master has the same code, so any game closing a user SPU image takes this path. | [emelin2004/rpcs3-xbox#47](https://github.com/emelin2004/rpcs3-xbox/pull/47): one line, same pattern as the syscall. Port-wide; worth sending upstream. | High |
| Bundled test programs (`dump_stack.elf`, `gs_gcm_basic_triangle.elf`, `gs_gcm_cube.elf`) "ended during game" | `ee91f95f`, 3 | **No firmware installed**; the user tried the programs from the package's `test/` folder. Each log just stops after 40 s to 2.5 min, with no error. Most likely the player closed the app. | No code change. UX idea: with no firmware, point new users to the firmware install instead of listing test programs | Low |

## Pass 9–10: 2026-10-25, 04:13–05:05 UTC

Quiet (no crashes since 03:24). One new session:

| What | Installs | Findings | Next step | Confidence |
|------|----------|----------|-----------|------------|
| LittleBigPlanet BCUS98148 (first run): 2 min at 5.7 fps, frames of 2–4 s | `39717ffa` | First run: 181 PPU modules and 1,031 SPU functions compiled, `0 cached functions`. Some long frames are RSX **CPU-bound inside a single draw** (`SET_BEGIN_END 98%`, RSX CPU 2,219 ms of a 2,365 ms frame, 0 new pipelines, FIFO not empty). Draw-time PSO variants in Dozen are ruled out: none in any `mesa.log`. Also `CELL_EFAULT` on `sys_fs_read`/`sys_fs_write` around the save data and a failed `TROPUSR.DAT` copy at boot. | Look at the next LittleBigPlanet session with warm caches. If 2 s draws remain, sample inside the draw (vertex/index processing vs. texture cache). | Low |

## Pass 11: 2026-10-25, 05:36–05:50 UTC

| What | Installs | Findings | Next step | Confidence |
|------|----------|----------|-----------|------------|
| **Pattern: a new install's first XMB session ends silently** (05:24) | `abb26e7c` (new), same as `e541b716` (02:36) | Both: first boot (146 PPU modules), the player browses (modules load on demand until 6:25 / 16:17), then 2–4 min idle with only `sys_hid_manager_ioctl` polling, then the log stops at 1.67–1.70 GB, with no stop and no error. Both restarted the app right after. That looks like the player **leaving through the Xbox guide**. If Quit is chosen while the app is only "constrained" (Xbox button, not suspended), it may end without the Suspending event that .77 uses to tell a deliberate close from a crash, so it's reported as `ended_during_game`. | **Proposal (not coded, because it needs new WinRT ABI declarations and can't be built here):** record window activation/visibility changes (`Window.Activated` / `CoreWindow.VisibilityChanged`) in the telemetry session marker, and on the next start report a run that ended while deactivated as `app_closed` (with a note) instead of a crash. That would also shrink the "silent end" group for #41. #44 already logs Suspending/Resuming. | Medium |

## Pass 12: 2026-10-25, 07:00–07:20 UTC

| What | Installs / reports | Findings | Fix / next step | Confidence |
|------|--------------------|----------|-----------------|------------|
| LittleBigPlanet: `VK_ERROR_OUT_OF_HOST_MEMORY` in `vk::command_buffer::end()` (06:49) | `abb26e7c`, 1 | **Not memory, not a device loss**: 2.79 GB of 5 GB, no `0x887a0005`. `dzn_cmd_buffer.c:1179` is `dzn_EndCommandBuffer`: `ID3D12GraphicsCommandList::Close` failed (usually an invalid command recorded in the list). The port's Mesa patch logs the HRESULT, the D3D12 messages and a command trace to `mesa.log`, but a fatal error uploads only `RPCS3.log`. | [emelin2004/rpcs3-xbox#48](https://github.com/emelin2004/rpcs3-xbox/pull/48): send `mesa.log` and `memory.log` with every fatal error. The next one will show the D3D12 reason. | High (diagnosis path) |
| LittleBigPlanet: savestate load crash in `lv2_socket_native::create_socket` (`ensure(family == SYS_NET_AF_INET)`, 06:55) | `abb26e7c`, 1 | The player used RPCS3's **home-menu "Save state"** at 5:03 and booted the game again. RPCS3 loaded the state and failed restoring a socket with a garbage family. Save and load code of all socket types is symmetric, but the save was taken right after `SPU thread continued after being paused (... unsavable=1)`, so the state was inconsistent. Upstream savestate behaviour. | **Decision for you:** keep "Save state" in the home menu, or hide it on the Xbox until savestates are reliable. No code change. | Medium |
| LittleBigPlanet / XMB / test programs: 5 silent ends | `abb26e7c` | New user trying several things (XMB, `spurs_test.self`, LittleBigPlanet, changing Shader mode to *Legacy* and *Interpreter* in Settings). No error lines. | (#41, guide-quit proposal in pass 11) | — |
| GTA IV BLUS30127: boot fails, `EBOOT.BIN` "Invalid or unsupported file format" | `03552d8b` | Bad or undecryptable copy in `dev_hdd0/game`. The menu says "Couldn't start the game: Invalid file or folder". Not a port bug. | — | High |
| Minecraft BLUS31426: silent end while loading 83 cached PPU modules (06:54), and at 06:24 | `03552d8b`, 2 | 1.3 GB, no error, the log stops mid-load | (#41) | — |

## Pass 13: 2026-10-25, 07:41–07:55 UTC

| What | Installs / reports | Findings | Fix / next step | Confidence |
|------|--------------------|----------|-----------------|------------|
| **Mortal Kombat vs. DC Universe** BLUS30246: `Access violation reading location 0x1a080c84 (unmapped memory)` in PPU `main_thread` at `0x004cedb8` (07:26, 07:35) | `03552d8b`, 2 (identical) | Deterministic: same instruction, same address, about 10 min in (`r10 = 0x1a080c83`, a pointer the game computed). No allocation in the log covers it (all near 0x10000000 or above 0x30000000), so it isn't memory the port's lazy commit missed. Looks like a game-side wrong pointer, i.e. an emulation-accuracy issue (upstream territory, e.g. a missing patch or setting for this game). | No port fix. Worth checking RPCS3's wiki/compatibility entry and patches for BLUS30246, and trying `SPU loop detection` / `Accurate xfloat` in a custom config. | Medium |
| LittleBigPlanet `VK_ERROR_OUT_OF_HOST_MEMORY` at `dzn_cmd_buffer.c:1179` again (07:02) | `abb26e7c`, now 2 | Same `ID3D12GraphicsCommandList::Close` failure after about 6 min, so it reproduces. The `mesa.log` uploaded at 07:05 belongs to a different run (ended at 2:39) and has no `Close failed` line, which confirms the gap #48 closes. | #48 | High (repro) |

### Notes (not bugs yet)
- Portal 2 compiles ~1,700 D3D12 PSOs at ~500 ms each in **every** session (`MESA: dzn: PSOs: 0 from the library, 1728 compiled (avg 494.5 ms)`), even with the Vulkan pipeline cache. That cache keeps SPIR-V→DXIL but not the D3D12 PSO compile, because the PSO library stays off. It's the main "shader compile" cost there.
- 50 Cent menu or loading frames: 1 draw and 836 FIFO commands, but 80–115 ms of GPU time (present wait). Probably a movie or texture-conversion path that is slow on Dozen. Menus only, low priority.
- XMB sessions show 230–760 fps averages on the Performance page, with VSync off and frame limit Auto. Worth checking whether the XMB really runs unlimited, which would waste GPU time and power, or whether the server's average is off.
- GTA V now averages about 25 fps (SPU-bound, 95%), better than the ~10 fps in the notes.
- 50 Cent: stutters are "Present 45%" (the console's present/copy path).
- Minecraft BLUS31426: 37 fps average with 25 stutters/min, classed "Game sync wait".

### Next
- After a new build: check whether #41 brings in heap guard logs for the silent ends, and whether #39 shows why 50 Cent loses its GPU device.
- Portal 2 long frames: find what the game thread waits on (PPU thread sampling would help).
- Each Portal 2 frame logs `NV3089_IMAGE_IN: Operation writes memory onto itself ... Will ignore` (about 30 lines/s). Harmless, but it fills the live log.

- Installs `db91d4cb` and `86f9116c` only opened the app without firmware (0 launches, no errors). Only the app_start log is sent, so a failed firmware install would not show. Maybe log or report firmware install results.
