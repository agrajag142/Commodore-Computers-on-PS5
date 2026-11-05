#!/usr/bin/env bash
# Builds the Commodore 64 as a PS5 app folder and ZIP, with the PS5 homebrew
# starter's kit and SDK (the same set-up as Hello PS5 and NUCLEAR MISSION:
# run the starter's tools/setup.sh once first).
#
#   c64/build.sh                    -> build/c64/PPSA99903/ and PPSA99903.zip
#   c64/build.sh deploy <PS5 IP>    -> also copies it to /data/homebrew over FTP
#
# Where the starter is: PS5_STARTER=/path/to/ps5-homebrew-starter, or a
# ps5-homebrew-starter folder next to this repository, or ~/ps5-homebrew-starter.
#
# Put your programs (.prg) and disks (.d64) in c64/assets/c64 first: they go
# inside the app, and the menu (Options) lists them.
set -euo pipefail

usage() { echo "usage: c64/build.sh [deploy <PS5 IP>]" >&2; exit 2; }
deploy_host=
case ${1:-} in
    "") [[ $# -eq 0 ]] || usage ;;
    deploy) [[ $# -eq 2 && -n $2 ]] || usage; deploy_host=$2 ;;
    *) usage ;;
esac

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(dirname -- "$here")
home=${PS5_STARTER_HOME:-$HOME/.ps5-starter}
kit=$home/kit-work-c64
sdk=${PS5_PAYLOAD_SDK:-$home/ps5-payload-sdk}

# 0. The starter (its kit/ folder)
starter=${PS5_STARTER:-}
if [[ -z $starter ]]; then
    for s in "$repo/../ps5-homebrew-starter" "$HOME/ps5-homebrew-starter"; do
        [[ -d $s/kit ]] && { starter=$s; break; }
    done
fi
if [[ -z $starter || ! -d $starter/kit ]]; then
    echo "Can't find the PS5 homebrew starter (its kit/ folder)." >&2
    echo "Set PS5_STARTER=/path/to/ps5-homebrew-starter, or put it next to this repository." >&2
    exit 1
fi
starter=$(cd -- "$starter" && pwd)
[[ -x $sdk/bin/prospero-lld ]] || {
    echo "No PS5 payload SDK in $sdk: run $starter/tools/setup.sh first." >&2
    exit 1
}
command -v ccache >/dev/null || export USE_CCACHE=0

# 1. The kit, copied to its own work folder whenever kit/ changed (as the
#    starter's app/build.sh does; kit-work-c64, so Hello PS5's and NUCLEAR
#    MISSION's kit-work is left alone)
stamp=$(cd "$starter/kit" && find . -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16)
if [[ ! -f $kit/.kit-stamp || $(cat "$kit/.kit-stamp") != "$stamp" ]]; then
    echo "Preparing the kit..."
    mkdir -p "$kit"
    find "$kit" -mindepth 1 -maxdepth 1 ! -name .deps -exec rm -rf {} +
    cp -r "$starter/kit/." "$kit/"
    echo "$stamp" > "$kit/.kit-stamp"
fi
kit_sdk=$kit/.deps/native/ps5-payload-sdk
[[ ! -L $kit_sdk || -e $kit_sdk ]] || rm -f "$kit_sdk"
if [[ ! -e $kit_sdk ]]; then
    mkdir -p "$kit/.deps/native"
    ln -s "$sdk" "$kit_sdk"
fi
# (the kit builds zlib on its first run; if zlib.net can't be reached, the
# same file from GitHub will do - the kit checks its SHA-256 either way)
zlib_dir=$kit/.deps/native/zlib
if [[ ! -f $zlib_dir/zlib-1.3.2.tar.gz && ! -d $zlib_dir/root ]]; then
    mkdir -p "$zlib_dir"
    curl -fsSL -o "$zlib_dir/zlib-1.3.2.tar.gz" \
        https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz || rm -f "$zlib_dir/zlib-1.3.2.tar.gz"
fi

# 2. The app's files beside the kit's own
rm -rf "$kit/c64"
mkdir -p "$kit/c64/assets/c64"
cp -r "$here/src" "$here/sce_sys" "$kit/c64/"
# programs and disks, and the list of them (the PS5 won't list a folder)
shopt -s nullglob nocaseglob
files=()
for f in "$here/assets/c64/"*.prg "$here/assets/c64/"*.d64; do
    cp "$f" "$kit/c64/assets/c64/"
    files+=("$(basename -- "$f")")
done
shopt -u nullglob nocaseglob
printf '%s\n' "${files[@]}" | sort -f > "$kit/c64/assets/c64/files.txt"
echo "Programs and disks in the app: ${#files[@]}"
[[ ${#files[@]} -gt 0 ]] || echo "  (none: put .prg and .d64 files in c64/assets/c64)"
bash "$kit/tools/validate-assets.sh" "$kit/c64/sce_sys"

# 3. Build. The kit compiles every .c/.cpp under c64/src.
vars=(APP_SOURCE_DIR=c64/src APP_PARAM=c64/sce_sys/param.json
      APP_SCE_SYS=c64/sce_sys APP_ASSETS=c64/assets)
if [[ -n $deploy_host ]]; then
    make -C "$kit" "${vars[@]}" deploy PS5_HOST="$deploy_host"
else
    make -C "$kit" "${vars[@]}"
fi

# 4. The result, next to the sources
title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' \
    "$here/sce_sys/param.json")
rm -rf "$repo/build/c64"
mkdir -p "$repo/build/c64"
cp -r "$kit/dist/$title" "$kit/dist/$title.zip" "$repo/build/c64/"
echo
echo "App folder: build/c64/$title"
echo "ZIP:        build/c64/$title.zip"
