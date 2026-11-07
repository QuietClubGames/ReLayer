# Fighting Layer character models: LMD, skeleton, motions, palettes

Reverse-engineered from the recompiled main program (addresses are MIPS addresses of the original code,
`func_XXXXXXXX` in `asm/main/0.s`, `f_XXXXXXXX` in `build/recomp/`). Checked on Kato (`TMD/P00_.LMD`,
`MOTDAT/PL00_MOT.DAT`). As a test, an offline renderer built only from this spec drew Kato's stance (motion 0, key 0)
correctly: all 19 parts, the skin and the face texture side. Every LMD in `assets/TMD` and `assets/FLASHROM` parses
with these rules.
Confidence: **sure** = read directly from the code and checked against data; *probable* = inferred, not proven.

Numbering: **E0…E21** = index in the LMD offset table. Earlier notes counted from the 44-byte block's *successor*
("entries 0-18 body parts, 19/20 data blocks"); that numbering is E*n*+1 here (old 19 = E20, old 20 = E21).

---

## 1. LMD file

### 1.1 Header (sure)

```
u32 entry_offset[N]      // in 4-byte words from the file start
```

`entry_offset[0]` points just past the table, so it equals N. That is why the first word looked like a count. The game
never reads the count from the file. It uses a per-character table instead (`D_802E7478[char]`, see 1.5).
Kato: N = 22, E0 at 0x58.

### 1.2 The "44-byte block" = E0, a dummy part (sure)

E0 is an ordinary part (1.3) with one untextured flat triangle:

```
01 00 20 00            group: count 1, type 0x20 (flat triangle)
ff 00 00 20            colour word: RGB = (255,0,0), GPU code 0x20
ff ff 00 00 fe ff ff ff  v0 = (-1, 0,-2)
ff ff 00 00 02 00 ff ff  v1 = (-1, 0, 2)
02 00 00 00 00 00 ff ff  v2 = ( 2, 0, 0)
00 00 00 f0 00 00 00 00  normal (0,-4096,0)
ff ff ff ff            end of part
```

It is a tiny, practically invisible red triangle. It is attached to **node 1** (the abdomen pivot, see 3.2). Any valid part
can go there, so a converted model may put a real mesh in E0. EX2's 52-byte block is the same kind of thing: one
0x28 flat quad.

### 1.3 Body part layout (sure)

A part is a list of polygon groups, then the terminator `0xFFFFFFFF`:

```
{ s16 count; s16 type; record[count]; } ...   0xFFFFFFFF
```

The **type is the PS1 GPU primitive code**. All readers check the types **once each, in this fixed order**:
0x20, 0x28, 0x30, 0x38, 0x24, 0x2C, 0x34, 0x3C. The readers are the preprocessor `func_8029E7E8`, the packet builder
`func_8029CE60`, the P2 patcher `func_8029BC8C`, and the renderers `func_80207E94` and `func_80208CA0`. After
preprocessing, the internal types 0x44 and 0x4C may follow. Each type can appear at most once per part, and only in
that order. A group out of order, or of an unknown type, silently ends the part: later groups are neither built nor
drawn. There are no shared vertex lists. Every polygon carries its own vertices and normals, so nothing references a
vertex by index.

`SVECTOR` = `s16 x, y, z, pad` (8 bytes). Vertices are in the owning node's local space, in model units (§3).
Normals are unit vectors scaled to 4096 (checked on every polygon of every LMD). The `pad` of vertices and normals
(the "flag": -1 on most parts, 0 on Kato's hand variants E6/E7/E11/E12) is **never read**. The vertex is loaded with
`lwc2` into VXY/VZ, which keeps only the low 16 bits of VZ. Write anything there; 0 is fine.

| type | primitive | record bytes | layout |
|---|---|---|---|
| 0x20 | flat triangle (F3) | 36 | `u32 rgbcode` (0x20BBGGRR), `SVECTOR v[3]` @4, `SVECTOR n` @0x1C |
| 0x28 | flat quad (F4) | 44 | `u32 rgbcode`, `v[4]` @4, `n` @0x24 |
| 0x30 | gouraud triangle (G3) | 60 | `u32 rgb0code, rgb1, rgb2`, `v[3]` @0xC, `n[3]` @0x24 |
| 0x38 | gouraud quad (G4) | 80 | `u32 rgb0code, rgb1, rgb2, rgb3`, `v[4]` @0x10, `n[4]` @0x30 |
| 0x24 | textured flat triangle (FT3) | 44 | `u16 tpage, clut, uv0, uv1, uv2, pad`, `v[3]` @0xC, `n` @0x24 |
| 0x2C | textured flat quad (FT4) | 52 | `u16 tpage, clut, uv0, uv1, uv2, uv3`, `v[4]` @0xC, `n` @0x2C |
| 0x34 | textured gouraud triangle (GT3) | 60 | `u16 tpage, clut, uv0, uv1, uv2, pad`, `v[3]` @0xC, `n[3]` @0x24 |
| 0x3C | textured gouraud quad (GT4) | 76 | `u16 tpage, clut, uv0..uv3`, `v[4]` @0xC, `n[4]` @0x2C |

`uv` = `u8 u, v`. The F/G colours are lit by the GTE (`nccs`/`ncct`). Textured polygons are lit from a fixed base
colour of 0x808080 (`func_80207E94`, e.g. 0x8020840C). Quads use the PS1 vertex order (triangles v0-v1-v2 and
v1-v3-v2). Polygons are single-sided: a polygon is drawn only if NCLIP(v0,v1,v2) > 0, which means **clockwise on screen**
(screen y points down).

**`tpage`** (sure): the raw GPU texture-page word, copied into the packet unchanged (`func_8029E388` is
`return a0`). Bits 0-3 = X/64, bit 4 = Y/256, bits 5-6 = semi-transparency mode, bits 7-8 = colour depth. Values seen:

- `TMD/` models (4-bit textures): 0x28 = page (512,0), 0x29 = page (576,0)
- `FLASHROM/` models (8-bit textures): 0xA8/0xA9 = (512,0)/(576,0), 0xAC/0xAD = (768,0)/(832,0), 0xB8/0xB9 at y=256

Abr = 1 is always set. It only matters for texels with the STP bit.

**`clut`**: the raw GPU CLUT word for **player 1**: `(y<<6) | (x>>4)`. Kato uses 0x7800-0x7808, so row 480 at
x = 16·k, and a 4-bit polygon picks one of 16 palettes of 16 colours. 8-bit FLASHROM models use 0x7800 (one
256-colour palette). See §2.3 for player 2.

**Metal (environment-mapped) polygons** (sure): GT3/GT4 records whose `tpage` is **0x20 or 0xA0**. The code checks
exactly these two values; X = 0 is otherwise unused. For these polygons `clut` holds a metal-texture index (0 or 1),
not a CLUT. `func_8029E7E8` moves them out of the 0x34/0x3C groups into new groups:

- type **0x44**: 52-byte records = the first 4 bytes of the record + v[3] + n[3] (the UVs are dropped)
- type **0x4C**: 68-byte records, the same for quads

It writes them in place after the 0x3C group and ends the part with a `(0,0)` group. They are drawn as FT3/FT4 on the
8-bit page (512,256) with CLUT row 484+index. V is fixed at load time and U is computed every frame from the
transformed normal. TMD Kato has none; TMD George has 14.

### 1.4 Hand variants (sure)

`func_80216EE4(player, a1, a2)` reads the LMD offset table by **hard-coded entry numbers** to swap hands:

- node 7 (right hand) gets entry `a2+4`
- node 11 (left hand) gets entry `a1+9`

for a value of 1-3. A value of 0 means no change; a value ≥ 4 or negative is used directly as the entry number. When
mirrored (`player+0x1C8` bit 0) the two values are swapped. The values come from each motion key (§3.4, byte +2).
Rendered: **E5/E10 = fist (1), E6/E11 = half-open (2), E7/E12 = open flat hand (3)**.

### 1.5 E20 and E21: skin (joint) data (sure)

The game uses the per-character count `n = D_802E7478[char]`:

| char | n |
|---|---|
| most characters | 20 |
| 0A, 0C, 12, 14 | 22 |
| 0E | 17 |
| 0F | 8 |
| 10 | 14 |
| 17 | 21 |

The model must have **E0…E(n-1) = parts**, **E(n) = skin UV list**, and **E(n+1) = skin geometry**
(`func_8029C358` at 0x8029CAF8: `player+0x1B6C = E(n+1)`, `func_802B5B10(player, E(n), packet_ptr)`).

**E(n+1), skin geometry** (`func_802B5B10` at load; `func_802B6088` / `func_802B639C` every frame) is a list of joints
ending with the u32 −1:

```
joint {
  { s16 node; s16 count; { SVECTOR v; SVECTOR n; }[count]; }  section A
  { s16 node; s16 count; { SVECTOR v; SVECTOR n; }[count]; }  section B
  u32 tri_count;
  { u32 p0, p1, p2; }[tri_count]
}
```

- Section A's vertices are in node A's local space and section B's in node B's. Each section is transformed with its
  own node matrix (`func_802B6564`: `rtps` + `nccs`).
- The results go to the scratchpad at 0x1F800200 + 16·i, section A first, then section B.
- Each triangle stores **absolute scratchpad addresses** `0x1F800200 + 16·i`, not indices.
- At most 32 vertices fit per joint (scratchpad limit); Kato uses at most 16.
- Triangles are culled like parts (NCLIP > 0) and sorted by (sz0+sz2)>>4.

Skin vertices duplicate the border vertices of the two parts they join. This is why another character's skin leaves gaps.

Kato's 14 joints (node A, node B): (2,3) neck, (2,5) R shoulder, (5,6) R elbow, (6,7) R wrist, (2,9) L shoulder,
(9,10) L elbow, (10,11) L wrist, (2,12) waist, (13,12) R hip, (16,12) L hip, (13,14) R knee, (14,15) R ankle,
(16,17) L knee, (17,18) L ankle. 282 triangles.

**E(n), skin UV list**: one 10-byte record per skin triangle, used in order across all joints:
`u16 tpage, clut, uv0, uv1, uv2`. The size is padded to a multiple of 4 (Kato: 282·10 + 4).

Each skin triangle becomes two GT3 packets (semi-transparency enabled, base colour 0x808080). If bit *j* of
`player+0x1764` is set, joint *j* is metal instead: FT3 on page (512,256), `clut` = metal index. The bitmask is
0x1791 for char 05 and 0x2881 for char 0D (0x8029CAD0); 0 otherwise.

---

## 2. Load-time processing (`func_8029C358(player, 0, 0)`, called twice from `func_802B4D68` at 0x802B4E0C/0x802B4E1C)

### 2.1 Where the files go (sure)

`func_802B46EC(side)` loads a character's files.

- **Per-character file table** `D_802EA410[char]`, 24 bytes: MOT, LMD, EX-LMD (or 0), colour PAC [colour 0],
  colour PAC [colour 1], T.PAC.
- **Load addresses**:
  - The MOT goes to 0x800BE000 (P1) / 0x8013E000 (P2). Then `player+0x1B5C` = MOT base.
  - The LMD follows immediately, 4-byte aligned. Then `+0x1B60` = LMD.
  - The EX-LMD follows the LMD. Then `+0x1B64` = EX-LMD.
- **Size limit**: everything above must end before the HIT file at 0x8011A000 / 0x8019A000. That leaves 0x5C000 bytes
  for MOT + LMD + EX-LMD (Kato uses 0x44D50).
- **Exceptions**:
  - Char 0C with colour 2 loads `TMD/P0CX_.LMD` and `PL/P0CX_0.PAC`.
  - Char 16 picks one of four colour PACs, `FLASHROM/P16_0..3.PAC`.

### 2.2 What `func_8029C358` builds (sure)

1. **Packet buffer.** Per side: 0x8011E000 (P1), 0x8019E000 (P2), chosen by `player+0x64 & 1`.
   - The buffer starts with n u32 pointers, then the packets. The budget is 0x1E000 bytes, up to `player+0x1B68`
     (0x8013C000 / 0x801BC000).
   - For each part *i* < n: `func_8029E7E8` reorders metal polygons (§1.3), then `func_8029CE60` builds **two copies**
     (double-buffered) of a GPU packet per polygon.
   - Packet types per polygon: F3 0x14, F4 0x18, G3 0x1C, G4 0x24, FT3 0x20, FT4 0x28, GT3 0x28, GT4 0x34, metal tri
     FT3 0x20, metal quad FT4 0x28.
   - The packets take UV, CLUT, tpage and the 0x80 base colour from the record. Vertices stay in the LMD: the renderer
     reads them from the LMD every frame and writes only the screen XY and the lit colour into the packets.
   - Totals for existing models: 0x8248 … 0x19000 bytes including skin.
2. **EX-LMD parts.** Up to 12 attachment parts, built by a per-character switch (`jtbl_802D827C`). Pairs
   (source, packets) go in `player+0x16CC + 8k`.
3. **Skin.** `player+0x1B6C` = E(n+1); `func_802B5B10` builds the skin packets. `+0x1B70` / `+0x1B74` = start / end of
   the skin packets.
4. **P2 patch.** On side 1 only, `func_8029BC8C` runs on every part (and every EX part); see §2.3.
5. **Skeleton.** The skeleton is built from main-program tables (§3).
6. **Final steps.** Then `func_802BD94C` and `func_8029F51C` run (not model-related).

### 2.3 Palettes, colour choice and player side (sure, except where noted)

- **The colour choice never touches the model.** `player+3` (colour 0/1) only selects which PAC is loaded:
  `Pxx_0.PAC` or `Pxx_1.PAC` (`D_802EA410[char] + 0xC + 4·colour`). Both contain the same two TIMs: images at
  (512,0) and (576,0); CLUT at (0,480).
- **Upload.** `func_80222844(tim, 0x40 + side)` (case 0x40/0x41) reshapes the CLUT rectangle from 16×16 to
  **256×1** and uploads it to row 480. It then uploads a **second copy one row lower**, row 481.
  - For side 1 (id 0x41), the image goes 128 px to the right (640/704) and the CLUT to rows 482 and 483.
  - `func_8029F06C` later copies 480→481 and 482→483 and saves rows 480-487 to 0x801BF000. *Probably* the +1 rows
    are a clean backup that palette effects restore from. The model always draws through row 480 (P1) / 482 (P2).
- **P2 model patch** (`func_8029BC8C`). On every textured packet it adds:
  - `tpage += 2`, i.e. +128 px in X (4-bit pages 8/9 → 10/11);
  - `clut += 0x80`, i.e. 2 rows down.
  - On metal packets: `clut += 0x80` and V += 0x40.
  - `func_802B5B10` does the same for the skin.
  - So **write the LMD for player 1 only**: pages at X = 512/576 and CLUT row 480.
- **Metal textures.** `D_802EA668[char][colour&1]` lists two `METAL/*.TIM` per colour. They are uploaded by
  `func_80222DF4` to (512, 384 + 64·side + 32·i), 8-bit, with CLUT row 484 + 2·side + i.
- **`PxxT.PAC`** holds the effect textures. Its upload ids are 0x10/0x12/0x24 (+side) and its CLUTs go to (256,480…).
  The body does not use it.

---

## 3. Skeleton

### 3.1 Where it comes from (sure)

**Not from the LMD, MOT.DAT or overlay.** It comes from a main-program table, `D_802E7CEC[char]`, 12 bytes per
character: `u32 offsets; u32 hierarchy; u8 node_count`. `func_8029C358` (0x8029C438-0x8029CDF8) reads it into the
node array at `player+0x1F4 + i·0xE8` (23 slots).

- **offsets**: `s16 x, y, z` per node, the translation from the parent. It is scaled at load: `v·15 >> 7` (×15/128).
  The scaled value is stored as the **integer half of 16.16 values** at node+0xA2/0xA6/0xAA, so the s32 values are at
  +0xA0/+0xA4/+0xA8. Node 0's translation is overwritten every frame by the motion's root position (§3.4).
- **hierarchy**: 8 bytes per node: `u8 child_count; u8 child[...]`. It is turned into first-child (node+0) and
  next-sibling (node+4) pointers.
- **`D_802E7E0C`**: node → LMD entry, 0xFF = no mesh. Then node+0xD0 = the source part (0 if its first word is −1) and
  node+0xD4 = its packets. Characters 0E, 0F and 10 use `D_802E7E2C`, `D_802E7E4C` and `D_802E7E6C`.
- **Scale**: node+0x88..0x90 are set to 0x1000; scaling only applies if node+0xAE ≠ 0.

| table (offsets/hierarchy) | nodes | characters |
|---|---|---|
| 0x802E7490 / 0x802E7504 | 19 | 00, 02, 06, 07, 08, 17 |
| 0x802E759C / 0x802E7610 | 19 | 01, 05, 0A, 0B, 0C, 0D, 11-15 |
| 0x802E76A8 / 0x802E771C | 19 | 03, 04, 16 |
| 0x802E77B4 / 0x802E7828 | 19 | 09 |
| 0x802E79E8 / 0x802E7A60 | 20 | 0E (non-humanoid) |
| 0x802E7B00 / 0x802E7B38 | 9 | 0F |
| 0x802E7B80 / 0x802E7BDC | 15 | 10 |

The four humanoid tables differ only in nodes 2-11 (torso and arms). Pelvis and legs are identical in all of them.

### 3.2 Humanoid hierarchy and Kato's offsets (sure; left/right: see §4)

Offsets are given raw and as ×15/128 model units.

| node | parent | children | raw offset (Kato) | ≈ units | mesh entry | name |
|---|---|---|---|---|---|---|
| 0 | – | 1, 12 | (0,−286,0) → replaced by motion root | | – | root |
| 1 | 0 | 2 | 0 | 0 | E0 (dummy) | abdomen pivot |
| 2 | 1 | 3, 4, 8 | (384,0,0) | 45 | E1 | chest / torso |
| 3 | 2 | – | (768,0,0) | 90 | E2 | head |
| 4 | 2 | 5 | (435,0,179) | (50,0,20) | – | R clavicle |
| 5 | 4 | 6 | (−384,0,0) | −45 | E3 | R upper arm |
| 6 | 5 | 7 | (742,0,0) | 86 | E4 | R forearm |
| 7 | 6 | – | (691,0,0) | 81 | E5 (E6/E7) | R hand |
| 8 | 2 | 9 | (435,0,−179) | | – | L clavicle |
| 9 | 8 | 10 | (384,0,0) | | E8 | L upper arm |
| 10 | 9 | 11 | (742,0,0) | | E9 | L forearm |
| 11 | 10 | – | (691,0,0) | | E10 (E11/E12) | L hand |
| 12 | 0 | 13, 16 | 0 | 0 | E13 | pelvis / hips |
| 13 | 12 | 14 | (384,0,256) | (45,0,30) | E14 | R thigh |
| 14 | 13 | 15 | (1177,0,0) | 137 | E15 | R shin |
| 15 | 14 | – | (1272,51,0) | (149,5,0) | E16 | R foot |
| 16 | 12 | 17 | (384,0,−256) | | E17 | L thigh |
| 17 | 16 | 18 | (1177,0,0) | | E18 | L shin |
| 18 | 17 | – | (1272,51,0) | | E19 | L foot |

Each limb's mesh extends along its node's +X axis by the length of the next bone. Examples: the thigh E14 spans x
29…144, the shin E15 x 0…163, the upper arm E3 x −16…91. Mesh-less nodes 0, 4 and 8 still have motion tracks.

### 3.3 Transform (sure)

`func_8020624C` → `func_8020654C` (recursive over child/sibling):
`local = [RotMatrix(node+0x98) | translation(node+0xA2,+0xA6,+0xAA)]`, `node+8 = parent+8 × local` (GTE
`CompMatrix`). Two more matrices are built the same way: +0x28 and +0x48, plus the light matrix +0x68.

`RotMatrix` (`func_802D065C`) builds **R = Rz·Ry·Rx** (rotate about X first). Angles are 4096 = 360°. Coordinates are
PS1 style: y points down, the ground is at y = 0, and the model faces −Z in its rest frame. The draw loop
(`func_802069CC`) sets node+8 as the GTE rotation/translation and node+0x68 as the light matrix, then draws node+0xD0
with node+0xD4. Node+0x98 is the final rotation: `func_80215338` blends previous (+0xB0) and current (+0xB8) by the
weight `player+0x1D0`/100, then `func_80216FF0` adds per-node offsets.

---

## 4. Part index → body part, and how it was determined

| E | old index | part | node |
|---|---|---|---|
| 0 | (44-byte block) | dummy (abdomen) | 1 |
| 1 | 0 | chest / torso | 2 |
| 2 | 1 | head | 3 |
| 3 | 2 | R upper arm | 5 |
| 4 | 3 | R forearm | 6 |
| 5 | 4 | R fist | 7 |
| 6 | 5 | R half-open hand | 7 |
| 7 | 6 | R open hand | 7 |
| 8 | 7 | L upper arm | 9 |
| 9 | 8 | L forearm | 10 |
| 10 | 9 | L fist | 11 |
| 11 | 10 | L half-open hand | 11 |
| 12 | 11 | L open hand | 11 |
| 13 | 12 | pelvis / hips | 12 |
| 14 | 13 | R thigh | 13 |
| 15 | 14 | R shin | 14 |
| 16 | 15 | R foot | 15 |
| 17 | 16 | L thigh | 16 |
| 18 | 17 | L shin | 17 |
| 19 | 18 | L foot | 18 |
| 20 | 19 | skin UV list | – |
| 21 | 20 | skin geometry | – |

How each piece was determined:

- **Node → entry**: from `D_802E7E0C` (sure).
- **Hierarchy**: from the tables (sure).
- **Hand poses**: by rendering E5-E7.
- **Joint names**: from the skin joint pairs (§1.5) and bone lengths matching the mesh extents.
- **Left/right**: Kato was rendered offline in motion 0, key 0, using this spec, with a camera looking down +Z.
  - The face is visible, so he faces the camera.
  - The node-5…7 arm and the node-13…15 leg are on the viewer's left, so they are his anatomical **right**.
  - *Probable*, for the authored (unmirrored) data.
  - When `player+0x1C8` bit 0 is set, `func_80216FF0` mirrors the pose. It swaps tracks through `D_802DCA30` =
    {0,1,2,3, 8,9,10,11, 4,5,6,7, 12, 16,17,18, 13,14,15} (arms 4-7↔8-11, legs 13-15↔16-18) and negates angles. So
    on screen the sides can appear swapped.

---

## 5. MOT.DAT

### 5.1 Banks (sure)

`player+0x1CA` is a u16 = `bank<<8 | motion`. The bank tables are `D_802DCAA4` (P1) and `D_802DCAB8` (P2),
selected through `D_802DCACC[side]`:

- banks 0-2 come from `PL/COMMON.PAC` (`func_802144B0`);
- bank 3 is the character's own MOT.DAT (`func_80214520` / `func_80214534`).

When `player+0x1C9 & 0x88` (the player is being thrown), the **opponent's** banks are used. So throw reactions and
COMMON motions drive every character's skeleton: a new character must keep the 19-node humanoid layout.

### 5.2 Layout (sure)

```
u32 motion_offset[]          // byte offsets from file start, indexed by motion id (Kato: 65, some unused/duplicated)
motion:
  u16 last_key;              // number of keys − 1 (→ player+0x1CC)
  u16 unknown;               // 0 in normal motions
  u32 flags;                 // bit0: pose has an extra 6/3-byte block (below); bit1: blend into next;
                             // bits2-3: root-delta shift (→ player+0x1C9 bits 5-6)
  key[]: { s16 duration;     // game frames (all 1 in FL: baked at 60 fps)
           u8  hands;        // low nibble → left hand (node 11), high nibble → right hand (node 7), 0 = keep
           u8  unknown;      // usually 0
           u32 pose_offset; }// byte offset from file start, or a control code:
     0x08000000 = end (hold the last pose), 0x08000001 = loop this motion,
     0x08000002 = chain: the next 8-byte entry's s16 at +4 = (bank<<8|motion) to play next
```

### 5.3 Pose (sure; `func_80215E98` = set the key, `func_80216234` = interpolate towards it)

- **Key 0** (`player+0x1CE == −1`) gives the absolute root position as **big-endian** `s16 x, y, z`. If flags bit0 is
  set, 6 more bytes come first (*unknown* vector, e.g. (0,0,−19)).
- **Later keys** give the root delta as 3 signed bytes, with 3 more bytes first if flags bit0 is set.
  - `func_80216234` spreads the delta over the key duration, shifted left by (flags>>2)&3.
  - `func_80215E98` adds it unshifted.
- The root position goes to `player+0x1744/48/4C` (16.16) and becomes **node 0's translation** (blended at
  0x80215B00). Kato's stance root is (−6,−313,−9): the pelvis stands 313 units above the floor.
- Then come **`D_802DCA74[char]` tracks** (19 for humanoids; 20/9/15 for 0E/0F/10), **track i = node i**, 4 bytes
  each:

```
b0 = xxxyyyzz, b1, b2, b3
rx = ((b0>>5)<<8 | b1) << 1     // 11 bits, step 2
ry = (((b0>>2)&7)<<8 | b2) << 1 // 11 bits, step 2
rz = ((b0&3)<<8 | b3) << 2      // 10 bits, step 4      (4096 = 360°, R = Rz·Ry·Rx)
```

These are stored at node+0xB8/BA/BC. Interpolation moves each angle towards the target by (target−current)/frames
left, taking the shorter of the two Euler equivalents (rx+2048, 2048−ry, rz+2048). Key sizes are 6+4·19 = 82 bytes
for key 0 and 3+76 = 79 bytes for later keys.

---

## 6. Conversion checklist (EX2 → FL)

- **Parts.** Polygon groups in the fixed type order with GPU-code types, records as in §1.3, terminator −1.
  EX2 parts already follow these rules (checked on Hayate `P04X.PAC` [2]: same order, same record sizes, same hand
  variants at E5-7/E10-12). EX2's joint data (`PxxX.PAC` [3]/[4]) uses a different, offset-table layout that is not
  decoded yet.
- **Entries.** E0…E19 at the indices above, then the skin UV list (E20) and the skin geometry (E21) built for *these*
  meshes, in node-local space.
- **Textures.** Write the LMD for player 1: tpage 0x28/0x29 (or the 8-bit 0xA8… values), CLUT row 480.
- **Packet budget.** At most 0x1E000 bytes of packets (2× packet size per polygon, plus 0x50 per skin triangle, plus
  4·n).
- **Bone lengths.** These come from `D_802E7CEC[char]` in the main program. To get different proportions, patch that
  table (or the node+0xA2… values after load), or author the meshes to fit the target slot's table.
- **Skeleton layout.** Motions must have 19 tracks in the node order of §3.2, with rest frames compatible with FL's,
  because COMMON.PAC and opponent throw motions will drive the model.
