#!/usr/bin/env bash
# Builds the Amiga app with Amiberry as its emulator (a test, beside the
# Amiga 500 app): the same front end (amiga/src/ps5), the emulator swapped
# for Amiberry's libretro core through amiberry/adapter/AmiberryCore.cpp.
#
#   amiberry/build.sh                 -> build/amiberry/PPSA99906/ and PPSA99906.zip
#   amiberry/build.sh deploy <PS5 IP> -> also copies it to /data/homebrew over FTP
#
# Steps:
#   1. Amiberry's source (amiberry/upstream, kept out of git): fetched at the
#      commit in amiberry/UPSTREAM, with its libretro-common and libco, and
#      amiberry/patches applied
#   2. its libretro core compiled for the PS5 (the SDK's clang, with C++
#      exceptions and RTTI, which Amiberry uses) into libamiberry.a, with
#      zlib and amiberry/ps5port (the bits of a C library the PS5 lacks)
#   3. the app, by the kit, as amiga/build.sh does, linked with those and
#      the SDK's libc++
# The first build of the core takes a while (an hour on two cores); after
# that only what changed is compiled again.
set -euo pipefail

usage() { echo "usage: amiberry/build.sh [deploy <PS5 IP>]" >&2; exit 2; }
deploy_host=
case ${1:-} in
    "") [[ $# -eq 0 ]] || usage ;;
    deploy) [[ $# -eq 2 && -n $2 ]] || usage; deploy_host=$2 ;;
    *) usage ;;
esac

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(dirname -- "$here")
home=${PS5_STARTER_HOME:-$HOME/.ps5-starter}
kit=$home/kit-work-amiberry
sdk=${PS5_PAYLOAD_SDK:-$home/ps5-payload-sdk}
jobs=${JOBS:-$(nproc)}

starter=${PS5_STARTER:-}
if [[ -z $starter ]]; then
    for s in "$repo/../ps5-homebrew-starter" "$HOME/ps5-homebrew-starter"; do
        [[ -d $s/kit ]] && { starter=$s; break; }
    done
fi
if [[ -z $starter || ! -d $starter/kit ]]; then
    echo "Can't find the PS5 homebrew starter (its kit/ folder): set PS5_STARTER." >&2
    exit 1
fi
starter=$(cd -- "$starter" && pwd)
[[ -x $sdk/bin/prospero-lld ]] || { echo "No PS5 payload SDK in $sdk: run the starter's tools/setup.sh." >&2; exit 1; }
command -v ccache >/dev/null || export USE_CCACHE=0

# ---- the kit (its own work folder), as amiga/build.sh
stamp=$(cd "$starter/kit" && find . -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16)
if [[ ! -f $kit/.kit-stamp || $(cat "$kit/.kit-stamp") != "$stamp" ]]; then
    echo "Preparing the kit..."
    mkdir -p "$kit"
    find "$kit" -mindepth 1 -maxdepth 1 ! -name .deps -exec rm -rf {} +
    cp -r "$starter/kit/." "$kit/"
    echo "$stamp" > "$kit/.kit-stamp"
fi
# the kit's link layout, with the symbols libunwind finds the exception
# tables by (C++ exceptions: Amiberry uses them; the kit's own apps don't)
ld=$kit/tooling/native/ps5-pie.ld
if ! grep -q __eh_frame_hdr_start "$ld"; then
    sed -i 's|KEEP(\*(.eh_frame_hdr))|__eh_frame_hdr_start = .; KEEP(*(.eh_frame_hdr)) __eh_frame_hdr_end = .;|;
            s|.eh_frame : { KEEP(\*(.eh_frame)) }|.eh_frame : { __eh_frame_start = .; KEEP(*(.eh_frame)) __eh_frame_end = .; }|' "$ld"
    grep -q __eh_frame_end "$ld" || { echo "Could not add the exception-table symbols to $ld" >&2; exit 1; }
fi
kit_sdk=$kit/.deps/native/ps5-payload-sdk
[[ ! -L $kit_sdk || -e $kit_sdk ]] || rm -f "$kit_sdk"
if [[ ! -e $kit_sdk ]]; then
    mkdir -p "$kit/.deps/native"
    ln -s "$sdk" "$kit_sdk"
fi
zlib_dir=$kit/.deps/native/zlib
if [[ ! -f $zlib_dir/zlib-1.3.2.tar.gz && ! -d $zlib_dir/root ]]; then
    mkdir -p "$zlib_dir"
    curl -fsSL -o "$zlib_dir/zlib-1.3.2.tar.gz" \
        https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz || rm -f "$zlib_dir/zlib-1.3.2.tar.gz"
fi

# ---- compilers for the core: the kit's PS5 clang wrapper, with exceptions and RTTI
work=$home/amiberry-work
mkdir -p "$work/bin" "$work/lib"
clang_c=$(command -v clang-18); clang_cxx=$(command -v clang++-18)
# (trace.list: core sources built so that the functions they enter are
# logged, for finding where the PS5 build dies; the compilers read the set
# from $work/trace-re)
trace_check='trace=; re=$(cat "'$work'/trace-re" 2>/dev/null); if [ -n "$re" ]; then for a in "$@"; do case $a in *.c|*.cpp) echo "$a" | grep -Eq "($re)\$" && trace=-finstrument-functions-after-inlining ;; esac; done; fi'
cat > "$work/bin/ps5cc" <<EOF
#!/bin/sh
$trace_check
PS5_PAYLOAD_SDK=$sdk PS5_CLANG=$clang_c exec sh $kit/tooling/prospero-clang18 -I$here/ps5port/include \$trace "\$@"
EOF
cat > "$work/bin/ps5cxx" <<EOF
#!/bin/sh
$trace_check
PS5_PAYLOAD_SDK=$sdk PS5_CLANG=$clang_cxx exec sh $kit/tooling/prospero-clang18 -I$here/ps5port/include -fexceptions -fcxx-exceptions -frtti \$trace "\$@"
EOF
chmod +x "$work/bin/ps5cc" "$work/bin/ps5cxx"
trace_re=$( [[ -f $here/trace.list ]] && grep -v '^#' "$here/trace.list" | grep . | paste -sd'|' || true)
old_re=$(cat "$work/trace-re" 2>/dev/null || true)
if [[ $old_re != "$trace_re" ]]; then
    # (the files that change state: their objects rebuilt)
    for re in "$old_re" "$trace_re"; do
        [[ -n $re ]] || continue
        (cd "$here/upstream" 2>/dev/null && find . \( -name '*.c' -o -name '*.cpp' \) | grep -E "($re)\$" | sed 's/\.[a-z]*$/.o/' | xargs -r rm -f) || true
    done
    echo "$trace_re" > "$work/trace-re"
fi

# ---- 1. Amiberry's source
up=$here/upstream
commit=$(grep -v '^#' "$here/UPSTREAM" | head -1)
if [[ ! -f $up/.ps5-commit || $(cat "$up/.ps5-commit") != "$commit" ]]; then
    echo "Fetching Amiberry $commit..."
    rm -rf "$up"
    git clone -q https://github.com/BlitterStudio/amiberry.git "$up"
    git -C "$up" checkout -q "$commit"
    git -C "$up" submodule update -q --init --depth 1 libretro/libretro-common libretro/libco
    for p in "$here"/patches/*.patch; do
        [[ -e $p ]] || continue
        git -C "$up" apply "$p"
    done
    echo "$commit" > "$up/.ps5-commit"
fi

# ---- 2. the core, zlib and ps5port
if [[ ! -f $work/lib/libz.a ]]; then
    echo "Building zlib..."
    rm -rf "$work/zlib" && mkdir -p "$work/zlib"
    tar -xzf "$zlib_dir/zlib-1.3.2.tar.gz" -C "$work/zlib" --strip-components=1
    for f in adler32 crc32 deflate infback inffast inflate inftrees trees zutil compress uncompr gzclose gzlib gzread gzwrite; do
        "$work/bin/ps5cc" -O2 -c "$work/zlib/$f.c" -o "$work/zlib/$f.o"
    done
    "$sdk/bin/llvm-ar" rcs "$work/lib/libz.a" "$work/zlib/"*.o
fi
for f in "$here"/ps5port/*.c "$here"/ps5port/*.cpp; do
    [[ -e $f ]] || continue
    o=$work/ps5port-$(basename "$f").o
    case $f in
        *.c) "$work/bin/ps5cc" -O2 -c "$f" -o "$o" ;;
        *) "$work/bin/ps5cxx" -O2 -std=gnu++17 -c "$f" -o "$o" ;;
    esac
done
rm -f "$work/lib/libps5port.a"
"$sdk/bin/llvm-ar" rcs "$work/lib/libps5port.a" "$work"/ps5port-*.o

echo "Compiling Amiberry's core (the first time: a long while)..."
core_make=(make -C "$up/libretro" -f Makefile -f "$here/ps5.mk" platform=unix ARCH=x86_64
    CC="$work/bin/ps5cc" CXX="$work/bin/ps5cxx" ZLIB_CFLAGS="-I$work/zlib" ZLIB_LIBS= PKG_CONFIG=false)
"${core_make[@]}" -k -j"$jobs" objs > "$work/core-build.log" 2>&1 || {
    grep -E " error:|fatal error" "$work/core-build.log" | head -20
    echo "The core did not compile: see $work/core-build.log" >&2
    exit 1
}
objects=$("${core_make[@]}" -s print-objects)
rm -f "$work/lib/libamiberry.a"
(cd "$up/libretro" && "$sdk/bin/llvm-ar" rcs "$work/lib/libamiberry.a" $objects)

# ---- 3. the app: the Amiga app's front end, the adapter, the libraries
rm -rf "$kit/amiberry"
mkdir -p "$kit/amiberry/src/core/chips" "$kit/amiberry/src/adapter" "$kit/amiberry/assets/amiga" "$kit/amiberry/lib"
cp -r "$repo/amiga/src/ps5" "$repo/amiga/src/stubs" "$kit/amiberry/src/"
rm -f "$kit/amiberry/src/ps5/cxx_runtime.cpp"           # (the real libc++ is linked here)
cp "$repo/amiga/src/core/AmigaCore.h" "$kit/amiberry/src/core/"
cp "$repo/amiga/src/core/chips/a500.h" "$kit/amiberry/src/core/chips/"
cp "$here/adapter/"*.cpp "$here/adapter/"*.c "$kit/amiberry/src/adapter/"
cp "$up/libretro/libretro.h" "$kit/amiberry/src/adapter/"
cp "$repo/common/"*.h "$kit/amiberry/src/ps5/"
cp -r "$here/sce_sys" "$kit/amiberry/"
cp "$work/lib/"*.a "$kit/amiberry/lib/"
# (the kit takes archive names of letters, digits, dot, dash and underscore)
cp "$sdk/target/lib/libc++.a" "$kit/amiberry/lib/libcxx.a"
cp "$sdk/target/lib/libc++abi.a" "$kit/amiberry/lib/libcxxabi.a"
cp "$sdk/target/lib/libunwind.a" "$kit/amiberry/lib/libunwind.a"
# (their "link pthread too" notes name a library the kit's link doesn't use:
#  the pthread functions come from the PS5's own libraries)
for l in libcxx.a libcxxabi.a libunwind.a; do
    "$sdk/bin/llvm-objcopy" --remove-section=.deplibs "$kit/amiberry/lib/$l"
done
# the Kickstart and disks: as for the Amiga app (amiga/assets/amiga)
shopt -s nullglob nocaseglob
files=()
for f in "$repo/amiga/assets/amiga/"*.rom "$repo/amiga/assets/amiga/"*.a500 "$repo/amiga/assets/amiga/"rom.ke[y] \
         "$repo/amiga/assets/amiga/"*.adf; do
    cp "$f" "$kit/amiberry/assets/amiga/"
    files+=("$(basename -- "$f")")
done
shopt -u nullglob nocaseglob
printf '%s\n' "${files[@]}" | sort -f > "$kit/amiberry/assets/amiga/files.txt"
if [[ -d $repo/amiga/assets/amiga/mods ]]; then
    mkdir -p "$kit/amiberry/assets/amiga/mods"
    cp -r "$repo/amiga/assets/amiga/mods/." "$kit/amiberry/assets/amiga/mods/"
    find "$kit/amiberry/assets/amiga/mods" -name 'README*' -delete
    python3 "$repo/tools/make_music_list.py" "$kit/amiberry/assets/amiga/mods"
    find "$kit/amiberry/assets/amiga/mods" -name 'Songlengths.md5' -delete
fi
bash "$kit/tools/validate-assets.sh" "$kit/amiberry/sce_sys"

libs="amiberry/lib/libamiberry.a amiberry/lib/libps5port.a amiberry/lib/libz.a amiberry/lib/libcxx.a amiberry/lib/libcxxabi.a amiberry/lib/libunwind.a"
vars=(APP_SOURCE_DIR=amiberry/src APP_PARAM=amiberry/sce_sys/param.json
      APP_SCE_SYS=amiberry/sce_sys APP_ASSETS=amiberry/assets
      APP_DEFINITIONS="ARDUINO AMIBERRY" APP_INCLUDE_PATHS="amiberry/src/stubs amiberry/src/core amiberry/src/adapter"
      APP_STATIC_ARCHIVES="$libs" APP_WRAP_SYMBOLS="__cxa_throw stat lstat open fopen mkdir pthread_create exit _exit abort mmap munmap mprotect madvise sigaction signal")
if [[ -n $deploy_host ]]; then
    make -C "$kit" "${vars[@]}" deploy PS5_HOST="$deploy_host"
else
    make -C "$kit" "${vars[@]}"
fi

title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$here/sce_sys/param.json")
rm -rf "$repo/build/amiberry"
mkdir -p "$repo/build/amiberry"
cp -r "$kit/dist/$title" "$kit/dist/$title.zip" "$repo/build/amiberry/"
echo
echo "App folder: build/amiberry/$title"
echo "ZIP:        build/amiberry/$title.zip"
