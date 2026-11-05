# Programs and disks

Put C64 programs (`.prg`) and disk images (`.d64`) here before `c64/build.sh`: they go inside the
app (`/app0/assets/c64`) and the menu (Options) lists them. `build.sh` writes `files.txt`, the list
the app reads (the PS5 won't let it list the folder).

Only `demo.prg` (plays a scale) and `testdisk.d64` (the same demo on a disk, from
`c64/tools/make_d64.py`) are kept in git; anything else you put here stays on your computer.
