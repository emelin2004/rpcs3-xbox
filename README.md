RPCS3 for Xbox (UWP)
====================

Could I please have a little favor from the ones who fork this Repo, either through git or by downloading it: PLEASE remove telemetry code from your forks. There is nothing hidden in how telemetry works, the code is public, but you are spamming my telemetry server with builds of yours which don’t even launch properly… If you are already using AI even for the Builds then Please at least ask it to stop sending random data to my servers. Thanks, Emelin. 

> **Proof of concept.** This is an unofficial, experimental port of the [RPCS3](https://rpcs3.net)
> PlayStation 3 emulator to Xbox Series X|S as a UWP app (Developer Mode). It is **not** supported by
> the RPCS3 team, so please don't ask them for help with it. Expect crashes, slow games and rough edges.
>
> The port is about 99% vibecoded: written with AI assistance (Claude), steered and checked with my
> previous programming knowledge. :)

**Video of it running on YouTube** (click the picture):

[![RPCS3 for Xbox running on an Xbox Series X](https://img.youtube.com/vi/VAKe_y5I3iw/maxresdefault.jpg)](https://youtu.be/VAKe_y5I3iw)

Downloads are on the [Releases](../../releases) page. Building, packaging, internals, all settings and
known issues are in [uwp/README.md](uwp/README.md).

## What works (October 2026)

Based on the developer's own tests and the opt-in telemetry from players' consoles (October 24–27,
official builds 0.1.34.73 to 0.1.35.7). Speeds are typical averages on Series X and S; the first
start of a game is always slower (see below).

Many games below can stop with "Out of host memory". That is not real memory pressure: the Xbox's
graphics driver rejects one frame. It is the main crash left, it affects the whole port, and it is
being worked on.

| Game | State |
| --- | --- |
| PS3 system menu (XMB, firmware 4.93) | Works, 60 fps; network and web browser work |
| Grand Theft Auto: San Andreas | Fully playable |
| Minecraft PlayStation 3 Edition | Playable, 50–57 fps with small stutters (pick "Continue playing offline" at the PSN prompt); sessions up to 37 min |
| Minecraft: Story Mode | Playable, 30 fps (the game's own cap); sessions over 2 hours |
| DRIVER San Francisco | Playable, ~59 fps |
| Jetpack Joyride Deluxe | Playable, ~58 fps |
| Marvel vs. Capcom 2 | Playable, 60 fps (from 0.1.35.5 on) |
| Portal 2 | Playable, ~26 fps (limited by the emulated PPU main thread); short stutters |
| X-Men Origins: Wolverine | Playable, 22–24 fps; can crash after about 30 minutes |
| WipEout HD Fury | In-game, ~45 fps with shader-compile stutters |
| Naughty Bear Gold Edition | In-game, 45–56 fps (limited by the emulated SPUs) |
| Dragon Ball: Raging Blast 2 | In-game, full speed |
| GTA V | In-game, ~22 fps (limited by the emulated SPUs); drops while new SPU code compiles |
| PlayStation Home | In-game, 20–30 fps; sometimes crashes in the main plaza |
| inFamous | In-game, 10–30 fps (GPU-bound) |
| LittleBigPlanet | In-game, ~20 fps; freezes and crashes after a few minutes |
| 50 Cent: Blood on the Sand, SAW II | In-game, but crash often |
| BLEACH: Soul Resurreccion | In-game, ~35 fps; crashes after a few minutes |
| Mortal Kombat vs. DC Universe (PSN), Mortal Kombat Komplete Edition, Injustice: Gods Among Us | In-game, but crash within 1–2 minutes |
| Resistance 2 | In-game, ~20 fps; crashes after about a minute |
| 007 Legends | Boots, intro scene around 40–55 fps |
| Plants vs. Zombies Garden Warfare, Watch Dogs | In-game briefly, not playable |
| L.A. Noire | Menus and intro only, crashes within a minute |
| Formula One Championship Edition, Mortal Kombat vs. DC Universe (disc) | Boot, crash before gameplay |
| FIFA Street | Doesn't boot (the game exits at once) |

Tried a game that isn't listed? Compatibility reports are welcome (see [Contributing](#contributing)).

Heavy games are mostly CPU-bound: the Xbox runs the PPU/SPU recompilers fine, but big games need
more CPU and memory than a UWP game app gets (5 GB). The first start of the XMB or a game compiles
its code and shaders and can take several minutes; later starts use the cache.

## Installing on your Xbox

You need an Xbox Series X|S in **Developer Mode**, a PC on the same network, a **USB drive or
external hard drive** (the emulated PS3 hard drive, games and caches live there; the app's own
storage is far too small), and your own PS3 firmware file (`PS3UPDAT.PUP`, 4.93 is tested) and games.

1. Put the console in Developer Mode (Microsoft's "Xbox Dev Mode" app and a developer account) and
   enable the Device Portal in Dev Home. Open it in a browser on your PC: `https://<your-xbox-ip>:11443/`.
2. From the release, download the `.appx` and `Microsoft.UI.Xaml.2.8.appx`.
3. In Device Portal: **Home > Add**, choose the RPCS3 `.appx`, then on the next page add
   `Microsoft.UI.Xaml.2.8.appx` as a dependency, and install.
4. In Dev Home on the Xbox, highlight **RPCS3 Xbox (Beta)**, press View > **View details** and set
   **App type: Game** (gives the app 5 GB of RAM instead of 1 GB). Reinstalling can reset this.
5. Plug in the USB drive and start the app. Copy `PS3UPDAT.PUP` to the root of the USB drive (or
   `PS3/UPDATE/`), open the **Install** tab and install the firmware.
6. Put games on the USB drive: game folders (with `PS3_GAME` inside) or `.iso` files in `GAMES/`,
   `PS3ISO/` or the root, and `.pkg`/`.rap` files in `PKG/` or the root (install those from the
   Install tab). Press Y in the menu to look again. Boot the XMB or a game from the **Games** tab.

### Controls

| Xbox | PS3 |
| --- | --- |
| A / B / X / Y | Cross / Circle / Square / Triangle |
| Menu / View | Start / Select |
| View + Menu (PS button) | Pause menu (resume, restart, trophies, friends, screenshot, save states, stop the game, the game's settings). On the XMB, hold ~1 s |

In the app menu: D-pad or left stick to move, A select, B back, LB/RB switch tabs, Y rescan.

## Privacy and log sending (telemetry)

On the first start the app asks whether it may send its logs to me, the developer, to help fix the
emulator. Nothing is sent unless you choose **Yes**, and you can turn it off at any time in
**Settings > Send logs to the developer**.

When it is on, the app sends reports to `rpcs3.emelinhabibovic.de`, automatically:

- when you agree (an "install" report, so I can count consoles) and when the app starts,
- when a game, homebrew or the XMB starts and when it stops (with the emulator log),
- after an emulator crash or fatal error, and on the next start after a run that ended while a game
  was running (with that run's logs).

**What is sent:** the emulator log (RPCS3.log, plus Mesa's graphics log and the app's memory log
after a crash), the app version and build commit, the console model (e.g. Xbox Series S), the Xbox
OS version, the PS3 firmware version, the emulator settings, the title ID and name of the game that
ran, how long it ran, and an **anonymous ID**: a random number made on your console when the app
first starts. It is shown at the top right of the menu, above the firmware, so you can mention it
in a bug report. Reports are grouped by that ID only.

**What is not sent:** your Xbox gamertag or Microsoft account, your RPCN username, password, token
or email, other players' names, IP or MAC addresses, your location, or the list of games you have.
The app removes these from the logs before uploading (`uwp/app/uwp_telemetry_scrub.cpp`), and the
server filters them again. The server doesn't store IP addresses. Reinstalling the app makes a new
anonymous ID and asks again. To have your reports deleted, open an issue or contact me with your
anonymous ID.

## Contributing

Pull requests are very welcome, and so are compatibility reports: if you try a game, open an issue
with the game's name and title ID, what happened (fps, crashes, glitches) and the app version.

## License and credits

This port is based on [RPCS3](https://github.com/RPCS3/rpcs3) and keeps its license (GPL-2.0, see
[LICENSE](LICENSE)); all emulation credit goes to the RPCS3 team and contributors. Graphics run on
[Mesa](https://mesa3d.org)'s Dozen driver (Vulkan on D3D12, MIT license) with Microsoft's DXIL
signer. The menu uses Microsoft's WinUI 2 (MIT). The source of everything in a release is in this
repository.

The upstream RPCS3 README follows.

---

RPCS3
=====

[![GitHub Actions](https://img.shields.io/github/actions/workflow/status/RPCS3/rpcs3/rpcs3.yml?branch=master&logo=github&label=Actions)](https://github.com/RPCS3/rpcs3/actions/workflows/rpcs3.yml)
[![RPCS3 Discord Server](https://img.shields.io/discord/272035812277878785?color=5865F2&label=RPCS3%20Discord&logo=discord&logoColor=white)](https://discord.gg/rpcs3)

The world's first free and open-source PlayStation 3 emulator/debugger, written in C++ for Windows, Linux, macOS and FreeBSD.

You can find some basic information on our [**website**](https://rpcs3.net/). Game info is being populated on the [**Wiki**](https://wiki.rpcs3.net/).
For discussion about this emulator, PS3 emulation, and game compatibility reports, please visit our [**forums**](https://forums.rpcs3.net) and our [**Discord server**](https://discord.gg/RPCS3).

[**Support the Lead Developers on Patreon**](https://rpcs3.net/patreon)

## Contributing

If you want to help the project but do not code, the best way to help out is to test games and make bug reports. See:
* [Quickstart](https://rpcs3.net/quickstart)

If you want to contribute as a developer, please take a look at the following pages:

* [Coding Style](https://github.com/RPCS3/rpcs3/wiki/Coding-Style)
* [Developer Information](https://github.com/RPCS3/rpcs3/wiki/Developer-Information)

**Before opening a pull request, please read our [contribution guidelines](.github/CONTRIBUTING.md).** In short:

* Discuss new ideas with the developers first, either in the **#development** channel on [Discord](https://discord.gg/RPCS3) or by opening a GitHub issue.
* Pull requests from first-time contributors must link an issue that a maintainer has assigned to them (`Fixes #<issue number>` in the description). Pull requests without one are closed automatically.
* Test your changes before submitting them. AI-assisted contributions must be disclosed, and you must fully understand and own any code you submit. See the [AI use policy](.github/CONTRIBUTING.md#ai-use).

## Building

See [BUILDING.md](BUILDING.md) for more information about how to setup an environment to build RPCS3.

## Running

Check our friendly [quickstart](https://rpcs3.net/quickstart) guide to make sure your computer meets the minimum system requirements to run RPCS3.

Don't forget to have your graphics driver up to date and to install the [Visual C++ Redistributable Packages for Visual Studio 2022](https://aka.ms/vs/17/release/VC_redist.x64.exe) if you are a Windows user.

## License

Most files are licensed under the terms of GNU GPL-2.0-only License; see LICENSE file for details. Some files may be licensed differently; check appropriate file headers for details.
