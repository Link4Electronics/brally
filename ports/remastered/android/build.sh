#!/bin/sh
# Build "Top Gear Rally Remastered.apk": link.sh with the Android host and the
# Vulkan renderer for each ABI, the game's files (Boss Rally's data track, its
# CD audio; Top Gear Rally's data from your ROM is built in), the icon, and
# Gradle.
#
#   env: ANDROID_NDK_HOME (or ANDROID_NDK_ROOT, or a ndk/ under the SDK)
#        ANDROID_HOME / ANDROID_SDK_ROOT (for Gradle)
#        TGR_ROM  Top Gear Rally's ROM (or --rom)
#        GRADLE   the gradle command (default: ./gradlew here, else gradle)
#        ABIS     the ABIs to build (default "arm64-v8a armeabi-v7a x86_64")
#        JAVA_HOME a JDK 17 for Gradle (AGP 8.5 needs it)
#
# Usage: ports/remastered/android/build.sh [--rom FILE] [--bin FILE] [--release] [--no-build]
#   --rom FILE   Top Gear Rally's ROM (default: $TGR_ROM, else reference/tgrally/Top Gear Rally (USA).z64)
#   --bin FILE   Boss Rally's disc image (default reference/brally/BossRally.BIN; the .cue beside it)
#   --release    Gradle assembleRelease (sign it with RM_KEYSTORE, or it is unsigned)
#   --no-build   use the libraries and data already staged
set -e
cd "$(dirname "$0")/../../.."
ROOT=$(pwd)
BUILD=1
RELEASE=0
BIN=reference/brally/BossRally.BIN
while [ $# -gt 0 ]; do
    case $1 in
        --rom) TGR_ROM=$2; export TGR_ROM; shift 2 ;;
        --bin) BIN=$2; shift 2 ;;
        --release) RELEASE=1; shift ;;
        --no-build) BUILD=0; shift ;;
        *) echo "build: unknown argument $1" >&2; exit 2 ;;
    esac
done
ABIS=${ABIS:-"arm64-v8a armeabi-v7a x86_64"}
APP=ports/remastered/android/app
OUT=build/remastered/android-vulkan
PY=.venv/bin/python
[ -x $PY ] || PY=python3

: "${TGR_ROM:=reference/tgrally/Top Gear Rally (USA).z64}"
: "${ROMDATA:=embed}"
export TGR_ROM ROMDATA

if [ $BUILD = 1 ]; then
    # ---- the NDK ----
    NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
    if [ -z "$NDK" ]; then
        for d in "${ANDROID_HOME:-}/ndk" "${ANDROID_SDK_ROOT:-}/ndk" "$HOME/Android/Sdk/ndk" /opt/android-sdk/ndk; do
            if [ -n "$d" ] && [ -d "$d" ]; then
                NDK=$(ls -d "$d"/* 2>/dev/null | sort -V | tail -1)
                [ -n "$NDK" ] && break
            fi
        done
    fi
    [ -n "$NDK" ] && [ -d "$NDK" ] || {
        echo "build: no Android NDK (set ANDROID_NDK_HOME to the NDK's folder)" >&2; exit 1; }
    case $(uname -s) in
        Darwin) TAG=darwin-x86_64 ;;
        *)      TAG=linux-x86_64 ;;
    esac
    API=28                              # minSdk: posix_spawn, AAudio
    if [ -f "$TGR_ROM" ]; then :; else
        echo "build: no Top Gear Rally ROM at $TGR_ROM (--rom FILE, or TGR_ROM=FILE)" >&2; exit 1
    fi
    if [ ! -f reference/brally/orig/BRGlide.dll ]; then
        echo "build: no reference/brally/orig/BRGlide.dll (Boss Rally's data is built in)" >&2
    fi

    for abi in $ABIS; do
        case $abi in
            arm64-v8a)   TRIPLE=aarch64-linux-android ;;
            armeabi-v7a) TRIPLE=armv7a-linux-androideabi ;;
            x86_64)      TRIPLE=x86_64-linux-android ;;
            *) echo "build: unknown ABI $abi" >&2; exit 2 ;;
        esac
        CC=$NDK/toolchains/llvm/prebuilt/$TAG/bin/${TRIPLE}${API}-clang
        [ -x "$CC" ] || { echo "build: no compiler $CC" >&2; exit 1; }
        echo "build: $abi ($TRIPLE$API)"
        ABIOUT=$OUT/$abi
        OUT=$ABIOUT HOST=android RENDER=vulkan GFLAG=-g0 CC=$CC ports/remastered/link.sh
        mkdir -p $APP/src/main/jniLibs/$abi
        cp $ABIOUT/libremastered.so $APP/src/main/jniLibs/$abi/
    done

    # ---- the game's files: Boss Rally's data track less installer and video,
    #      its CD audio, and the guide's version stamp ----
    A=$APP/src/main/assets
    rm -rf "$A/data"
    mkdir -p "$A/data/disc"
    DISC=
    for d in reference/brally/data/disc build/brally/macos-metal/extract/disc build/remastered/extract/disc; do
        [ -f "$d/BRGlide.dll" ] && DISC=$d && break
    done
    if [ -z "$DISC" ]; then
        [ -f "$BIN" ] || { echo "build: no Boss Rally disc image at $BIN" >&2; exit 1; }
        DISC=build/remastered/extract/disc
        mkdir -p "$DISC"
        $PY tools/brally/extract_disc.py "$BIN" "$DISC"
    fi
    ( cd "$DISC" && tar cf - \
        --exclude 'brally.avi' --exclude '*.cab' --exclude '*.exe' --exclude '*.EXE' \
        --exclude '_INST32I.EX_' --exclude '_SETUP.DLL' --exclude 'dsetup*.dll' --exclude 'Msvcrt.dll' \
        --exclude 'BRD3D.dll' --exclude 'SETUP.INI' --exclude 'setup.*' --exclude 'Setup.BMP' \
        --exclude 'Autorun.inf' --exclude '*.url' --exclude 'layout.bin' . ) |
        ( cd "$A/data/disc" && tar xf - )
    BIN=$BIN ports/remastered/music.sh "$A/data/music/cd"
    if [ "$ROMDATA" = file ]; then
        if [ -f build/tgrally/linux-vulkan/assets/romdata.bin ]; then
            cp build/tgrally/linux-vulkan/assets/romdata.bin "$A/data/romdata.bin"
        else
            echo "build: ROMDATA=file but no romdata.bin (build the tgrally port once with your ROM)" >&2
            exit 1
        fi
    fi
    git rev-parse --short HEAD 2>/dev/null > "$A/data/version.txt" || date +%s > "$A/data/version.txt"
    cp ports/remastered/icon.png $APP/src/main/res/drawable/icon.png
fi

# ---- Gradle ----
GRADLE=${GRADLE:-}
if [ -z "$GRADLE" ]; then
    if [ -x ports/remastered/android/gradlew ]; then
        GRADLE=$ROOT/ports/remastered/android/gradlew
    else
        GRADLE=gradle
    fi
fi
command -v "$GRADLE" >/dev/null 2>&1 || [ -x "$GRADLE" ] ||
    { echo "build: no gradle (install Gradle 8.7+, set GRADLE, or put a wrapper here)" >&2; exit 1; }
if [ $RELEASE = 1 ]; then TASK=assembleRelease; else TASK=assembleDebug; fi
( cd ports/remastered/android && "$GRADLE" --no-daemon "$TASK" )
if [ $RELEASE = 1 ]; then
    APK=ports/remastered/android/app/build/outputs/apk/release/app-release-unsigned.apk
    [ -f "$APK" ] || APK=ports/remastered/android/app/build/outputs/apk/release/app-release.apk
    OUTAPK=$OUT/TopGearRallyRemastered-$TASK.apk
else
    APK=ports/remastered/android/app/build/outputs/apk/debug/app-debug.apk
    OUTAPK=$OUT/TopGearRallyRemastered.apk
fi
[ -f "$APK" ] || { echo "build: Gradle made no APK at $APK" >&2; exit 1; }
cp "$APK" "$OUTAPK"
echo "packaged $OUTAPK"
