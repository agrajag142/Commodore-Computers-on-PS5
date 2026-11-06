# Commodore 64 for the PS5

The C64 emulator from the Waveshare ESP32-P4 sketch (`WaveshareP4_C64Amiga_003`, itself the
[T-HMI-C64](https://github.com/retroelec/T-HMI-C64) emulator by retroelec) as a PS5 home-screen
app, built with the [PS5 homebrew starter](https://github.com/MiladRTX/ps5-homebrew-starter)'s kit,
the same way as NUCLEAR MISSION. Title ID `PPSA99904`, so it sits beside Hello PS5 (`PPSA99901`) and
NUCLEAR MISSION (`PPSA99902`).

## Build and install

In WSL or Linux, after the starter's `tools/setup.sh` (as for Hello PS5):

```bash
chmod +x c64/build.sh                         # (if the zip was unpacked in Windows)
cp /path/to/your/games/*.prg c64/assets/c64/  # programs and .d64 disks: the menu lists them
PS5_STARTER=~/ps5-homebrew-starter c64/build.sh             # build/c64/PPSA99904 and its ZIP
PS5_STARTER=~/ps5-homebrew-starter c64/build.sh deploy <PS5 IP>   # or build and copy it over FTP
```

`PS5_STARTER` can be left out when the starter is in `~/ps5-homebrew-starter` or next to this
repository. Then put it on the home screen with ShadowMountPlus, as with Hello PS5.

## Controls

**Playing** (the C64 picture 4x, the full height of the TV):

| DualSense | C64 |
| --- | --- |
| D-pad or left stick | joystick |
| Cross (or R2) | fire |
| Square | SPACE |
| Circle | RETURN |
| R1 | F1 |
| L1 | joystick in port 1 / port 2 (most games: 2) |
| Triangle | the on-screen keyboard |
| Options | the menu |
| Touchpad click | the SID music player, straight away |

**On-screen keyboard** (Triangle; the C64 picture 3x above it):

| DualSense | |
| --- | --- |
| D-pad or left stick | choose a key (red frame) |
| Touchpad | is the keyboard: point at a key; click to press it |
| Cross | press the chosen key (held as long as Cross is) |
| Square / Circle / R1 | SPACE / INST-DEL / RETURN |
| L1 (held) | SHIFT |
| Triangle | back to the joystick |

SHIFT, CTRL and C= chosen with Cross hold for the next key (blue), as on the board.
C= then SHIFT switches upper / lower case. SHIFT LOCK stays on until pressed again.
RESTORE works too.

**Menu** (Options): BACK TO THE C64, SID MUSIC PLAYER, RESET C64, the joystick port, the volume,
USB KEYBOARD TEST (below), QUIT TO THE PS5 HOME SCREEN (Cross twice), then the programs and disks. It opens on the program chosen last (or the first one). Choosing one restarts the C64
and starts it:

- a `.prg`: loaded straight into memory and RUN (as the board's AUTOSTART does)
- a `.d64`: put in the drive, then `LOAD"*",8,1` and `RUN` typed

The C64 keeps running behind the menu. D-pad up / down, Cross chooses, left / right change the
port and the volume, Circle or Options go back.

## USB keyboard test (experimental)

**USB KEYBOARD TEST** in the menu finds out whether a USB keyboard plugged into the PS5 reaches the
app. It loads Sony's keyboard library (`libSceKeyboard`) while the app runs, opens the keyboard
and shows, each frame, the raw bytes it returns: red where a byte has just changed, and below, the
latest changes (`offset=value`, unlike the keyboard left alone). Leave the keyboard alone for the
first second (that's how it tells a clock in the bytes from a key), then press keys one at a time.
Circle goes back.

Every step also goes into `kbdtest.txt` (in `/data/homebrew/PPSA99904`, else `/download0`; the
screen says which, or that neither could be written), written before each call: if the test closes
the app, the file's last line says which call did it. The code is `src/ps5/KeyboardProbe.*`; the
simulator has a made-up keyboard (`SIM_KEY="300-320:04"` holds key code 04 in byte 8 for those
frames, `SIM_NOKBD=1` makes the library fail to load).

The test first checks that the app can load libraries this way at all (the controller's and the
sound's, which it already uses), then whether the keyboard library's file can be seen. A second
build, `KBD_DIRECT=1 c64/build.sh` (`build/c64kbd/PPSA99906`, "C64 Keyboard Test", beside the C64),
also imports the keyboard library the usual way, as the app does the controller's: if the PS5
can't give the library to a homebrew app, that build doesn't start at all.

## SID music player

**SID MUSIC PLAYER** in the menu (Options) opens the board's SID player as a screen of its own. It
saves the C64 as it is (a game carries on afterwards) and plays the tunes on the same emulated SID
chip as games do.

Put `.sid` files in `c64/assets/c64/music` before building (folders inside are fine: HVSC's
`C64Music` folder works as it is). With HVSC's `Songlengths.md5` there too, each song's length is
shown and the player moves on when it ends (the next song, then the next tune); without it, after
5 seconds of silence.

| DualSense | |
| --- | --- |
| D-pad up / down (left / right a page) | choose a folder or tune |
| Cross | open the folder / play the tune |
| Circle | up a folder (at the top: back to the C64) |
| Square | pause |
| Triangle | the visuals (below), the next one, then back to the list |
| Touchpad click | stop |
| L1 / R1 | the tune's previous / next song |
| L2 / R2 | the previous / next tune in the folder |
| Options | back to the C64 |

The screen shows the tune's title, author and release, the song and its time, the SID model, and
for each voice its waveform, note and envelope level.

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

**Music added later over FTP:** the PS5 won't let the app list a folder, so the player reads
`music.txt`, the list the build makes with `tools/make_music_list.py`. Copy the tunes into
`/data/homebrew/PPSA99904/assets/c64/music`, run the tool on a copy of that folder on your PC
(`python3 tools/make_music_list.py path/to/music`), and copy the new `music.txt` there too.

## Drive 8

- `LOAD"NAME",8` loads `NAME.prg` from the app (any case; `NA*` and `*` work).
- `LOAD"$",8` then `LIST` shows the programs (the drive makes the listing up, as the board's
  built-in drive does).
- With a disk in the drive (from the menu), `LOAD"$",8` shows the disk and everything loads from it.
- `SAVE"NAME",8` saves into the app's own storage (`/download0/c64`); the menu lists saved programs too.

The PS5's sandbox won't list a folder, so `build.sh` writes `assets/c64/files.txt` with the names.
Files copied into `/data/homebrew/PPSA99904/assets/c64` over FTP later need a line in that
`files.txt` as well.

## What's in here

| | |
| --- | --- |
| `src/core/` | the emulator: CPU, VIC, SID, CIAs, the 1541 drive, cartridges (T-HMI-C64 with the Waveshare port's fixes) and the C64's ROMs |
| `src/ps5/ps5_main.cpp` | the TV picture, the DualSense, the on-screen keyboard and the menu |
| `src/ps5/PS5Drivers.cpp` | the emulator's display, sound, keyboard and joystick drivers |
| `src/ps5/PS5File.cpp` | drive 8: the app's `assets/c64` folder |
| `src/ps5/PlatformPS5.cpp` | threads, timers, the clock |
| `src/ps5/P4KeyboardData.h` | the Waveshare port's keyboard picture (scaled 1.5x on the TV) |
| `src/ps5/P4SidEngine.*` | the board's SID player engine (a spin lock for `std::mutex`, fixed buffers) |
| `../common/MusicList.h` | the music players' list (`music.txt`), shared with the Amiga; `build.sh` copies it in |
| `src/ps5/cxx_runtime.cpp` | the bit of the C++ library a PS5 app doesn't link (std::string) |
| `sce_sys/` | `param.json` and the home-screen icon |
| `assets/c64/` | your programs and disks (kept out of git, apart from the demo and the test disk); `music/`: SID tunes |
| `sim/` | the PC simulator (below) |
| `tools/make_d64.py` | makes a test disk from a program |

### Changes to the emulator for the PS5

All marked "PS5 port" in the source:

- `Config.h`: one PS5 set-up; 48 kHz sound (the PS5's AudioOut only takes 48 kHz).
- The driver factories only declare `create()`; the PS5 drivers define it.
- No exceptions (a PS5 app has none): `PlatformManager` stops instead of throwing; `C64Sys`
  without its `try`; `Floppy` without `std::stringstream`, `std::stoul` and `std::to_string`.
- No `.config.json` (and so no JSON library): `FileConfig` is an empty stand-in.
- Two new external commands: `WAITBASIC` (waits until BASIC is at READY after a reset - the start-up
  takes about 3 seconds) and `TYPETEXT` (types into the keyboard buffer when BASIC is waiting). The
  command queue is now safe to fill from the screen thread.

## The PC simulator

The same code built for Linux, with stand-ins for the PS5's system functions (`sim/sim_sce.cpp`): a
scripted controller, screenshots of the TV picture, the sound to a WAV file. For testing without
the console:

```bash
c64/sim/build.sh
mkdir -p c64/sim/run && cd c64/sim/run
# boot, Options at 3 s, Cross: load the first program; pictures at 3.5 s and 12 s
SIM_APP0=../.. SIM_FRAMES=740 SIM_SHOTS=210,720 SIM_PAD="180-183:8;200-203:4000" ../out/c64sim
```

`SIM_PAD` holds buttons (the PS5's bits, in hex: Cross `4000`, Square `8000`, Circle `2000`,
Triangle `1000`, Options `8`, D-pad up `10` / right `20` / down `40` / left `80`, L1 `400`,
R1 `800`) for frames from-to. `SIM_TOUCH="200-260:0.5,0.4"` puts a finger on the touchpad,
`SIM_WAV=out.wav` keeps the sound. `SIM_SPEED=1` prints the C64's speed every 10 seconds while a SID
tune plays (50.0 frames a second is a real PAL C64).

## Not yet

- Cartridges (`.crt`) from the board's menu.
- The C64 runs at 50 frames a second (PAL) and the TV at 60, so one picture in five is shown
  twice: smooth scrolling can look slightly uneven.
- Nothing is kept when the app closes (the C64's memory; the volume and port go back to 80% and 2).

## Licence

GPL-3.0 (see `LICENSE`), as the emulator and the starter's kit are. The C64's ROMs in
`src/core/roms` are Commodore's, included as in the T-HMI-C64 project.
