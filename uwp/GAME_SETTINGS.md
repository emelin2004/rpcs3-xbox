# Recommended game settings

Some games need a setting changed before they run well on the Xbox. This table lists those settings,
so the app itself doesn't have to carry fixes for single games.

To apply one, open the Games tab, highlight the game and press Menu (from 0.1.35.13 on). Change the
settings listed for it, and the app saves them for that game only, in
`LocalState/rpcs3/config/custom_configs/config_<serial>.yml`. "Use the app's settings" removes them
again. A setting marked "config file only" isn't in that menu yet, so it goes into the same
`config_<serial>.yml` by hand, under its section (for example `Core:` then `  Max SPURS Threads: 4`).

| Game | Title IDs | Settings | Without them | Tested on |
| --- | --- | --- | --- | --- |
| Mortal Kombat Komplete Edition | BLUS30902 | CPU: SPU block size: Safe | Stops with "SPU ... Compilation failed" while the first fight loads ([#80](https://github.com/emelin2004/rpcs3-xbox/issues/80)) | Series S, 0.1.35.11, firmware 4.93: menus 54–60 fps, fights 24–30 fps (SPU-bound) |
| Portal 2 | BLUS30732, BLES01222 | CPU: SPU block size: Safe, SPU loop detection: on; `Max SPURS Threads: 4` (config file only) | 22 fps instead of 27 in a big test chamber, with slow frames | By the developer, on the Xbox |
| Minecraft: PlayStation 3 Edition | BLES01976, BLUS31426, NPEB01899, NPUB31419 | Network: Network: Disconnected | With the network connected and no PSN, a new world stays on "Finalizing" | By the developer, on the Xbox (0.1.34.41) |

Up to 0.1.35.16 the app applied the Portal 2 and Minecraft settings by itself. Newer versions don't,
so set them as above.

Found a setting that makes a game work? Add a row, and say in the PR which console and app version
you tested it on.
