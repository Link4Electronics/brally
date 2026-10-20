#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."

ARCH=${ARCH:-$(uname -m)}
VERSION=${VERSION:-$(git describe --tags --always 2>/dev/null || echo 0)}
export ARCH VERSION

pacman -Syu --noconfirm --needed \
    alsa-lib binutils clang ffmpeg git libxkbcommon patchelf pkgconf \
    python rsync vulkan-headers vulkan-icd-loader wayland zlib
get-debloated-pkgs --add-common --prefer-nano

RENDER=vulkan ports/remastered/package_linux.sh \
    --rom "${TGR_ROM:-reference/tgrally/Top Gear Rally (USA).z64}" \
    --bin "${BIN:-reference/brally/BossRally.BIN}"

APP="build/remastered/linux-vulkan/Top Gear Rally Remastered"
[ -x "$APP/remastered" ] || { echo "make-appimage: no build at $APP" >&2; exit 1; }

rm -rf AppDir dist
mkdir -p AppDir/bin dist
cp -a "$APP"/. AppDir/bin/
rm -f AppDir/bin/*.desktop AppDir/bin/icon.png
cp ports/remastered/icon.png AppDir/remastered.png
cat > AppDir/remastered.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=Top Gear Rally Remastered
Comment=Top Gear Rally's menus with Boss Rally's racing engine
Exec=remastered
Icon=remastered
Terminal=false
Categories=Game;Racing;
EOF
printf 'LC_ALL=C.UTF-8\n' > AppDir/.env
printf '%s\n' "$VERSION" > dist/VERSION.txt

export OUTPATH="$PWD/dist"
export APPNAME=remastered
export DEPLOY_OPENGL=1
export DEPLOY_VULKAN=1
export ADD_HOOKS=self-updater.hook
if [ -n "${GITHUB_REPOSITORY:-}" ]; then
    export UPINFO="gh-releases-zsync|${GITHUB_REPOSITORY%/*}|${GITHUB_REPOSITORY#*/}|latest|*${ARCH}.AppImage.zsync"
fi

quick-sharun AppDir/bin/remastered
quick-sharun --make-appimage
