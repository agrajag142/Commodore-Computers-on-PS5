#!/usr/bin/env bash
# The PC simulator for the Amiberry app: the Amiga app's front end, the
# adapter and Amiberry's core, all built for Linux, with the PS5 stand-ins
# (c64/sim/sim_sce.cpp). The core's objects come from a native build of
# amiberry/upstream (NATIVE=path/to/a copy built with platform=unix and
# ps5.mk's "objs" target, default ~/amiberry-native).
#
#   amiberry/sim/build.sh   -> amiberry/sim/out/amiberrysim
#
# SIM_SANDBOX=1 makes folder listing fail as on the PS5. The app writes to
# /download0/amiberry and /data/homebrew/PPSA99906, as on the PS5.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
native=${NATIVE:-$HOME/amiberry-native}
out=$here/out
mkdir -p "$out/obj" "$out/inc/core/chips"
cp "$repo/amiga/src/core/AmigaCore.h" "$out/inc/core/"
cp "$repo/amiga/src/core/chips/a500.h" "$out/inc/core/chips/"
common=(-O2 -g -DARDUINO -DAMIBERRY -DPS5_SIM -I"$repo/amiga/src/stubs" -I"$repo/common" -I"$out/inc/core"
        -I"$native/libretro" -pthread -w)
objs=()
srcs=("$repo"/amiga/src/ps5/*.cpp "$repo"/amiberry/adapter/*.cpp "$here"/sim_ps5port.cpp)
for f in "${srcs[@]}"; do
    [[ $(basename "$f") == cxx_runtime.cpp ]] && continue
    o=$out/obj/$(basename "$f").o
    # (the front end includes ../core/...: build it from a copy beside inc/core)
    if [[ ! -f $o || $f -nt $o ]]; then
        echo "  CXX $(basename "$f")"
        if [[ $f == */amiga/src/ps5/* ]]; then
            mkdir -p "$out/inc/ps5"
            cp "$f" "$out/inc/ps5/"
            for h in "$repo"/amiga/src/ps5/*.h; do cp "$h" "$out/inc/ps5/"; done
            mkdir -p "$out/inc/stubs"; cp "$repo"/amiga/src/stubs/*.h "$out/inc/stubs/"
            g++ -std=c++20 -fno-exceptions -fno-rtti "${common[@]}" -c "$out/inc/ps5/$(basename "$f")" -o "$o"
        else
            g++ -std=c++20 "${common[@]}" -c "$f" -o "$o"
        fi
    fi
    objs+=("$o")
done
g++ -std=c++20 -O2 -c "$repo/c64/sim/sim_sce.cpp" -o "$out/obj/sim_sce.o"
core=$(cd "$native/libretro" && make -s -f Makefile -f "$repo/amiberry/ps5.mk" platform=unix ARCH=x86_64 print-objects)
corepaths=()
for o in $core; do corepaths+=("$native/libretro/$o"); done
g++ -pthread -rdynamic "${objs[@]}" "$out/obj/sim_sce.o" "${corepaths[@]}" -o "$out/amiberrysim" -lz -ldl -lutil -lm
echo "Built $out/amiberrysim"
