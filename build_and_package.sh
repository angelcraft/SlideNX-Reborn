#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TITLE_ID="420000000000000F"
DIST="$ROOT/dist"

if [[ -z "${DEVKITPRO:-}" || ! -f "$DEVKITPRO/libnx/switch_rules" ]]; then
  echo "ERROR: DEVKITPRO/libnx/switch_rules not found." >&2
  echo "Open the devkitPro MSYS2/terminal environment and install switch-dev + switch-mpg123." >&2
  exit 2
fi

make -C "$ROOT" clean
make -C "$ROOT" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

if [[ ! -f "$ROOT/slidenx.nsp" ]]; then
  echo "ERROR: build completed without slidenx.nsp; see the compiler output above." >&2
  exit 3
fi

rm -rf "$DIST"
mkdir -p "$DIST/atmosphere/contents/$TITLE_ID/flags" "$DIST/SlideNX/sounds"
cp "$ROOT/slidenx.nsp" "$DIST/atmosphere/contents/$TITLE_ID/exefs.nsp"
: > "$DIST/atmosphere/contents/$TITLE_ID/flags/boot2.flag"
cp "$ROOT/config.ini" "$DIST/SlideNX/config.ini"
cat > "$DIST/SlideNX/sounds/README.txt" <<'TXT'
Place your own MP3 files here.
For fixed sounds, use attach.mp3 and detach.mp3.
For random mode, set random=true in ../config.ini.
TXT
cp "$ROOT/README.md" "$DIST/README-SlideNX-Reborn.txt"

echo
echo "Build/package created under: $DIST"
echo "Copy the contents of dist/ to the root of the Switch SD card."
echo "This is an untested candidate build; verify on emuMMC first if available."
