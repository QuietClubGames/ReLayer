# Plan: Fighting Layer → C → Windows → Dreamcast

## Scale

- Main executable: 1,716 functions after splitting (about 313 sit at or above `0x802C0000`, roughly where the
  Sony PsyQ library starts; the rest is game code)
- 21 character overlays: about 1,195 functions
- **About 2,600 game functions to decompile**, plus a hardware layer to replace the Sony libraries.

For comparison, matching PS1 decomps of similar size have taken teams years. A playable Windows build
is much closer if we **don't wait for every function to be hand-decompiled**. That is the basis of the plan below.

## Approach: static recompilation first, decompilation over time

1. **Disassembly.** splat + spimdisasm split `main.bin` and the overlays into `asm/`.
2. **Static recompiler (`tools/recomp/recomp.py`).** Mechanically translates every MIPS function into C that runs
   on a register context (`cpu.r[...]`), with memory accesses going into a 4 MB `g_ram[]` array plus a 1 KB
   scratchpad. Output compiles with any C compiler, so the *whole game* builds without understanding it first.
3. **HLE platform layer (`port/`).** Replaces the Sony kernel/libraries and the System 12 hardware with C:
   - GPU → software PS1-GPU rasterizer (later an OpenGL/PowerVR path)
   - GTE → C implementation
   - kernel → events, threads (host coroutines), interrupt hook, heap
   - banked ROM, EEPROM, watchdog, shared RAM
   - inputs → keyboard/pad (to do)
   - sound: the H8/3002 + C352 driver. Either emulate the H8 and C352, or high-level emulation of the
     driver's command set.
4. **Decompilation (ongoing).** Replace recompiled functions one at a time with readable C (m2c output, cleaned up
   by hand). Check each replacement against the recompiled version in the same build: run both, compare RAM.
   The game stays playable throughout.
5. **Dreamcast.** Once most code is real C, build the same sources with KallistiOS (SH-4 GCC):
   - GPU → PowerVR2 backend (convert PS1 VRAM texture pages and CLUTs into PVR textures with a cache)
   - sound → AICA (the C352 samples are 8-bit linear/µ-law PCM, which converts directly)
   - assets (≈ 48 MB with samples) don't fit in 16 MB RAM, so stream them from GD-ROM. The game already
     loads everything through its file table, so that is the natural seam.

### Ground rules that keep the Dreamcast port possible

- **Build 32-bit (i686) on Windows.** Game structs hold 32-bit pointers; a 64-bit build would break them.
  Dreamcast is 32-bit too.
- Use only C99 plus a thin platform layer (`port/pc`, later `port/dc`). No Win32 calls outside `port/pc`.
- Keep the game's 4 MB address-space model until the code is fully decompiled.

## Alternative: matching decomp

Byte-identical recompilation with the original PsyQ GCC (likely 2.7.2/2.8.x via decomp.me presets) and `maspsx`.
Most rigorous, but it needs a MIPS toolchain and gives no playable build until nearly everything matches. It can
be layered on later; the splat setup is shared.

## Status

- [x] ROM extraction + CRC check (`tools/extract_rom.py`)
- [x] Program ROM, banked ROM, file table, overlay slots mapped (`docs/HARDWARE.md`)
- [x] splat configs for main (`config/main.yaml`) and overlays (`config/ovl.yaml`, generated)
- [x] BIOS stubs named (`tools/find_bios_stubs.py`)
- [x] **Static recompiler**: 1,716 main functions + 21 × 2 overlays → C
- [x] **Runtime**: interrupts, DMA, timers, kernel calls, threads, events, EEPROM, banked ROM window
- [x] **GTE** in C (`port/core/gte.c`)
- [x] **Software GPU renderer** (polygons, sprites, lines, textures, blending, 2 MB VRAM). Triangles use exact
      integer interpolation and the top-left fill rule; float rounding caused seams between textured quads.
- [x] **High-resolution renderer**: `port/core/gpu_hd.c` records every drawing command aimed at a screen buffer
      (with unrounded GTE positions and depth); `port/pc/gpu_hd_d3d11.c` redraws the displayed frame at an
      integer scale, decoding 4/8/15-bit textures from a VRAM snapshot in the pixel shader, with the four PS1
      blend modes (two passes for per-texel translucency). The software renderer still runs for VRAM accuracy
      and is the fallback (unknown uploads/copies into a screen buffer). Checked frame by frame against the
      software picture; 5x with sound: 59 of 60 seconds without a repeated frame on the RTX 5070 Ti.
      Lesson: the VS screen stays up while the next fight's textures load, so textures must come from a
      snapshot taken when the frame went on screen, not live VRAM.
- [x] **Pause menu** (`port/ui/menu.c`, platform-neutral; drawn by `port/pc/ui_d3d11.c`): Tab / Esc or
      Back + Start. Display, Audio, Controls (remapping), Game (arcade settings, unlocks), System pages, all
      at one fixed height with an optional `menu_art.png` behind them; settings saved to `settings.ini`;
      toasts and an optional frame-rate counter. Rounded panels are signed-distance shapes in a pixel shader,
      text comes from a glyph atlas rasterised at the window's exact pixel size, the paused game is dimmed
      and blurred behind it.
- [x] **Timing and display**: drawn with Direct3D 11 through a flip-model swap chain; `Present` with vsync
      paces the game on 60 Hz screens (sync interval 2 on 120 Hz, etc.; a 1 ms-precision 60 Hz timer
      otherwise), with one frame allowed in the queue to absorb a slow frame. GDI (`StretchDIBits` +
      `DwmFlush`) remains as a fallback. Audio uses rate control (each frame stretched/squeezed by < 2%) to
      stay ~3 buffers ahead. Every second the log prints fps, worst frame time and "repeated frames on
      screen" (exact, from DXGI frame statistics).
      History: `Sleep(1)` pacing ran at ~48 fps; syncing to the sound card gave 60 fps with 6-25 ms frame
      gaps; GDI + vsync had ~10-14 repeated frames a minute; Direct3D 11 measured 1 in two minutes of play.
- [x] **Display options**: window (sizes 640x480-1920x1440), borderless or exclusive full screen
      (`SetFullscreenState`, re-taken after alt-tab), monitor choice, picture shape, graphics card choice
      (most video memory by default), quality presets and Auto quality (GPU timestamp queries; lowers the
      scale when the high-resolution pass needs more than 9 ms)
- [x] **First C replacements** (`src/game/`, via `config/recomp_overrides.txt`): `func_802B3F98` (hours since
      the release-start date) answers "long enough" when Unlock all characters is on; arcade settings are
      read and written as EEPROM bit fields and patched into the running game's RAM copies
- [x] **Windows build boots and runs the attract mode**: boot screens, logos, intro, 3D demo fight, rankings
- [x] **Sound**: the real H8/3002 sound program runs on an H8/300H interpreter (`port/core/h8.c`), driving a
      C352 emulation (`port/core/c352.c`); Windows audio via waveOut, `--wav` recorder. Intro and demo music play.
- [x] **Controls and coins** through the real H8 path (`0x300000-3`), keyboard-mapped. Checked with scripted
      inputs (`tests/*.txt`): coins and credits, 1P/2P start, character select, walking, jumping, crouching,
      punches and kicks in a match, and the operator test menu.
- [x] **Xbox controllers** via XInput (`port/pc/pad_xinput.c`), remappable in `controls.ini`; the mapping logic
      (`port/pc/pad_map.c`) is platform independent so the Dreamcast port can reuse it with its own pad reader
- [ ] Sound on Dreamcast: the H8 interpreter is fine on PC (about 5× real time headless) but heavy for an SH-4;
      plan a high-level version of the sound driver, using this emulation as the reference
- [ ] Identify Sony library functions (signature matching against PsyQ 4.x `.LIB`s)
- [ ] Decompilation into readable C
- [ ] Dreamcast backend

## What the runtime learned about System 12 (beyond `docs/HARDWARE.md`)

| Address | What | How we found it |
|---|---|---|
| `0x1F000000` | Banked ROM select: 2 MB bank number (`file offset >> 21`) | write 8, then reads matching `TIM/FONTX.TIM` |
| `0x1FA00000–0x1FBFFFFF` | Banked ROM window (2 MB) | same |
| `0x1F018000` | Watchdog (game writes 0 while it waits) | boot wait loops |
| `0x1F080000–0x1F083FFF` | RAM shared with the H8 sound/I/O CPU (H8 side `0x080000`, 64 KB) | boot handshake `0x3163` → `0x7601` at `+0x3002` |
| `0x1F083140`, `0x1F083142` | Read by the vblank callback: probably player inputs | `func_801F0BB0` |
| `0x1F140000–0x1F140FFF` | 2 KB EEPROM (AT28C16), one byte per 16-bit word | write-then-poll loop |
| VRAM | 1024 × 1024 (2 MB); texture-page bit 11 selects the lower 512 lines | garbage textures until enabled |
| `0x1F083030–0x1F083037` | RTC-4543 clock as seen by the main CPU: BCD year, month/day, weekday/hour, minute/second | operator A.D.S. "NOW TIME" and the release clock |
| EEPROM `0x18–0x1F` | Operator settings: `0x18` start cost, `0x19` chutes, `0x1A` credit mode / coin counter / free play (bits 0-2), `0x1C` difficulty / damage / timer, `0x1D` fight counts and change-at-continue, `0x1E` change in VS / attract music / event mode | one setting at a time in the operator menu, EEPROM compared |
| RAM `0x803552B0`, `0x80368CB4` | The game's copies of EEPROM `0x18–0x1B` and `0x1C–0x1F` (free play = bit 18 of the first word, `func_801F1474`) | RAM diff of two boots differing only in free play |
| EEPROM `0x2A0` / RAM `0x80318230` | Date the board was first switched on; characters unlock 360, 696, 1032 hours later | `func_802B3F98` |

The game uses kernel threads (`OpenTh`/`ChangeTh`), a vblank event callback, and libetc's
`HookEntryInt`/`setjmp` interrupt handler. All are emulated in `port/core/bios.c`.

## Sound / I-O board (H8/3002)

| H8 address | What |
|---|---|
| `0x000000–0x07FFFF` | Program ROM `ftl1vera.11s` (dump is word-swapped; vectors: reset `0x100`, IRQ1 `0x404A`) |
| `0x080000–0x08FFFF` | Shared RAM (16-bit wide; byte `a` on the H8 is byte `a ^ 1` on the main CPU) |
| `0x280000–0x287FFF` | C352 registers (32 voices × 8 words, key-on/off trigger at word `0x202`) |
| `0x300000–0x300003` | Inputs, active low: P2, P1, system/coins, extra buttons |
| port 7 | DIP switches (bit 7 = service mode) |
| port B bit 7, IRQ1 | vblank |

The sequencer ticks from ITU timer overflow interrupts (OVI0/OVI1 reload `TCNT`), so tempo follows the
emulated H8 clock (16.9344 MHz). The C352 runs at 25.4016 MHz / 288 = 88.2 kHz and only the front pair is
wired to the speakers. IRQ1 is edge-triggered and the handler never clears its flag, so the request must
clear when the interrupt is accepted. The earlier guessed handshake replies have been removed: the real
program answers the main CPU now.
