<div align="center">

# ReLayer

**Fighting Layer, rebuilt for Windows.**<br>
An unofficial PC port of Arika's 1998 arcade fighter.

[![Latest release](https://img.shields.io/github/v/release/QuietClubGames/ReLayer?label=download&style=for-the-badge&color=e8a33d)](../../releases/latest)
[![Downloads](https://img.shields.io/github/downloads/QuietClubGames/ReLayer/total?style=for-the-badge&color=4c8bf5)](../../releases)
![Windows](https://img.shields.io/badge/Windows-10%20%2F%2011-0078D6?style=for-the-badge&logo=windows&logoColor=white)
[![License: MIT](https://img.shields.io/badge/license-MIT-6e7781?style=for-the-badge)](LICENSE)

**[Download](../../releases/latest)** · [Install](#install) · [The menu](#the-menu) · [Controls](#controls) · [FAQ](#faq) · [For developers](docs/DEVELOPING.md)

</div>

> [!IMPORTANT]
> ReLayer does **not** include the game. You need your own legally obtained copy of the arcade ROM, the MAME
> set `fgtlayer.zip` (Fighting Layer, World, Ver. A). Setup builds the game from it on your PC; the download
> contains only ReLayer's own code and the open-source tools it uses.

## What ReLayer adds

| | |
|---|---|
| **Sharper 3D** | Redrawn on your graphics card at up to 8x the arcade's resolution, without the original's wobbling polygons and warping textures. Or keep the original picture (F9 switches). |
| **Your screen, your way** | Window, borderless or exclusive full screen; 4:3, stretched or pixel-perfect scaling; sharp, smooth or scanline filter. |
| **Controllers** | Xbox-style controllers for two players, keyboard, and button remapping. |
| **Arcade settings** | Difficulty, damage, round timer, rounds and free play, all from an in-game menu, plus an option to unlock the three time-release characters from the start. |
| **Native** | Not an emulator: the arcade code is recompiled into a Windows program. |

## Install

1. Download **`ReLayer-Setup.zip`** from the [latest release](../../releases/latest) and unzip it anywhere.
2. Run **`Setup.exe`**.
3. Choose your `fgtlayer.zip` and where to install, then press **Install**. It takes a minute or two and adds
   desktop and Start menu shortcuts. No administrator rights are needed.

> [!NOTE]
> Setup isn't code-signed, so Windows may show **"Windows protected your PC"**. Choose **More info**, then
> **Run anyway**.

To uninstall: **Windows Settings → Apps → ReLayer (Fighting Layer PC port) → Uninstall**.

## The menu

Press **Back + Start** together on a controller (View + Menu on newer Xbox pads), or **Tab** / **Esc** on the
keyboard. The game pauses while the menu is open.

| | Controller | Keyboard |
|---|---|---|
| Move | D-pad or stick | Arrow keys |
| Choose | A | Enter |
| Back | B | Esc |
| Switch pages | LB / RB | Q / E |
| Close | Start | Tab |

| Page | What's there |
|---|---|
| **Display** | Window / borderless / exclusive full screen, window size, monitor, picture shape, enhanced or original graphics, quality, screen filter, frame-rate counter |
| **Audio** | Volume, mute |
| **Controls** | Remap any button, stick dead zone, restore defaults |
| **Game** | Unlock all characters, free play, difficulty, damage, round timer, rounds and the arcade's other settings |
| **System** | Resume, the arcade's operator menu, restart, quit |

## Controls

Insert a coin, then press Start. With free play on, Start alone is enough. The first controller is player 1,
the second is player 2.

| | Controller | Keyboard (player 1) | Keyboard (player 2) |
|---|---|---|---|
| Move | D-pad or left stick | Arrow keys | I J K L |
| Punch: light / medium / heavy | X / Y / RB | Z / X / C | Q / W / E |
| Kick: light / medium / heavy | A / B / RT | A / S / D | R / T / Y |
| Start | Start | 1 | 2 |
| Insert coin | Back | 5 | 6 |
| Menu | Back + Start | Tab or Esc | |

| Key | Does |
|---|---|
| Alt+Enter or F11 | Window / full screen |
| F9 | High-resolution 3D on / off |
| F2 | The arcade's test button (operator menu) |

## FAQ

<details>
<summary><b>Is this an emulator?</b></summary>

No. ReLayer is a *static recompilation*: a tool translates the arcade game's MIPS processor code into C,
which is compiled into a native Windows program, with the arcade hardware (graphics, sound, inputs) replaced
by portable code. This happens on your PC when you install, from your own ROM.
</details>

<details>
<summary><b>Which ROM do I need?</b></summary>

The MAME set `fgtlayer.zip`, Fighting Layer (World, Ver. A). Any MAME version's set works: Setup checks the
twelve ROM chips by checksum, whatever they are named. No BIOS file is needed. Please don't ask where to get
it.
</details>

<details>
<summary><b>My controller doesn't work.</b></summary>

ReLayer recognises Xbox-style (XInput) controllers. PlayStation controllers work through Steam or a tool such
as DS4Windows, and many arcade sticks have an XInput mode. The keyboard always works.
</details>

<details>
<summary><b>Where are my settings saved?</b></summary>

In the install folder: `settings.ini` (menu settings), `controls.ini` (controller buttons) and `eeprom.bin`
(the arcade's own settings and high scores).
</details>

<details>
<summary><b>What do I need to run it?</b></summary>

64-bit Windows (10 or 11 recommended). The high-resolution 3D needs a DirectX 11 graphics card; without one
the game falls back to the original picture.
</details>

<details>
<summary><b>Something went wrong. How do I report it?</b></summary>

Open an [issue](../../issues) and attach `fgtlayer.log` from the install folder (the previous run's is
`fgtlayer.old.log`).
</details>

## About

ReLayer was made with Claude, Anthropic's AI model. Its source code is under the [MIT license](LICENSE),
which covers ReLayer only, not the game. Building it yourself is described in
[docs/DEVELOPING.md](docs/DEVELOPING.md).

<sub>ReLayer is a fan project, not affiliated with or endorsed by Arika or Bandai Namco. Fighting Layer is a
trademark of its owners.</sub>
