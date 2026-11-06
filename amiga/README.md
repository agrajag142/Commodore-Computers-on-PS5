# Amiga 500 for the PS5

The Amiga 500 emulator from the Waveshare ESP32-P4 sketch (`WaveshareP4_C64Amiga_003`: Massitux's
[amiga500-esp32](https://github.com/massitux/amiga500-esp32) with Musashi's 68000, and the board
port's work on the picture, copper, blitter and sound) as a PS5 home-screen app, built like the C64
one. Title ID `PPSA99905`, beside the C64 (`PPSA99904`).

## What you need

No ROM or disk comes with it: use your own copies (Amiga Forever has both).

- **Kickstart 1.3** (256K) as `kick.rom` (or `kick34005.A500`, `kick13.rom`, `amiga-os-130.rom`, or
  any `.rom`). Amiga Forever's encrypted ROMs work with their `rom.key` next to them. 512K
  Kickstarts (2.0 and later) are not supported. Without one the app shows NO KICKSTART.
- **Disks** (`.adf`, 880K). `default.adf` goes in DF0: at start-up, else the first one by name.

Put them in `amiga/assets/amiga` before building (they stay out of git).

## Build and install

As for the C64 (the starter's `tools/setup.sh` first):

```bash
chmod +x amiga/build.sh
cp /path/to/kick.rom /path/to/*.adf amiga/assets/amiga/
PS5_STARTER=~/ps5-homebrew-starter amiga/build.sh             # build/amiga/PPSA99905 and its ZIP
PS5_STARTER=~/ps5-homebrew-starter amiga/build.sh deploy <PS5 IP>   # or build and copy it over FTP
```

Then copy the `PPSA99905` folder into `/data/homebrew` (so that
`/data/homebrew/PPSA99905/sce_sys/param.json` exists): ShadowMountPlus installs it.

**Adding files later, over FTP:** copy them into `/data/homebrew/PPSA99905/assets/amiga`. These
names are found as they are: `kick.rom`, `kick34005.A500`, `kick13.rom`, `amiga-os-130.rom`,
`rom.key`, `default.adf`, `df0.adf`, `df1.adf`, `disk1.adf`, `disk2.adf`. Any other name needs a
line in `files.txt` there (the PS5 won't let the app list the folder).

## Controls

The picture is the Amiga's 640 x 256 at 2 x 4 (1280 x 1024), the full height of the TV.

| DualSense | Amiga |
| --- | --- |
| Left stick, or the touchpad (slide, like a laptop's) | the mouse |
| Square, R1, R2 or a touchpad click | left mouse button |
| Circle, L1 or L2 | right mouse button |
| D-pad | joystick in port 2 |
| Cross | fire |
| Triangle | the MOD player, straight away |
| Options | the menu |

**A USB keyboard** plugged into the PS5 is the Amiga's keyboard, and the mouse of a wireless set on
the same receiver is the Amiga's mouse too (with the stick and the touchpad). The app takes them
from the PS5 while it runs (and gives them back when it quits, or with USB keyboard in the menu set
to off), and looks for them every 2 seconds, so they can be plugged in any time. The keys go by
their place, as on an Amiga keyboard laid out like a US PC one, and:

| PC key | Amiga |
| --- | --- |
| Windows / Menu (or right Windows) | left / right Amiga |
| Alt | Alt |
| F11 | HELP |
| Caps Lock | Caps Lock (it locks) |
| Ctrl + both Amiga keys | the reset |
| F12 | the menu |

In the menu and the MOD player: the arrows, Return (Cross), Esc (Circle), Space (Square), Tab
(Triangle), F12 (Options). The emulator's CIA-A now has its serial port: each key goes to the
Amiga as the real keyboard sends it, and the next one after the Amiga's handshake.

Mouse and joystick work at the same time. **Menu** (Options): back to the Amiga, the MOD player,
reset (with the disk in DF0: still in), the volume, the USB keyboard (on / off), quit to the PS5 home screen (Cross twice), and
the disks: **Cross** restarts the Amiga with that disk in
DF0:, **Square** swaps it in without a restart (for a game's or demo's next disk - the old one comes
out for half a second first, so the Amiga notices). The one in DF0: is orange. The Amiga keeps
running behind the menu. The bottom left shows the disk in DF0: and the frames per second (50 is
full speed).

## MOD player

**MOD player** in the menu (Options) opens the board's ProTracker replayer as a screen of its own:
the Amiga pauses and carries on afterwards. It needs no Kickstart: without one, Options opens it
from the NO KICKSTART screen.

Put modules in `amiga/assets/amiga/mods` before building (`.mod`, `mod.*`, `.nst`, `.stk`, folders
inside are fine, up to 8 MB each): 4-channel ProTracker / Soundtracker modules and 2-8 channel
ones (6CHN, 8CHN, xxCH).

| DualSense | |
| --- | --- |
| D-pad up / down (left / right a page) | choose a folder or module |
| Cross | open the folder / play the module |
| Circle | up a folder (at the top: back to the Amiga) |
| Square | pause |
| Triangle | the visuals (below), the next one, then back to the list |
| Touchpad click | stop |
| L1 / R1 | the song's previous / next position |
| L2 / R2 | the previous / next module in the folder |
| Options | back to the Amiga |

The screen shows the module's title, format, channels and samples, the position, pattern, row,
speed, tempo and time, and for each channel a scope, its level, note and sample. When a song comes
back round to its start, the next module in the folder plays.

**Visuals:** Triangle steps through the views; Circle comes back to the list. First the list with
**channel bars** over it: one see-through LED bar per channel rising from the bottom, the whole
height at full level, with a peak that falls slowly. Then four full-screen visuals, each following
the music (its channels' levels and notes, every new note, and the sound itself):

- **Copper bars** - the Amiga's raster bars, one colour per channel, swinging on sines and flaring
  with their channel, over a starfield, with a sine scroller of the tune's name, all mirrored in
  rippling water
- **Glenz vectors** - a see-through faceted glenz cube round a lit, solid gem, turning faster and
  breathing with the music, over a warp starfield, each channel a glowing light in orbit
- **Plasma** - a plasma whose colours follow the notes, the sound itself as a glowing oscilloscope
  ring and line, sparks rising on new notes
- **Dancer** - a vector dancer on a disco floor, copper bars and stars behind: a new step on each
  beat (a new note on a loud channel), a new dance move every 8 steps (the disco point, waving arms,
  the robot, star jumps, side steps, hands in the air), a visor and chest light pulsing with the music

The song controls (pause, L1/R1, L2/R2) work there too. The visuals are `common/Visuals.h`, shared by
both apps.

**Modules added later over FTP:** copy them into `/data/homebrew/PPSA99905/assets/amiga/mods`, run
`python3 tools/make_music_list.py` on a copy of that folder on your PC, and copy the new
`music.txt` there too (the PS5 won't let the app list a folder).

## What's in here

| | |
| --- | --- |
| `src/core/AmigaCore.*` | the emulated Amiga as one unit (the board's file; a thread-safe reset added) |
| `src/core/chips/` | amiga500-esp32 and Musashi, as on the board (Paula at 48 kHz for the PS5) |
| `src/stubs/` | `Arduino.h` and `esp_heap_caps.h`: the emulator is built as for the board (`ARDUINO`) and these stand in |
| `src/ps5/ps5_main.cpp` | the TV picture, the DualSense, the menu, the sound, the emulator's thread |
| `src/ps5/ps5_system.cpp` | memory (`mmap`: a PS5 app's malloc heap is small), threads, the clock |
| `src/ps5/AmigaFilesPS5.cpp` | the Kickstart (and Amiga Forever's encryption) and the disks, from `assets/amiga` |
| `src/ps5/AmigaFont.h` | the board's 8 x 16 font |
| `src/ps5/ModEngine.*` | the board's ProTracker replayer (at 48 kHz here) |
| `../common/MusicList.h` | the music players' list (`music.txt`), shared with the C64; `build.sh` copies it in |
| `sce_sys/` | `param.json` and the icon |
| `sim/` | the PC simulator (the C64's stand-ins, `c64/sim/sim_sce.cpp`) |
| `tools/testrom.s` | a tiny test "Kickstart" for the simulator (below) |

The emulator runs on one thread here: the board's split of picture drawing and area blits onto its
second core isn't needed on the PS5.

## The PC simulator

```bash
amiga/sim/build.sh
# a folder with assets/amiga/kick.rom (and disks), then e.g. 10 seconds with pictures at 5 and 10 s
SIM_APP0=/path/to/that/folder SIM_FRAMES=600 SIM_SHOTS=300,600 amiga/sim/out/amigasim
```

`SIM_PAD`, `SIM_TOUCH` (`"110-170:0.2,0.6>0.5,0.3"` slides a finger) and `SIM_WAV` as for the C64
(`c64/README.md`); `SIM_LOG=1` shows the emulator's log.

Without a Kickstart, `tools/testrom.s` stands in: assembled with
[vasm](http://sun.hasenbraten.de/vasm/) (`vasmm68k_mot -Fbin -o testrom.rom testrom.s`), it shows
stripes through the copper and a bitplane, a sprite the mouse moves, a tone on Paula, changes
colour with the mouse buttons and the joystick, and draws each key code it gets from the keyboard
as an 8-pixel band at the top left (`SIM_USBKEYS` as for the C64). It boots nothing else.

## Not yet

- Custom-loader disks: standard AmigaDOS disks work; many games load with their own track loaders.
- 50 frames a second on a 60 Hz TV: one picture in five is shown twice.

## Licence

GPL-3.0 for this repository (`LICENSE`); amiga500-esp32 is MIT (`LICENSE_amiga500-esp32.txt`), as is
Musashi.
