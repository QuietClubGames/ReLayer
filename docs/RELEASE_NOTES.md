# ReLayer release notes

## 0.1.0 (first public release)

ReLayer is an unofficial PC port of Fighting Layer (Arika / Namco, 1998). Setup builds the game on your PC
from your own MAME `fgtlayer.zip`; the download contains no game code or data.

**Download:** `ReLayer-Setup.zip`. Unzip it, run `Setup.exe`, choose your `fgtlayer.zip`.

What's in it:

- The whole arcade game: arcade and versus modes, every fighter, the operator settings and test menu.
- High-resolution 3D drawn on your graphics card (Auto quality, or a fixed 2x-8x), perspective-correct
  textures, or the original picture (F9).
- Window, borderless and exclusive full screen; 4:3, stretched or whole-number scaling; sharp, smooth or
  scanline filter.
- Xbox-style controllers for two players, keyboard, and button remapping in the menu.
- All characters selectable from the start (switchable in the menu).

Known limits:

- Controllers: only Xbox-style (XInput) controllers are recognised. PlayStation controllers work through
  Steam or a tool such as DS4Windows; many arcade sticks have an XInput mode.
- Needs 64-bit Windows (10 or 11 recommended). The high-resolution 3D needs a DirectX 11 graphics card;
  without one the game falls back to the original picture.
- Setup is not code-signed, so Windows SmartScreen asks for confirmation the first time.

Reporting problems: open an issue and attach `fgtlayer.log` from the install folder.

ReLayer is a fan project, not affiliated with or endorsed by Arika or Bandai Namco.
