# TyrQuakeCell

A native homebrew port of [TyrQuake](https://github.com/RetroPie/tyrquake) for the PS3.

## Lineage

TyrQuake is itself a conservative, actively maintained branch of the original
Quake source code released by id Software in 1999. This project adds a full
PS3 platform layer on top of it (video, audio, input, system integration).
In short:

```
Quake (id Software, 1996) -> TyrQuake (maintained fork) -> TyrQuakeCell (this project)
```

## Features

- **One package, one launcher** (1.6.0): Quake, both mission packs and
  your mods in a single install. At start a menu lists what is installed
  (skipped when there is only one thing to play); Quit from the game's menu
  comes back to it.
- **Video**: TyrQuake's OpenGL renderer drawn by the RSX (stable 60fps, up
  to 1280x720), or the original software renderer, chosen in Options ->
  Video Settings -> Renderer (the game restarts in the other one: the two
  renderers are two programs in the package; Square in the launcher
  changes it too). Everything else is set from Video Settings as well: resolution,
  HUD scale, brightness, gamma, TV screen fit, scaling, frame rate, FPS
  display, and (OpenGL only) texture filtering (off / bilinear /
  trilinear, `gl_texturemode`) and model shadows (`r_shadows`).
- **Audio**: real hardware audio on its own dedicated thread, OGG background
  music (drop your own legally-owned soundtrack files in, see
  [Installation](#installation)).
- **Input**: DualShock 3 (movement, look, every action), USB mouse, USB
  keyboard (hit-or-miss depending on the specific keyboard model -- a
  known PS3-side limitation, not something this project can fix). Every
  action is rebindable from the in-game Customize Controls menu, including
  the quicksave/quickload shortcuts this port adds and an optional
  **lock-on** (unbound by default). Mouse and joystick sensitivity are both
  adjustable from Options.
- **Console**: Select + Triangle opens it from anywhere, and the PS3's own
  on-screen keyboard types the commands -- no USB keyboard needed.
- **Compatibility**: shareware and full registered Quake, save/load (regular
  and quicksave/quickload), demo playback, both official mission packs
  (*Scourge of Armagon* and *Dissolution of Eternity*) and mods (`-game`
  folders), all sharing one copy of the base game data.

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

3. **Mission packs are optional**: drop each expansion's own `pak0.pak` into
   its folder and it shows up in the launcher:

   ```
   TYRQ00001/USRDIR/hipnotic/pak0.pak   <- Scourge of Armagon
   TYRQ00001/USRDIR/rogue/pak0.pak      <- Dissolution of Eternity
   ```

   Both mission packs shipped with their own soundtrack, separate from the
   base game's. If you have those, they go in `hipnotic/music/` and
   `rogue/music/` respectively, same naming convention as above.

4. **Mods**: every other folder in `USRDIR` with a `pak0.pak` or a
   `progs.dat` in it is listed on the launcher's MODS tab (L1/R1) and runs
   as `-game <folder>`. An optional `mod.txt` in the folder gives the name
   shown, as `name = Arcane Dimensions`. Mods made for the mission packs
   (which need `-hipnotic` / `-rogue` on top of their own folder) are not
   supported.

   If you installed the separate mission pack or "SW" packages of earlier
   versions (`TYRQHIP01`, `TYRQROG01`), they can be deleted: this one
   package does everything, with the same data, saves and `config.cfg`.

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
   git clone https://github.com/stingziz0u/TyrQuakeCell.git
   cd TyrQuakeCell/tyrquake-ps3
   make                                 # -> TyrQuake.gnpdrm.pkg
   make DEBUG_BUILD=1                   # -> TyrQuakeDebug.gnpdrm.pkg
   ```

`make` builds both programs, OpenGL (`EBOOT.BIN`, and `RELOAD.SELF` for
starting it again) and software (`TYRQUAKE_SW.SELF`), each in its own
folder (`obj/<target>`), and puts them in the one package. The launcher
starts the renderer chosen in Video Settings (saved in
`USRDIR/ps3_renderer.txt`).

`DEBUG_BUILD=1` adds testing tools not present in the release build: noclip
and god mode (bindable from Customize Controls, not bound by default), an
L1+R1 "skip to next level" shortcut (uses `changelevel` directly, so it also
skips past the Chthon and Shub-Niggurath boss encounters rather than
requiring them), and verbose diagnostic logging. It installs next to the
release package (its own APPID) and plays the same game data.


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
| Scoreboard | Select | While held (`+showscores`) |
| Lock on | *(unbound)* | Bind it from Customize Controls ("lock on"). While held, the view turns toward the visible enemy closest to the crosshair and follows it; the sticks still work |
| Console | Select + Triangle | Opens / closes it, in game or in the menu |
| Clear a binding | Select | While inside Customize Controls only |

Everything above except Start, the console and Select's menu role is
rebindable from Options -> Customize Controls. Select is also the console
modifier: on its own it reaches the game when released, or after a quarter
of a second while held, so Select + Triangle never triggers either button.

With the console open:

| Action | Button |
|---|---|
| Type a command (PS3 on-screen keyboard, runs it when accepted) | Cross |
| Run the current line | Triangle |
| Complete the command (Tab) | Square |
| Command history / erase a character | D-pad up, down / left |
| Scroll the console | L1 / R1 |
| Close the console | Circle (or Select + Triangle) |

## Known limitations

- USB keyboard support depends entirely on the specific keyboard model --
  some are read correctly, some report as connected but never produce any
  key data. This traces back to the PS3's own low-level `ioKb` driver, not
  something fixable from application code.

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
