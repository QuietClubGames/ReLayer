# Fighting Layer — what's in the ROM

These are findings from the MAME merged set `fgtlayer.zip` (World "Ver. A" program `ftl3vera`, plus the Japan `ftl0vera` clone).
Everything here was checked against the actual ROM bytes with the scripts in `tools/`.

## Platform: Namco System 12

| Part | Chip | Notes |
|---|---|---|
| Main CPU | Sony CXD8661R (PS1 R3000A + GTE) | 4 MB main RAM, 1 KB scratchpad at `0x1F800000` |
| GPU | Sony CXD8654Q (PS1 GPU) | 2 MB VRAM (a PS1 has 1 MB) |
| Sound CPU | Hitachi H8/3002 | Runs its own program (`ftl1vera.11s`) |
| Sound chip | Namco C352 | 32-voice PCM; 16 MB of samples (`wave0`/`wave1`) |

The game is a **PsyQ-SDK PlayStation program**. It uses Sony's libraries (`Library Programs (c) 1993-1997 SCEI`,
`libgpu sys.c v1.135 1997/09/02`, `libetc intr.c v1.76 1997/02/12`), so this is roughly the PsyQ 4.1/4.2 era.
Both the CPU and the Dreamcast's SH-4 are little-endian, and so is x86, so data needs no byte swapping.

## ROMs

| File(s) | Size | Contents |
|---|---|---|
| `ftl3vera.2e` + `.2j` (16-bit interleave, `.2e` = even byte) | 4 MB | Program ROM, mapped at `0x1FC00000` |
| `ftl1rom0/1/2` | 3 × 8 MB | Banked mask ROM, offsets `0x0000000–0x17FFFFF` |
| `ftl1fl3l/h`, `ftl1fl4l/h` (interleaved pairs) | 2 × 4 MB | Banked flash, offsets `0x1800000–0x1FFFFFF` |
| `ftl1vera.11s` | 512 KB | H8/3002 sound program |
| `ftl1wave0/1` | 2 × 8 MB | C352 sample ROM |

## Program ROM map (`baserom/prg.bin`)

| ROM offset | Runs at | What it is |
|---|---|---|
| `0x000000–0x01FFFF` | `0xBFC00000` | Sony boot kernel ("PS-X Realtime Kernel 2.5", "COH-100", "COH-OS.3.0") |
| `0x020000–0x023E5F` | in ROM | Namco System 12 loader / test module ("System12 Hot Start!") |
| `0x024000` | — | Game image descriptor: `src=0x1FC28000 dst=0x801F0000 size=0x140000` |
| `0x028000–0x143957` | **`0x801F0000`** | **Main game executable** → `baserom/main.bin` |
| `0x180000–0x18329F` | — | **File table**: 405 × 32-byte entries `{char name[24]; u32 offset; u32 size;}` |
| `0x184000–0x36DB2F` | — | `OVL/`, `OVL2/`, `HIT/` files (file-table offsets with bit 31 set point here) |

### Main executable (`main.bin`)

- Entry `0x801F0000` (`j 0x801F0010`), standard PsyQ crt0: clears BSS, sets `gp = 0x8030B958`,
  calls `InitHeap` (`0x802C508C`), then `main` (`0x801F0424`).
- `.text` `0x801F0000–0x802D5C83` (≈ 940 KB, **1,695 functions** as found by spimdisasm; 1,716 after the recompiler splits merged ones). Sony library code sits at the top,
  from about `0x802C0000`.
- `.rodata`/`.data` `0x802D5C84–0x8030B957`.
- `.bss` `0x8030B958–0x8036D4A7` (`0x61B50` bytes).

### Character overlays

Each of the 21 characters has a code overlay loaded at runtime:

- `OVL/PLxx_1.OVL` → slot 1 (player 1) at **`0x8009E000`**
- `OVL2/PLxx_2.OVL` → slot 2 (player 2) at **`0x800AE000`**

`_1` and `_2` are **the same code linked at two addresses**: every differing word is a relocation
(pointer +0x10000, `lui` +1, `jal` +0x4000). So only the 21 `_1` overlays need decompiling
(≈ 885 KB, **about 1,195 functions** on the first pass). Each overlay begins with a table of pointers,
then character data (command/move scripts), then code, then more data.

Character IDs present: `00–11`, `16`, `17`, `18` (hex). `12–15` are missing from the set.

### File table / assets

The file table indexes everything the game loads: stages (`STG`, `STG2`), models (`TMD`), PS1 textures (`TIM`),
motion data (`MOTDAT`), per-character packs (`PL`), demo/attract (`DEMO`), ending (`END`), select screen
(`SEL2`), LED cabinet programs (`LED`, `FLASHROM/LEDPROG.*`) and hit data (`HIT`).
Offsets without bit 31 index the 32 MB banked ROM. All 114 `.TIM` entries start with the TIM magic `0x10`,
which confirms the mask-then-flash bank order.

`tools/extract_rom.py` writes every entry to `assets/<DIR>/<FILE>`.
