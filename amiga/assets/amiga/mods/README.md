# Modules

Put ProTracker modules here (`.mod`, `mod.*`, `.nst`, `.stk`; folders inside are fine; up to 8 MB
each), before `amiga/build.sh`. The build writes `music.txt`, the list the MOD player reads
(`tools/make_music_list.py`).

`python3 tools/make_test_music.py amiga/assets/amiga/mods` makes a small test module.
Nothing here is kept in git apart from this file.
