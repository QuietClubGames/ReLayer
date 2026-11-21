# ReLayer release notes

## 0.1.3

New: **Uncensored**, the original Japanese version of the game. Install it over 0.1.2 (settings are kept).

Fighting Layer came out in Japan first. The World version, the ROM most people have, is the same program set
to a different region, and with a hidden arcade setting called Regulation switched on. Together they tone the
game down. ReLayer now plays the World ROM as the Japanese machine does, so you get the uncut game without
needing the Japanese ROM:

| | World version | Uncensored (Japanese version) |
|---|---|---|
| Sessyu | a stick | his sword |
| Janis | sticks | her knives |
| Hit sparks | green | red |
| Fire and light effects | purple | their normal colours |
| Winning with a super move | nothing | the "SUPER FINISH!" banner |
| Blair | a covered-up outfit | her original costume |
| Lan, Clemence and Blair | stiff | chest physics |
| Moves Regulation switches off (Preston Ajax's mines, Joe's "Evil Judge" super, some of Vold's specials, as documented by The Cutting Room Floor) | off | on |
| Tetsuo Kato | called "Kato" | called "Tetsuo" |

- It is **on by default**. To play the World version, open the menu, go to the **Game** page and switch
  **Uncensored** off. It takes effect at once; restart the game (System page) to see the start-up screens change.
- The game stays in **English**: the Japanese version has the same English text and the same voices.
- With it on, the game starts with the Japanese machine's notice ("This game is for use exclusively in Japan")
  and the Namco logo, as the Japanese arcade did. That's normal.
- The arcade's own operator menu shows two extra rows in this mode, HIT COLOR and REGULATION. ReLayer's
  Uncensored option sets both, so changing them there has no effect while it is on.

## 0.1.2

The same game as 0.1.1; only the installer changed. Some antivirus programs, Windows Defender included,
blocked 0.1.1's `Setup.exe` as "Program:Win32/Contebrew.A!ml". That is an automatic guess about new,
unsigned programs, not a virus found in the file.

- `Setup.exe` and `ReLayer.exe` now carry the usual Windows version details (publisher, product name,
  version; right-click → Properties → Details), which makes them look less like unknown programs to
  antivirus checks.
- If Windows Defender still blocks Setup: open **Windows Security → Virus & threat protection → Protection
  history**, choose the entry for `Setup.exe`, then **Actions → Allow on device**, and run Setup again.
  Please don't turn your antivirus off.

## 0.1.1

Fixes for freezes. Updating is recommended: install it over 0.1.0 (settings are kept).

- **Black screen after a fight or two in arcade mode.** The game's stage loader runs as a task that ends
  itself; ReLayer never freed its slot, so after a few stages there was no room to start the next one and the
  game sat on a black screen. Fixed, and tested with full arcade runs to the ending for every fighter.
- **Picture froze when switching to exclusive full screen** (from the menu, Alt+Enter / F11, or Alt+Tab back
  to the game). Fixed.
- **Controls stopped working after using the menu** with a controller that always reports a button as held
  (a resting trigger, some fight sticks, a second device). The game now listens again after half a second.
- If the graphics driver resets or Direct3D fails, ReLayer starts Direct3D again instead of falling back to
  a mode without the menu. PCs without a usable graphics card use Windows' software renderer (original
  graphics only) rather than a mode where the menu could not be shown.
- If the picture ever stops for 3 seconds, `fgtlayer.log` now records what the game was doing, and a restart
  from the menu keeps the previous log as `fgtlayer.old.log`. Please attach both when reporting a freeze.

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

**The menu:** press **Back + Start** together on a controller (View + Menu on newer Xbox pads), or **Tab** /
**Esc** on the keyboard. The game pauses while it is open. Move with the D-pad, stick or arrow keys; choose
with A or Enter; go back with B or Esc; switch pages with LB / RB or Q / E; close with Start or Tab.

**Controls:** insert a coin, then press Start (with free play on, Start alone is enough).

| | Controller | Keyboard, player 1 | Keyboard, player 2 |
|---|---|---|---|
| Move | D-pad or left stick | arrow keys | I J K L |
| Punch light / medium / heavy | X / Y / RB | Z / X / C | Q / W / E |
| Kick light / medium / heavy | A / B / RT | A / S / D | R / T / Y |
| Start | Start | 1 | 2 |
| Insert coin | Back | 5 | 6 |

Alt+Enter or F11: full screen. F9: high-resolution 3D on / off. Buttons can be remapped in the menu's
Controls page.

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
