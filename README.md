# ReLayer

**An unofficial PC port of Fighting Layer** (Arika / Namco, 1998, Namco System 12 arcade).

ReLayer is a *static recompilation*: a tool translates the arcade game's MIPS code into C, which is compiled
into a native Windows program, with the arcade hardware (graphics, sound, inputs) replaced by portable C.
It is not an emulator, and no game code or data is distributed: Setup builds the game on your PC from your
own ROM. The long-term goal is a readable decompilation and a Sega Dreamcast version.

> ReLayer is a fan project, not affiliated with or endorsed by Arika or Bandai Namco. Fighting Layer is a
> trademark of its owners. You need your own copy of the game.

## Playing

1. Get the download from the [Releases](../../releases) page and unzip it anywhere.
2. Run `Setup.exe`. Windows may say "Windows protected your PC" because Setup is not signed: choose
   **More info**, then **Run anyway**.
3. Choose your ROM, the MAME set **`fgtlayer.zip`** (Fighting Layer, World, Ver. A), and press **Install**.
   Any MAME version's set works: Setup checks the twelve ROM chips by checksum, whatever they are named.
   It builds the game in a minute or two and adds shortcuts. No administrator rights are needed.

What you get:

- **High-resolution 3D** redrawn on your graphics card (up to 8x the arcade's resolution), without the
  original's wobbling polygons and warping textures, or the original picture (F9 switches).
- **Window, borderless or exclusive full screen**, 4:3 or stretched, sharp / smooth / scanlines.
- **Xbox-style controllers** (two players), keyboard, and button remapping in the menu.
- **All characters selectable** from the start (the arcade released three of them over weeks), and the
  arcade's operator settings (difficulty, timer, rounds, free play) in the menu.

### The menu

Press **Back + Start** together on a controller (View + Menu on Xbox Series / One pads), or **Tab** or
**Esc** on the keyboard. The game pauses while it is open.

| | Controller | Keyboard |
|---|---|---|
| Move | D-pad or stick | arrow keys |
| Choose | A | Enter |
| Back | B | Esc |
| Switch pages | LB / RB | Q / E |
| Close | Start | Tab |

Pages: **Display** (window / borderless / exclusive full screen, size, monitor, picture shape, enhanced or
original graphics, quality, screen filter, frame-rate counter), **Audio**, **Controls** (remap any button),
**Game** (unlock all characters, free play, difficulty, damage, timer, rounds and the arcade's other
settings) and **System** (resume, the arcade's operator menu, restart, quit).

### Controls

Insert a coin, then press Start (with free play on, Start alone is enough). The first controller is player 1,
the second player 2.

| | Controller (Xbox-style) | Keyboard, player 1 | Keyboard, player 2 |
|---|---|---|---|
| Move | D-pad or left stick | arrow keys | I J K L |
| Punch light / medium / heavy | X / Y / RB | Z / X / C | Q / W / E |
| Kick light / medium / heavy | A / B / RT | A / S / D | R / T / Y |
| Start | Start | 1 | 2 |
| Insert coin | Back | 5 | 6 |

Other keys: **Alt+Enter** or **F11** window / full screen, **F9** high-resolution 3D on / off, **F2** the
arcade's test button (operator menu).

**Problems:** please open an issue and attach `fgtlayer.log` from the install folder (the previous run's is
`fgtlayer.old.log`). Please don't ask where to get the ROM.

## About

ReLayer was made with Claude, Anthropic's AI model. Its own source code is under the MIT license
(`LICENSE`); that covers ReLayer only, not the game.

---

# For developers

No ROM data is included in this repository. You need your own MAME `fgtlayer.zip` (merged set).

## Setup

```bash
python -m pip install "splat64[mips]" spimdisasm rabbitizer pyyaml
python tools/extract_rom.py fgtlayer.zip     # verify CRCs, write baserom/ and assets/
python tools/gen_ovl_yaml.py                  # generate the overlay splat config
python -m splat split config/main.yaml        # disassemble the main executable -> asm/main
python -m splat split config/ovl.yaml         # disassemble the character overlays -> asm/ovl
```

## The Setup package

`python tools/install/make_package.py --public` makes `dist/public/ReLayer-Setup.zip` (about 80 MB): `Setup.exe`,
a trimmed Python with the disassembler packages, a trimmed 32-bit clang, and this project's source.
It contains no ROM data and nothing made from the ROM. The player unzips it, runs Setup.exe and picks their own
`fgtlayer.zip`; Setup checks it (by CRC, so differently named sets work too), then runs
`tools/install/build_from_rom.py`, which does the steps below on their PC (about 20 seconds on an 8-core PC)
and installs `ReLayer.exe` plus `rom/` into `%LOCALAPPDATA%\Programs\ReLayer` (no admin rights
needed). It adds desktop and Start menu shortcuts and an Apps & features entry; `Uninstall.exe` removes the
game and asks whether to keep the player's settings. Progress and errors go to `setup.log` in the install
folder. `menu_art.png` in the repo root (git-ignored) becomes the menu art, the Setup banner and the icon.

Testing switches: `Setup.exe /rom=<zip> /dest=<folder> /silent /noshortcuts /noregister` (from Git Bash, set
`MSYS_NO_PATHCONV=1` or the `/` switches get turned into paths).

## Build and run (Windows)

Needs CMake, Ninja and a **32-bit** clang/MinGW (`i686-w64-mingw32-clang`). The game's structs hold 32-bit
pointers, so a 64-bit build will not work.

```bash
python tools/recomp/recomp.py                 # MIPS -> C for the main program and all overlays (build/recomp)
cmake -S . -B build/pc -G Ninja -DCMAKE_C_COMPILER=i686-w64-mingw32-clang
cmake --build build/pc
./build/pc/fgtlayer.exe                       # opens a window; Esc opens the menu
./build/pc/fgtlayer.exe --headless --frames 4200   # no window; stop after N frames
./build/pc/fgtlayer.exe --headless --frames 4200 --wav out.wav   # record the audio
```

To play from a Windows PowerShell terminal in the repo folder:

```powershell
.\build\pc\fgtlayer.exe
.\build\pc\fgtlayer.exe --fullscreen
```

**Menu:** press **Tab** or **Esc** (keyboard) or **Back + Start** together (controller) to pause and open the
menu. Settings are saved to `settings.ini`, button mapping to `controls.ini`.

| Page | Options |
|---|---|
| Display | window / borderless full screen / exclusive full screen, window size, monitor, picture shape (arcade 4:3, stretch, whole-number scale), enhanced or original graphics, quality (Auto or a fixed 2x-8x), 3D texture correction, screen filter (sharp / smooth / scanlines), graphics card, frame-rate counter |
| Audio | volume, mute |
| Controls | button remapping ("press the button you want"), stick dead zone, restore defaults |
| Game | unlock all characters, free play, difficulty, damage, round timer, fights per game (1 player / versus), character change on continue / in versus, attract-mode music, event mode |
| System | resume, the arcade operator menu, restart, quit |

The Game page changes the arcade's own operator settings (stored in `eeprom.bin`) and takes effect at once.
**Unlock all characters** (on by default) makes Preston Ajax, Clemence Keliber and Joe Fendi selectable
from the start; the arcade released them 15, 29 and 43 days after the board was first switched on. Scroll
past either end of the character strip to find them.

**Menu art:** put a picture called `menu_art.png` (or `.jpg`) next to where you run the game and it is shown
inside the menu panel under a dark overlay. It is cropped to fill the panel and keeps its right-hand side, so
put the subject on the right; about 2300x1900 looks sharp in 4K.

**Graphics:** 3D is redrawn on the graphics card at a multiple of the arcade's 512x240 that fills the window
(5x = 2560x1200 in 1080p full screen), with sub-pixel vertex precision (no PS1 polygon wobble) and
perspective-correct textures. **F9** switches between this and the original picture while playing.
Quality **Auto** measures how long the graphics card takes per frame and steps the scale down (with a
message) if it gets near the 60 fps budget; the fixed presets never change. On laptops the game asks for the
dedicated GPU; **Graphics card** picks one by hand (takes effect after a restart).
Options: `--native` (original picture only), `--render-scale N` (fixed 1-8x), `--affine` (keep the PS1 texture warping).

**Full screen:** Alt+Enter or F11 switches between the window and full screen (borderless, or exclusive if
that was chosen in the menu). Borderless alt-tabs instantly; exclusive takes over the monitor and falls back
to borderless if Windows refuses it. The picture keeps the arcade monitor's 4:3 shape with black bars on wide
screens unless Picture shape says otherwise.

Other options: `--fullscreen` (start in full screen), `--mute` (no audio output), `--script file` (scripted
inputs and screenshots), `--keep-eeprom` (let a `--headless` run read and write `eeprom.bin`).

Settings and high scores live in `eeprom.bin`. Headless runs start from a blank EEPROM in memory and never
touch that file, so scripted tests can't wipe a player's settings.

### Controls

**Xbox controller** (360 / One / Series, wired or wireless; controller 1 = player 1, controller 2 = player 2,
plug in any time):

| Cabinet | Xbox |
|---|---|
| Joystick | D-pad or left stick (8-way) |
| Buttons 1 / 2 / 3 (top row) | X / Y / RB |
| Buttons 4 / 5 / 6 (bottom row) | A / B / RT |
| Start | Start (Menu) |
| Coin | Back (View), counted when released |
| Pause menu | Back + Start together |

Remap in `controls.ini` (e.g. `b6 = LT`, `coin = BACK LB`, `stick_deadzone = 0.35`). The mapping logic has a
hardware-free test: `build/pc/pad_map_test.exe`.

The first connected controller is player 1 and the second is player 2, whichever XInput slots they occupy.
If a controller seems dead, look for `pad:` lines in `fgtlayer.log`: every button change is logged with the
raw XInput bits and the cabinet bits it produced. `FGT_FAKE_PAD="frame:hexbuttons:hold,..."` feeds scripted
XInput states through the same code (e.g. `1500:0020:6,1600:0010:6` = Back, then Start).

**Keyboard:** P1 arrows, Z X C / A S D (buttons 1-6), 1 = start, 5 = coin. P2 I J K L, Q W E / R T Y,
2 = start, 6 = coin. F2 = test button, F1 (held) = service DIP switch, Alt+Enter / F11 = full screen,
F9 = high resolution on/off, Tab / Esc = menu (quit from the menu's System page).

### Scripted tests

`tests/*.txt` drive the game without a keyboard, for checking that changes haven't broken anything:

```bash
./build/pc/fgtlayer.exe --headless --frames 2700 --script tests/moves.txt
```

Each line is `<frame> <hold-frames> <input>` (inputs: `coin1`, `coin2`, `test`, `service`, `p1.`/`p2.` +
`up down left right b1..b6 start`), `<frame> shot <name>` to save a screenshot, or `<frame> set <name>=<value>`
to change an arcade setting (`free_play`, `difficulty`, `damage`, `timer`, `fights_1p`, `fights_vs`,
`change_continue`, `change_vs`, `attract_music`, `event_mode`), or `<frame> poke <hex address>=<value>` to
write a byte of game RAM. Every headless run is a clean first boot (blank EEPROM). Useful addresses: the
arcade CPU opponent `8034BC7E` and stage `8033542C` (both chosen at frame 2012 after a start at 1600; stages
0-15; the arcade order is 6, 11, 2, 10, 3, 13, then 9 or another, and 12 for Vold). `FGT_WATCH=<hex address>` logs every change of a RAM byte.
The previous run's log is kept as `fgtlayer.old.log`.

| Script | Checks |
|---|---|
| `tests/coin_start.txt` | coin → credit, start → player select |
| `tests/select_fight.txt` | cursor movement, picking Lan, the match loading |
| `tests/moves.txt` | walk forward/back, jump, punch, kick, crouch in a match |
| `tests/p2_and_test.txt` | second player joining |
| `tests/testmenu.txt` | operator test menu and navigating it |
| `tests/switchtest.txt` | the test menu's SWITCH TEST screen |
| `tests/buttons.txt` | each of the six buttons alone in a match |
| `tests/hd_shots.txt` | run windowed: saves software and high-resolution (`*_hd.bmp`) pictures of the same frames |
| `tests/ui_demo_inputs.txt` | with `FGT_UI_DEMO=screenshots/menu`: walks through every menu page and saves the screen |
| `tests/round_start.txt` | run windowed: every 3rd frame from select screen to round start, both renderers |
| `tests/unlock_select.txt` | the character strip scrolls to the three time-release characters |
| `tests/freeplay.txt` | turning free play on while the game runs: Start alone begins a game |
| `tests/boss_vold.txt` | arcade against the last boss on his stage (forced with `poke`) |
| `tests/opmenu_game.txt`, `tests/opmenu_coin.txt` | the operator menu's GAME and COIN pages |

Run it from the repo root (it reads `baserom/`; an installed copy reads `rom/` next to the exe). It writes
`fgtlayer.log` (everything the runtime noticed) and `eeprom.bin` (cabinet settings); headless runs also write
`screen.bmp` (last frame) and `vram.bmp` (video memory).

If the log says a call went to an unknown function entry, the address is appended to `missing_entries.txt`.
Copy it into `config/recomp_extra_funcs.txt` and re-run the recompiler.

## How it works

1. `tools/recomp/recomp.py` turns every MIPS function into a C function that works on an emulated register
   file and the game's own 4 MB RAM image (`port/core/recomp.h`).
2. `port/core/` replaces the hardware and the Sony kernel: interrupts/DMA/timers (`hw.c`), kernel calls,
   threads and events (`bios.c`), the 3D-maths coprocessor (`gte.c`), and the GPU with a software renderer
   (`gpu.c`, `gpu_raster.c`). The sound/I-O board runs the original H8 program on an H8/300H interpreter
   (`h8.c`) with a C352 sample-chip emulation (`c352.c`), glued together in `s12sound.c`.
3. `port/pc/` is the Windows layer: window, timing, coroutines for the game's threads.
4. `src/game/` is hand-written C that replaces recompiled functions one at a time (see `docs/PLAN.md`). A
   function listed in `config/recomp_overrides.txt` is recompiled as `f_<address>_orig`, so the C version in
   `src/` takes its name and can still call the original. The first ones: the character release clock
   (`release.c`) and the arcade settings (`arcade_settings.c`).

## Layout

| Path | Contents |
|---|---|
| `tools/` | Extraction and analysis scripts |
| `config/` | splat configs and symbol lists |
| `docs/HARDWARE.md` | ROM and memory map findings |
| `docs/PLAN.md` | Roadmap (recompile → HLE → decompile → Dreamcast) |
| `baserom/`, `assets/`, `asm/`, `build/` | Generated from your ROM (git-ignored) |
| `tools/recomp/` | The static recompiler |
| `tools/h8dis.py` | H8/300H disassembler for the sound program |
| `port/core/` | Portable runtime: hardware, kernel, GTE, GPU |
| `port/pc/` | Windows platform layer (later `port/dc/` for Dreamcast) |
| `port/ui/` | Pause menu and settings (platform independent) |
| `src/game/` | Hand-written / decompiled game code |
| `tools/install/` | Setup: the from-ROM build script, Setup.exe source, the packaging script |
