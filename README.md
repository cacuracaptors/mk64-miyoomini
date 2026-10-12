# Mario Kart 64 - Miyoo Mini Plus / OnionOS Port

This is a port of [**mk64-dc**](https://github.com/jnmartin84/mk64-dc) - jnmartin84's Sega Dreamcast port of
the [Mario Kart 64 decompilation](https://github.com/n64decomp/mk64) - to the **Miyoo Mini Plus** handheld
running **OnionOS**.

The Miyoo Mini Plus has no 3D GPU, so this port draws the game with a software renderer written for it:
NEON-accelerated and spread over both CPU cores. It plays the **USA version** of Mario Kart 64, built from your
own ROM.

**[Download the latest release](https://github.com/cacuracaptors/mk64-miyoomini/releases)**

## OnionOS Exclusive Features

- Native 640x480, no overclock needed
- 60 FPS single-player races, with dynamic resolution: in the heaviest moments the picture drops for an
  instant to as low as 448x336 instead of the frame rate dropping
- A software renderer made for the Miyoo Mini Plus:
  - perspective-correct textures and a proper depth buffer
  - rendering pipelined over both CPU cores
  - the N64's 3-point texture filter on the road and other close-up surfaces
  - a smooth, filtered waterfall in D.K.'s Jungle Parkway
- No overclock
- Smooth controls: steering ramps up while held
- Full sound: the N64 music and sound effects, mixed in software
- Records, options and ghosts are saved
- Easy installation: just copy your ROM (`.z64`, `.n64`, `.v64` or zipped) into the game's folder. The game data
  is built from it automatically on the first start, with a progress screen, and the ROM is removed afterwards
- The Menu key quits the game on a single press, and the OnionOS Menu+Power screenshot combo is safe to use

## Installation

1. Download the release zip and extract it to the root of your OnionOS SD card. This places the `MarioKart64`
   folder in `Roms/PORTS/Games/`, and the shortcut and its image in `Roms/PORTS/Shortcuts/` and
   `Roms/PORTS/Imgs/`.
2. You need your own legitimate copy of **Mario Kart 64 (USA)** as a ROM file. Copy it into
   `Roms/PORTS/Games/MarioKart64/`. Any of `.z64`, `.n64` or `.v64` works, also inside a `.zip`; the file name
   doesn't matter.
3. On OnionOS, open **Mario Kart 64** from the Ports list (use "Refresh roms" at the bottom of the list if it
   doesn't show up).

The first start builds the game data from the ROM. It takes about five minutes, shows a progress screen, and only
happens once: the ROM is removed at the end. It needs about 25 MB of free space on the SD card; if there isn't
enough, the screen says how much is needed. If the ROM is not the USA version (or was modified), the screen says
so and nothing is changed.

When you update the port to a new version, copy the ROM into the folder again: the new version rebuilds its data
from it on its first start.

## Controls

| Button | Action |
|---|---|
| D-pad | Steer / move in menus |
| A | Accelerate / confirm |
| B | Brake, reverse / back |
| R1 or R2 | Hop / drift |
| L1 or L2 | Use item (L1 is also the N64 L button in menus) |
| Y | Change the camera (C-up) |
| X | C-right |
| Start | Pause / start |
| Select | N64 L button (the "L: Option" menu) |
| Menu | Quit the game (press and release) |

Select+Start held for one second also quits.

## Options

The shortcut (`Roms/PORTS/Shortcuts/Mario Kart 64.port`) starts the game with `-hires -60fps -resdin -filtro`.
You can edit its `Arguments` line:

| Option | Effect |
|---|---|
| `-hires` | Renders at 640x480 (without it: 320x240, faster and blurrier) |
| `-60fps` | Single-player races at up to 60 FPS (without it: 30 FPS, as on the N64) |
| `-resdin` | Dynamic resolution (with `-hires`): smaller frames only while a frame doesn't fit in 1/60 s |
| `-filtro` | N64-style texture filter on the road and other close-up surfaces |
| `-mudo` | No sound |
| `-perf` | Timing lines in `log.txt` every 2 seconds |

## Known issues

- Occasionally, the second screen showing your competitors after the race finishes briefly flashes with random rendering artifacts.
- On the Award Ceremony screen, the floor disappears depending on the camera angle and position.
- Some tracks are missing filtering on their ground textures.
- An OnionOS screenshot (Menu+Power) pauses the game for about a second while it is saved.
- The game's log is in `log.txt`, in `Roms/PORTS/Games/MarioKart64/` - please attach it to your reports.

## Changelog

- **v1.0.0** - Initial release.

## Building from source

This port cross-compiles for ARMv7 hard-float on Windows + WSL2, with the
[union-miyoomini-toolchain](https://github.com/shauninman/union-miyoomini-toolchain) (the same glibc as OnionOS).

### Prerequisites

- WSL2 with Ubuntu, `python3` and `gcc`
- The toolchain copied out of its Docker container once, to `~/fallout-miyoo/miyoomini-toolchain`:
  `docker cp miyoomini-toolchain-debug:/opt/miyoomini-toolchain ~/fallout-miyoo/miyoomini-toolchain`
  (or point `CROSS_PREFIX` at your own `arm-linux-gnueabihf-`)
- Your Mario Kart 64 (USA) ROM as `baserom.us.z64` in the root of this repository

### Steps

```bash
sh port/build_miyoo.sh          # the game, with its data, in ~/fallout-miyoo/mk64-pacote (for testing)
sh port/release_miyoo.sh 1.0.0  # the release zip, without any game data
```

`release_miyoo.sh` takes the game data out of the executable (`port/romkit/romdata.py`), writes the recipe
`mk64.rcp` that rebuilds it - and all of `dc_data/` - from the player's ROM (`port/romkit/romrecipe.c`), checks
that everything comes back byte for byte, and zips the OnionOS folder layout. The recipe holds no game data:
only where each piece comes from in the ROM, plus the few bytes the port itself changed.

### What this fork changes (compared to upstream jnmartin84/mk64-dc)

- **`port/gfx_sw.c`** - the software renderer that replaces the Dreamcast PowerVR backend: NEON span
  rasterizer, fixed-point depth buffer, translucency sorted like the PowerVR, the 3-point filter, and frames
  drawn on the second core while the first prepares the next one
- **`port/video_miyoo.c`**, **`port/gfx_wm_miyoo.c`** - the screen: frames converted, rotated and scaled by the
  Miyoo Mini's 2D engine (MI_GFX), the game clock taken from the panel's vsync, dynamic resolution
- **`port/audio_miyoo.c`**, **`port/mixer_miyoo.c`**, **`src/audio/synthesis.c`** - sound: the N64 software
  synthesis path (the Dreamcast mixed on its AICA chip) with the audio microcode commands in C, output
  through MI_AO
- **`port/input_miyoo.c`** - the Miyoo Mini Plus buttons
- **`port/saves_miyoo.c`** - the EEPROM and the Controller Pak (ghosts) as files in `saves/`
- **`port/setup_miyoo.c`**, **`port/romkit/`** - the first-run setup that rebuilds the game data from the
  player's ROM with a progress screen, and the release tools
- **`port/port_main.c`**, **`port/kos_shim.c`**, **`port/n64_stubs.c`**, **`port/include/`** - startup,
  options and the KallistiOS services the game code still calls
- **`Makefile.miyoo`**, **`port/build_miyoo.sh`**, **`port/release_miyoo.sh`**, **`port/tools/`** - the ARM
  build
- **`src/`** - small fixes for the ARM build and for 60 FPS (the race steps by the vblanks a frame took)

## Credits

- Mario Kart 64 decompilation by the n64decomp team - https://github.com/n64decomp/mk64
- Mario Kart 64 for Dreamcast by jnmartin84 - https://github.com/jnmartin84/mk64-dc
- Software audio mixer from the Super Mario 64 PC port (sm64-port)
- Miyoo Mini audio output (MI_AO) based on Steward Fu's SDL2 port - https://github.com/steward-fu/sdl2
- Miyoo Mini toolchain by Shaun Inman - https://github.com/shauninman/union-miyoomini-toolchain
- tinf (zip support in the setup) by Joergen Ibsen - https://github.com/jibsen/tinf
- Setup screen text in DejaVu Sans Mono - https://dejavu-fonts.github.io
- Original Mario Kart 64 (1996) by Nintendo
- Miyoo Mini Plus port by [cacuracaptors](https://github.com/cacuracaptors)

## License

The decompilation and the Dreamcast port this fork is based on come without a license file; the code added for
the Miyoo Mini Plus follows the same terms. tinf is under the zlib license (see `port/romkit/tinflate.c`).
Mario Kart 64 is a trademark of Nintendo; no game data is included - you need your own copy of the game.
