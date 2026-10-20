#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."

TGR_ROM=${TGR_ROM:-reference/tgrally/Top Gear Rally (USA).z64}
BIN=${BIN:-reference/brally/BossRally.BIN}
CUE=${CUE:-reference/brally/BossRally.cue}
DLL=${DLL:-reference/brally/orig/BRGlide.dll}

command -v curl >/dev/null 2>&1 || { echo "fetch-game-data: curl not found" >&2; exit 1; }

get() {
    dest=$1
    url=$2
    label=$3
    required=${4:-1}
    hint=${5:-}
    if [ -s "$dest" ]; then
        echo "$label: $dest"
        return 0
    fi
    if [ -z "$url" ]; then
        [ "$required" = 0 ] && return 0
        echo "$label: no local file and no URL configured${hint:+ ($hint)}" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$dest")"
    echo "$label: downloading"
    curl -fL --retry 5 --retry-delay 3 --retry-all-errors --connect-timeout 20 \
        -o "$dest.part" "$url"
    mv "$dest.part" "$dest"
    echo "$label: $dest"
}

get "$TGR_ROM" "${TGR_ROM_URL:-}" "TGR ROM" 1 "set the TGR_ROM_URL secret"
get "$BIN" "${BOSS_RALLY_BIN_URL:-}" "Boss Rally disc image" 1 "set the BOSS_RALLY_BIN_URL secret"
get "$CUE" "${BOSS_RALLY_CUE_URL:-}" "Boss Rally cue" 0 "optional: set BOSS_RALLY_CUE_URL"
if [ ! -s "$DLL" ] && [ -s reference/brally/data/disc/BRGlide.dll ]; then
    mkdir -p "$(dirname "$DLL")"
    cp reference/brally/data/disc/BRGlide.dll "$DLL"
fi
get "$DLL" "${BOSS_RALLY_DLL_URL:-}" "Boss Rally DLL" 1 "set the BOSS_RALLY_DLL_URL secret"
