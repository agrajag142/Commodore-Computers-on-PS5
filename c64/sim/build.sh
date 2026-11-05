#!/usr/bin/env bash
# Builds the PC simulator: the PS5 app's own code for Linux, with stand-ins
# for the PS5's system functions (sim_sce.cpp). For testing without the
# console; see sim_sce.cpp for the SIM_* settings.
#
#   c64/sim/build.sh        -> c64/sim/out/c64sim
#
# Then, for example (from c64/sim/out), 10 seconds, a picture at 5 and 10 s:
#   SIM_APP0=../.. SIM_FRAMES=600 SIM_SHOTS=300,600 ./c64sim
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$here/../src
out=$here/out
mkdir -p "$out/obj"
flags=(-std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -DPS5_SIM -pthread)
objs=()
while IFS= read -r -d '' f; do
    o=$out/obj/$(echo "${f#$src/}" | tr '/' '_').o
    if [[ ! -f $o || $f -nt $o ]]; then
        echo "  CC ${f#$src/}"
        g++ "${flags[@]}" -c "$f" -o "$o"
    fi
    objs+=("$o")
done < <(find "$src" -name '*.cpp' -print0 | sort -z)
g++ "${flags[@]}" -c "$here/sim_sce.cpp" -o "$out/obj/sim_sce.o"
g++ -pthread "${objs[@]}" "$out/obj/sim_sce.o" -o "$out/c64sim"
echo "Built $out/c64sim"
