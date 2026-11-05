#!/usr/bin/env bash
# Builds the PC simulator: the Amiga app's own code for Linux, with the
# stand-ins for the PS5's system functions shared with the C64
# (c64/sim/sim_sce.cpp: the SIM_* settings are described there).
#
#   amiga/sim/build.sh      -> amiga/sim/out/amigasim
#
# Then, for example (from a folder of your own), 10 seconds, pictures at 5 and 10 s:
#   SIM_APP0=path/to/amiga SIM_FRAMES=600 SIM_SHOTS=300,600 path/to/amigasim
# (SIM_APP0/assets/amiga holds the Kickstart and the disks.)
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$here/../src
out=$here/out
mkdir -p "$out/obj"
common=(-O2 -DARDUINO -DPS5_SIM -I"$src/stubs" -pthread -w)
objs=()
while IFS= read -r -d '' f; do
    o=$out/obj/$(echo "${f#$src/}" | tr '/' '_').o
    if [[ ! -f $o || $f -nt $o ]]; then
        echo "  CC ${f#$src/}"
        if [[ $f == *.c ]]; then gcc -std=gnu11 "${common[@]}" -c "$f" -o "$o"
        else g++ -std=c++20 -fno-exceptions -fno-rtti "${common[@]}" -c "$f" -o "$o"; fi
    fi
    objs+=("$o")
done < <(find "$src" \( -name '*.cpp' -o -name '*.c' \) -print0 | sort -z)
g++ -std=c++20 -O2 -c "$here/../../c64/sim/sim_sce.cpp" -o "$out/obj/sim_sce.o"
g++ -pthread "${objs[@]}" "$out/obj/sim_sce.o" -o "$out/amigasim" -lm
echo "Built $out/amigasim"
