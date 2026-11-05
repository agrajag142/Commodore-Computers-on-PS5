# Kickstart and disks

Put the Kickstart 1.3 ROM (`kick.rom`, 256K; or Amiga Forever's `amiga-os-130.rom` with its
`rom.key`) and your disk images (`.adf`) here before `amiga/build.sh`: they go inside the app
(`/app0/assets/amiga`). `build.sh` writes `files.txt`, the list the app reads (the PS5 won't let it
list the folder). None of them are kept in git.
