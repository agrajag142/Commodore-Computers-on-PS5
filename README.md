# Commodore Computers on PS5

Justin's Commodore emulators from the Waveshare ESP32-P4 board (`WaveshareP4_C64Amiga`), as PS5
homebrew apps.

| | |
| --- | --- |
| [`c64/`](c64/README.md) | **Commodore 64** - joystick on the DualSense, the C64 keyboard on screen, programs (`.prg`) and disks (`.d64`) from a menu, a SID music player, a USB keyboard |
| [`amiberry/`](amiberry/README.md) | **Amiga** - [Amiberry](https://github.com/BlitterStudio/amiberry) behind the Amiga front end: A500, A500+, A600, A1200 or A4000, disks (`.adf`) and a hard drive (`.hdf`) from a menu, mouse on the touchpad, joystick on the D-pad, a MOD player, a USB keyboard and mouse; bring your own Kickstarts |
| [`amiga/`](amiga/README.md) | the Amiga front end (shared with `amiberry/`) and the board's own Amiga 500 core, the first Amiga app (now the classic core, `PPSA99907`) |
| `tools/` | `make_music_list.py` (the music players' lists, with HVSC song lengths), `make_test_music.py` (a test SID tune and module) |
| `common/` | shared by both music players: `MusicList.h` (their lists) and `Visuals.h` (channel bars, copper bars, glenz vectors, plasma, a dancer), `UsbKeyboard.h` (a USB keyboard and mouse straight from USB, after [PS5-KBM-Bridge](https://github.com/coru182/PS5-KBM-Bridge)) |

Built with the [PS5 homebrew starter](https://github.com/MiladRTX/ps5-homebrew-starter)'s kit and the
PS5 payload SDK v0.42, as NUCLEAR MISSION is. See each folder's README to build and install.
Title IDs: C64 `PPSA99904`, Amiga `PPSA99905` (Amiberry), the classic Amiga 500 core `PPSA99907`.
