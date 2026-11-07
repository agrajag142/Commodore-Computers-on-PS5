# Amiga with Amiberry (test)

The Amiga app's front end (`amiga/src/ps5`: the menu, the controller, the USB keyboard and mouse,
the on-screen keyboard, the drive light, the MOD player) with [Amiberry](https://github.com/BlitterStudio/amiberry)
as the emulator instead of the board's: Amiberry's libretro core, compiled for the PS5, behind the
same interface (`AmigaCore.h`). Title ID `PPSA99906`, beside the Amiga 500 app (`PPSA99905`). A test:
stage 1 of moving the Amiga app to Amiberry.

## Build

```bash
PS5_STARTER=~/ps5-homebrew-starter amiberry/build.sh    # build/amiberry/PPSA99906 and its ZIP
```

It fetches Amiberry at the commit in `UPSTREAM` into `amiberry/upstream` (not in git), applies
`patches/`, compiles the libretro core with the SDK's clang (with C++ exceptions and RTTI, which
Amiberry uses: the kit's own apps go without) into `libamiberry.a`, and builds the app with the kit,
linked with that, zlib, `ps5port` and the SDK's libc++. The first compile of the core takes about an
hour on two cores.

The Kickstart and disks are the Amiga app's (`amiga/assets/amiga`); on the PS5, copy them into
`/data/homebrew/PPSA99906/assets/amiga` as for the Amiga app.

| | |
| --- | --- |
| `adapter/AmiberryCore.cpp` | the front end's emulator interface on Amiberry's libretro API: the picture (XRGB8888 into the 640 x 512 picture), 48 kHz sound, the mouse, joystick and keyboard, disks (written to the app's storage and put in DF0: through the disk control interface), the drive light, the Kickstart (as the front end loaded it, written out as `kick.rom`) |
| `adapter/weak_imports.c` | two weak symbols the kit's converter wants defined |
| `ps5port/` | what the PS5's C library lacks: iconv (UTF-8 / Latin-1), the per-locale functions libc++ calls, emulated thread-locals, and polite failures for a host shell and network databases |
| `patches/` | changes to Amiberry (FreeBSD's `sys/endian.h`) |
| `ps5.mk` | builds the core's objects only, and lists them |

The kit's link layout gets the four `__eh_frame*` symbols libunwind looks for (build.sh adds them to
the kit's copy). Amiberry writes its log to `/data/homebrew/PPSA99906/amiberry.log` (or
`/download0/amiberry/amiberry.log`); its own files (the Kickstart it reads, disks) are in
`/download0/amiberry`.

## Licence

Amiberry is GPL-3.0, as this repository is.
