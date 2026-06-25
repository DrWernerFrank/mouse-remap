#!/usr/bin/env bash
# Build Mouse_Remap-x86_64.AppImage from source.
# Requires: a C compiler and appimagetool (auto-downloaded if missing).
set -euo pipefail
cd "$(dirname "$0")"

ARCH=x86_64
OUT="Mouse_Remap-${ARCH}.AppImage"

echo "==> Compiling engine"
cc -O2 -Wall -o mouse-remap mouse-remap.c

echo "==> Assembling AppDir"
rm -rf AppDir
mkdir -p AppDir/usr/bin/profiles
cp mouse-remap            AppDir/usr/bin/mouse-remap
cp mouse-remap-gui.py     AppDir/usr/bin/mouse-remap-gui.py
cp profiles/*.json        AppDir/usr/bin/profiles/
cp mouse-remap.png        AppDir/mouse-remap.png
cp mouse-remap.png        AppDir/.DirIcon

cat > AppDir/AppRun <<'SH'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export MOUSE_REMAP_BIN="$HERE/usr/bin/mouse-remap"
if ! command -v python3 >/dev/null 2>&1; then
    echo "mouse-remap needs python3 (with tkinter) installed." >&2
    exit 1
fi
exec python3 "$HERE/usr/bin/mouse-remap-gui.py" "$@"
SH
chmod +x AppDir/AppRun

cp mouse-remap.desktop AppDir/mouse-remap.desktop

echo "==> Fetching appimagetool if needed"
if [ ! -x ./appimagetool ]; then
  wget -q "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" -O appimagetool
  chmod +x appimagetool
fi

echo "==> Building $OUT"
ARCH=$ARCH ./appimagetool AppDir "$OUT" || {
  ./appimagetool --appimage-extract >/dev/null
  ARCH=$ARCH ./squashfs-root/AppRun AppDir "$OUT"
}

echo "==> Done: $OUT"
