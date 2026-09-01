# TyrQuakeCell

A native homebrew port of [TyrQuake](https://github.com/RetroPie/tyrquake) for the PS3.

"Native" matters here: this is a real homebrew executable built directly against
PSL1GHT (RSX, `libaudio`, `libpad`, etc.), not a libretro core running inside
RetroArch. It boots straight from the XMB.

## Lineage

TyrQuake is itself a conservative, actively maintained branch of the original
Quake source code released by id Software in 1999. This project adds a full
PS3 platform layer on top of it (video, audio, input, system integration)
In short:

```
Quake (id Software, 1996) -> TyrQuake (maintained fork) -> TyrQuakeCell (this project)
```

## Features

- **Video**: 720p output, selectable internal render resolution (320x200 up
  to 640x400 tested; 512x384 is the shipped default), hardware-accelerated
  scaling via the RSX 2D transfer unit (not a CPU blit), triple-buffered
  presentation, HUD/menu scaling that adapts to whatever resolution is set.
- **Audio**: real hardware audio on its own dedicated thread, OGG background
  music (drop your own legally-owned soundtrack files in, see
  [Installation](#installation)).
- **Input**: DualShock 3 (movement, look, every action), USB mouse, USB
  keyboard (hit-or-miss depending on the specific keyboard model -- a
  known PS3-side limitation, not something this project can fix). Every
  action is rebindable from the in-game Customize Controls menu, including
  the quicksave/quickload shortcuts this port adds. Mouse and joystick
  sensitivity are both adjustable from Options.
- **Compatibility**: shareware and full registered Quake, save/load (regular
  and quicksave/quickload), demo playback, both official mission packs
  (*Scourge of Armagon* and *Dissolution of Eternity*) as separate installs
  sharing one copy of the base game data.

## Requirements

- A PS3 capable of running homebrew (either HEN or CFW works fine)
- Your own legally-owned copy of Quake. **No game data is included
  in this repository or in any release build** -- see
  [Installation](#installation).
- To build from source: the [ps3dev / PSL1GHT](https://github.com/ps3dev)
  toolchain (`ppu-gcc`, PSL1GHT SDK, `make_self_npdrm`, `pkg.py`, etc.).

## Installation

1. Install `TyrQuake.gnpdrm.pkg` from the XMB (shows up as "TyrQuakeCell").
   This creates `/dev_hdd0/game/TYRQ00001/USRDIR/`, including empty
   `id1/music/`, `hipnotic/music/`, and `rogue/music/` folders ready to use.
2. By FTP, place your own game data there:

   ```
   TYRQ00001/USRDIR/id1/pak0.pak        <- required (shareware or full)
   TYRQ00001/USRDIR/id1/pak1.pak        <- optional, full game only
   TYRQ00001/USRDIR/id1/music/          <- optional, track02.ogg .. track11.ogg
   ```

3. **Mission packs are optional, separate installs** that share the data
   above -- install `TyrQuakeHipnotic.gnpdrm.pkg` and/or
   `TyrQuakeRogue.gnpdrm.pkg` from the XMB (shown as "TyrQuakeCell (Scourge
   of Armagon)" and "TyrQuakeCell (Dissolution of Eternity)", each with its
   own icon), then drop each expansion's own `pak0.pak` into:

   ```
   TYRQ00001/USRDIR/hipnotic/pak0.pak   <- Scourge of Armagon
   TYRQ00001/USRDIR/rogue/pak0.pak      <- Dissolution of Eternity
   ```

   Both mission packs shipped with their own soundtrack, separate from the
   base game's. If you have those, they go in `hipnotic/music/` and
   `rogue/music/` respectively, same naming convention as above.

   **Filenames are case-sensitive.** `pak0.pak` and `PAK0.PAK` are two
   different files as far as the PS3's filesystem is concerned -- if a pak
   isn't showing up, this is the first thing to check.

## Building from source

1. Install the [ps3dev toolchain](https://github.com/ps3dev/ps3toolchain)
   (third-party, not part of this project -- these are its own official
   instructions):

   ```bash
   git clone https://github.com/ps3dev/ps3toolchain.git
   cd ps3toolchain
   export PS3DEV=/usr/local/ps3dev
   export PSL1GHT=$PS3DEV
   export PATH="$PATH:$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin"
   ./toolchain.sh
   ```
2. Clone this repository and build:

   ```bash
   git clone https://github.com/<stingziz0u>/TyrQuakeCell.git
   cd TyrQuakeCell/tyrquake-ps3
   make clean && make                       # base game -> TyrQuake.gnpdrm.pkg
   make clean && make MISSIONPACK=hipnotic  # -> TyrQuakeHipnotic.gnpdrm.pkg
   make clean && make MISSIONPACK=rogue     # -> TyrQuakeRogue.gnpdrm.pkg
   make clean && make DEBUG_BUILD=1         # -> TyrQuakeDebug.gnpdrm.pkg
   ```

`make clean` between builds is required -- object files bake in different
`CFLAGS` per variant and will silently go stale otherwise.

`DEBUG_BUILD=1` adds testing tools not present in the release build: noclip
and god mode (bindable from Customize Controls, not bound by default), an
L1+R1 "skip to next level" shortcut (uses `changelevel` directly, so it also
skips past the Chthon and Shub-Niggurath boss encounters rather than
requiring them), and verbose diagnostic logging. `DEBUG_BUILD` can be
combined with `MISSIONPACK` too.

Internal render resolution is a compile-time constant (`BASEWIDTH` /
`BASEHEIGHT` in `tyrquake-ps3/common/vid_ps3.c`) -- edit and rebuild if you
want something other than the shipped 512x384.

## Controls

| Action | Default | Notes |
|---|---|---|
| Move / Strafe | Left stick | |
| Look | Right stick | |
| Jump / Swim up | Cross | |
| Attack | R2 | |
| Run | L2 | |
| Prev / Next weapon | L1 / R1 | Also mouse wheel down/up |
| Swim up / down | L3 / R3 | |
| Quicksave / Quickload | Triangle / Circle | |
| Confirm (menu) | Cross | |
| Back / Cancel (menu) | Circle or Start | |
| Open/close menu | Start | Fixed, not rebindable |
| Clear a binding | Select | While inside Customize Controls only |

Everything above except Start and Select's menu role is rebindable from
Options -> Customize Controls.

## Known limitations

- USB keyboard support depends entirely on the specific keyboard model --
  some are read correctly, some report as connected but never produce any
  key data. This traces back to the PS3's own low-level `ioKb` driver, not
  something fixable from application code.
- The in-game "Video Options" menu was removed rather than wired up --
  internal render resolution is a build-time choice (see
  [Building from source](#building-from-source)) rather than a runtime
  menu option.

## Credits

- [TyrQuake](https://github.com/RetroPie/tyrquake) by Kevin Shanahan
  (Tyrann) and contributors -- the engine this project is built on.
- id Software, for releasing the original Quake source code under the GPL.
- Reference PS3 homebrew projects whose code patterns this project directly
  learned from:
  **dragonfly-quake-ps3** (RSX init/present sequence)
  **GamePad Tester** (confirmed-working `ioPad` usage)
  **xash3d-fwgs**'s PS3 port (diagnosed and fixed the same hard-30fps double-buffering
  quantization bug this project also hit).

## License

GPLv2, inherited from TyrQuake and, through it, id Software's original
Quake source release. See [`tyrquake/gnu.txt`](tyrquake/gnu.txt).

No Quake data files (`.pak`, music, or otherwise) are included in this
repository. You need your own legally-owned copy of the game.

## AI disclosure

This project's code was written collaboratively with Claude (Anthropic),
working through this port with me in real time over many sessions.
Every bit of testing and debugging were made by me on real hardware.
