# Commodore Computers on PS5

Justin's Commodore emulators from the Waveshare ESP32-P4 board (`WaveshareP4_C64Amiga`), as PS5
homebrew apps.

| | |
| --- | --- |
| [`c64/`](c64/README.md) | **Commodore 64** - joystick on the DualSense, the C64 keyboard on screen, programs (`.prg`) and disks (`.d64`) from a menu, a SID music player |
| [`amiga/`](amiga/README.md) | **Amiga 500** - mouse on the touchpad, joystick on the D-pad, disks (`.adf`) from a menu, a MOD player; bring your own Kickstart 1.3 |
| `tools/` | `make_music_list.py` (the music players' lists, with HVSC song lengths), `make_test_music.py` (a test SID tune and module) |
| `common/` | shared by both music players: `MusicList.h` (their lists) and `Visuals.h` (copper bars, glenz vectors, plasma) |

Built with the [PS5 homebrew starter](https://github.com/MiladRTX/ps5-homebrew-starter)'s kit and the
PS5 payload SDK v0.42, as NUCLEAR MISSION is. See each folder's README to build and install.
Title IDs: C64 `PPSA99904`, Amiga 500 `PPSA99905`.
