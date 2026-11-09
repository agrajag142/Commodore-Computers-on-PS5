# Amiga for the PS5 (Amiberry)

The Amiga app: the Amiga front end (`amiga/src/ps5`: the menu, the controller, the USB keyboard and
mouse, the on-screen keyboard, the drive lights, the MOD player) with
[Amiberry](https://github.com/BlitterStudio/amiberry) as the emulator: Amiberry's libretro core,
compiled for the PS5, behind the front end's interface (`AmigaCore.h`). Title ID `PPSA99905`, beside
the C64 (`PPSA99904`). (The board's own Amiga 500 core is still in `amiga/`, as `PPSA99907`.)

## What you need

No ROM, disk or hard drive comes with it: use your own (Amiga Forever has them; its encrypted ROMs
work with its `rom.key`). In `assets/amiga`, and named in `files.txt` unless they have one of the
usual names (`kick.rom`, `kick204.rom`, `kick205.rom`, `kick31.rom`, `kick40068.A1200`,
`amiga-os-310-a1200.rom`, `default.adf`, `default.hdf`, ...):

| Model (menu: Model) | Kickstart |
| --- | --- |
| A500 | 1.3 |
| A500+ | 2.04 |
| A600 | 2.05 (or 3.1 for the A500/A600) |
| A1200 | 3.1 for the A1200 (40.68) or 3.0 |
| A4000 (68040) | 3.1 for the A4000 |

Each ROM goes to its model by its version (and, for 3.x, its checksum or a "1200"/"4000" in its
name); the top of the log says where each went. The menu offers the models with a Kickstart.

- **Disks** (`.adf`): Cross boots one (a reset, the disk in), Square swaps it into DF0:.
- **Hard drive** (`.hdf`, `.hdz`), DH0:: Cross in the menu's hard-drive list starts the Amiga again
  with it (a partition table, or one partition, as Amiberry finds); "(no hard drive)" takes it out.
  `default.hdf` boots at start unless another one (or none) was chosen. Opened from
  `/data/homebrew/PPSA99905/assets/amiga`, so the Amiga can write to it. Tested up to 3.8 GB (OS 3.9).
- The model, the hard drive, the volume and so on are kept in `/download0/amiberry`.

## Build

```bash
PS5_STARTER=~/ps5-homebrew-starter amiberry/build.sh    # build/amiberry/PPSA99905 and its ZIP
```

It fetches Amiberry at the commit in `UPSTREAM` into `amiberry/upstream` (not in git), applies
`patches/`, compiles the libretro core with the SDK's clang (with C++ exceptions and RTTI, which
Amiberry uses: the kit's own apps go without) into `libamiberry.a`, and builds the app with the kit,
linked with that, zlib, `ps5port` and the SDK's libc++. The first compile of the core takes about an
hour on two cores.

The files in `amiga/assets/amiga` (not in git) go into the app; on the PS5, copy files into
`/data/homebrew/PPSA99905/assets/amiga` over FTP.

| | |
| --- | --- |
| `adapter/AmiberryCore.cpp` | the front end's emulator interface on Amiberry's libretro API: the picture (XRGB8888 into the 640 x 512 picture), 48 kHz sound, the mouse, joystick and keyboard, disks (written to the app's storage and put in DF0: through the disk control interface), the drive light, the Kickstart (as the front end loaded it, written out as `kick.rom`) |
| `adapter/weak_imports.c` | two weak symbols the kit's converter wants defined |
| `ps5port/` | what the PS5's C library lacks: iconv (UTF-8 / Latin-1), the per-locale functions libc++ calls, emulated thread-locals, and polite failures for a host shell and network databases |
| `patches/` | changes to Amiberry for the PS5: FreeBSD's `sys/endian.h`, no exceptions from `std::filesystem` checks, a smaller picture buffer, a 160 MB memory reservation (released with its size), no folder drive (the PS5 lets no folder be listed) |
| `ps5port/bigalloc.c` | blocks of 4 MB and more mapped straight from the system (the PS5's heap is small) |
| `ps5port/diagnostics.cpp` | crash reports into the log (the instruction pointer is context word 28), and the debugging aids `kDebug` in the adapter turns on: a self-test, every call from the core, a function trace (`trace.list`) |
| `ps5.mk` | builds the core's objects only, and lists them |

The kit's link layout gets the four `__eh_frame*` symbols libunwind looks for (build.sh adds them to
the kit's copy). Amiberry writes its log to `/data/homebrew/PPSA99905/amiberry.log` (or
`/download0/amiberry/amiberry.log`); its own files (the Kickstart it reads, disks) are in
`/download0/amiberry`.

## Licence

Amiberry is GPL-3.0, as this repository is.
