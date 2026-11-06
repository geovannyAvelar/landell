#!/usr/bin/env bash
# Build a static Skia (raster only, no GL, no fonts) for landell-gui.
# Usage: SKIA_DIR=~/skia scripts/build-skia.sh
set -euo pipefail

SKIA_BRANCH=chrome/m158
SKIA_DIR=${SKIA_DIR:-$HOME/skia}
DEPOT_DIR=${DEPOT_DIR:-$HOME/depot_tools}

if [ ! -d "$DEPOT_DIR" ]; then
  git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git "$DEPOT_DIR"
fi
export PATH="$DEPOT_DIR:$PATH"

if [ ! -d "$SKIA_DIR" ]; then
  git clone --depth 1 --branch "$SKIA_BRANCH" https://skia.googlesource.com/skia.git "$SKIA_DIR"
fi
cd "$SKIA_DIR"

python3 tools/git-sync-deps

bin/gn gen out/Static --args='
  is_official_build=true
  is_component_build=false
  skia_use_gl=false
  skia_use_system_expat=false
  skia_use_system_harfbuzz=false
  skia_use_system_icu=false
  skia_use_system_libjpeg_turbo=false
  skia_use_system_libpng=false
  skia_use_system_libwebp=false
  skia_use_system_zlib=false
  skia_use_freetype=false
  skia_use_fontconfig=false
  skia_use_harfbuzz=false
  skia_enable_fontmgr_empty=true
  skia_enable_pdf=false
  skia_enable_skshaper=false
'
"$(command -v -p ninja || echo /usr/bin/ninja)" -C out/Static skia
echo "libskia: $SKIA_DIR/out/Static/libskia.a"
