#!/bin/sh
# Build "Top Gear Rally Remastered" for Linux: link.sh with the Wayland host
# (host/linux: a Wayland window, ALSA, evdev pads, music through FFmpeg) and
# the Vulkan renderer, Boss Rally's data track as its CD root, its CD audio,
# and a .desktop file.
# Top Gear Rally's data (from your ROM) and Boss Rally's BRGlide.dll data are
# built into the executable, so the folder needs neither ROM nor disc once built.
#
#   remastered                  the two games and the host (the executable)
#   disc                        Boss Rally's data track, less the installer and the video
#   skies                       the lighting's skymaps (shelved: only with RM_SHELVED_ASSETS=1)
#   surfaces                    the Remastered textures' sets (shelved: only with RM_SHELVED_ASSETS=1)
#   music/cd                    Boss Rally's CD audio, the PC's music (music.sh)
#   "Top Gear Rally Remastered.desktop"   the launcher
#   icon.png                    the icon the .desktop names
#
# Saves go to ~/.local/share/Top Gear Rally Remastered (Boss Rally's in its
# Boss Rally folder); the same folder can be moved anywhere, or BR_RESDIR,
# BR_CDROOT, BR_MUSICDIR and BR_SAVEDIR can point elsewhere.
#
# Usage: ports/remastered/package_linux.sh [--rom FILE] [--bin FILE] [--no-build]
#   --rom FILE  Top Gear Rally's ROM (default: $TGR_ROM, else reference/tgrally/Top Gear Rally (USA).z64)
#   --bin FILE  Boss Rally's disc image, when its data track or its CD audio is not
#               already extracted (default reference/brally/BossRally.BIN; the .cue beside it)
#   env: RENDER vulkan (default) | gl (a PC without Vulkan)
set -e
cd "$(dirname "$0")/../.."
BUILD=1
BIN=reference/brally/BossRally.BIN
while [ $# -gt 0 ]; do
    case $1 in
        --rom) TGR_ROM=$2; export TGR_ROM; shift 2 ;;
        --bin) BIN=$2; shift 2 ;;
        --no-build) BUILD=0; shift ;;
        *) echo "package_linux: unknown argument $1" >&2; exit 2 ;;
    esac
done
PY=.venv/bin/python
[ -x $PY ] || PY=python3

RENDER=${RENDER:-vulkan}
OUT=build/remastered/linux-$RENDER
DIR="$OUT/Top Gear Rally Remastered"
if [ $BUILD = 1 ]; then
    OUT=$OUT HOST=linux RENDER=$RENDER GFLAG=-g0 ports/remastered/link.sh
fi
[ -x $OUT/remastered ] || { echo "package_linux: no build at $OUT/remastered" >&2; exit 1; }

# ---- Boss Rally's data track: the tree's, else the disc image's ---------------------
DISC=reference/brally/data/disc
if [ ! -f $DISC/BRGlide.dll ]; then
    DISC=build/brally/macos-metal/extract/disc
    if [ ! -f $DISC/BRGlide.dll ]; then
        [ -f "$BIN" ] || { echo "package_linux: no Boss Rally disc image at $BIN" >&2; exit 1; }
        mkdir -p $DISC
        $PY tools/brally/extract_disc.py "$BIN" $DISC
    fi
fi

rm -rf "$DIR"
mkdir -p "$DIR"
cp $OUT/remastered "$DIR/remastered"
rsync -a --exclude 'brally.avi' --exclude '*.cab' --exclude '*.exe' --exclude '*.EXE' \
    --exclude '_INST32I.EX_' --exclude '_SETUP.DLL' --exclude 'dsetup*.dll' --exclude 'Msvcrt.dll' \
    --exclude 'BRD3D.dll' --exclude 'SETUP.INI' --exclude 'setup.*' --exclude 'Setup.BMP' \
    --exclude 'Autorun.inf' --exclude '*.url' --exclude 'layout.bin' \
    $DISC/ "$DIR/disc/"

# ---- the PC's music: Boss Rally's CD audio (host_music_dir: music/cd) ----------------
BIN=$BIN ports/remastered/music.sh "$DIR/music/cd"

# ---- the shelved lighting's skies and textures' sets: only with RM_SHELVED_ASSETS=1 ----
if [ "${RM_SHELVED_ASSETS:-0}" = 1 ]; then
    if [ -f ports/common/skies/skies.txt ]; then
        mkdir -p "$DIR/skies/ph8k"
        cp ports/common/skies/skies.txt "$DIR/skies/"
        grep -v '^#' ports/common/skies/skies.txt | awk 'NF >= 3 { print $3 }' | sort -u | while read id; do
            if [ -f "ports/common/skies/ph8k/${id}_8k.hdr" ]; then
                cp "ports/common/skies/ph8k/${id}_8k.hdr" "$DIR/skies/ph8k/"
            else
                echo "package_linux: sky $id missing (ports/common/skies/fetch.sh)" >&2
            fi
        done
    else
        echo "package_linux: no ports/common/skies/skies.txt: the Remastered lighting falls back to a grey day" >&2
    fi
    mkdir -p "$DIR/surfaces/baked"
    cp ports/common/surfaces/surfaces.txt "$DIR/surfaces/"
    grep -v '^#' ports/common/surfaces/surfaces.txt | awk 'NF >= 2 { print $1 }' | while read id; do
        for m in a b; do
            if [ -f "ports/common/surfaces/baked/${id}_$m.raw" ]; then
                cp "ports/common/surfaces/baked/${id}_$m.raw" "$DIR/surfaces/baked/"
            else
                echo "package_linux: surface set $id missing (ports/common/surfaces/fetch.py, bake.py): Textures off" >&2
            fi
        done
    done
fi

# ---- the launcher and the icon ------------------------------------------------------
cp ports/remastered/icon.png "$DIR/icon.png"
cat > "$DIR/Top Gear Rally Remastered.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Top Gear Rally Remastered
Comment=Top Gear Rally's menus, Boss Rally's engine: the Remastered races
Exec=remastered
Icon=icon
Terminal=false
Categories=Game;Racing;
EOF
echo "packaged $DIR"
