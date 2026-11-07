# ReLayer release notes

## 0.1.0 (first public release)

ReLayer is an unofficial PC port of Fighting Layer (Arika / Namco, 1998). It does not include the game:
you need your own legally obtained copy of the arcade ROM, the MAME set `fgtlayer.zip` (Fighting Layer,
World, Ver. A). Setup turns that ROM into a native Windows program on your PC. The download contains only
ReLayer's own code and the open-source tools Setup uses, no game code or data.

**Download:** `ReLayer-Setup.zip`. Unzip it, run `Setup.exe` and choose your `fgtlayer.zip`.

What ReLayer adds when it runs your copy of the game:

- High-resolution 3D drawn on your graphics card (Auto quality, or a fixed 2x-8x), with perspective-correct
  textures, or the original picture (F9).
- Window, borderless and exclusive full screen; 4:3, stretched or whole-number scaling; sharp, smooth or
  scanline filter.
- Xbox-style controllers for two players, keyboard, and button remapping, from an in-game menu
  (Back + Start on a controller, Tab / Esc on the keyboard).
- The arcade's operator settings (difficulty, timer, rounds, free play) in that menu, and an option to make
  the three time-release characters selectable from the start.

Known limits:

- Only Xbox-style (XInput) controllers are recognised. PlayStation controllers work through Steam or a tool
  such as DS4Windows; many arcade sticks have an XInput mode.
- Needs 64-bit Windows (10 or 11 recommended). The high-resolution 3D needs a DirectX 11 graphics card;
  without one the game falls back to the original picture.
- Setup is not code-signed, so Windows may say "Windows protected your PC": choose More info, then
  Run anyway.

Reporting problems: open an issue and attach `fgtlayer.log` from the install folder. Please don't ask where
to get the ROM.

ReLayer is a fan project, not affiliated with or endorsed by Arika or Bandai Namco. Fighting Layer is a
trademark of its owners.
