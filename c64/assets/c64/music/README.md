# SID music

Put `.sid` files here (folders inside are fine - the High Voltage SID Collection's `C64Music`
folder works as it is), before `c64/build.sh`. The build writes `music.txt`, the list the SID
player reads (`tools/make_music_list.py`). With HVSC's `Songlengths.md5` in this folder (or its
`DOCUMENTS` folder) the list gets each song's length too, and the player moves on when a song
ends; without it, after 5 seconds of silence.

`python3 tools/make_test_music.py c64/assets/c64/music` makes a small test tune.
Nothing here is kept in git apart from this file.
