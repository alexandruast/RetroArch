#!/bin/bash
# Cross-compiles the CPU video filter and audio DSP plugins for the MVSX with
# the same toolchain and CPU flags as the main binary.
#
# Usage: pkg/mvsx/build-filters.sh
#   TOOLCHAIN_DIR  toolchain root (default: <workspace>/_tools/arm-gnu)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
TC=${TOOLCHAIN_DIR:-$SRC/../_tools/arm-gnu}
FLAGS="-mcpu=cortex-a5 -mtune=cortex-a5 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2"

run_make() {
   make -C "$1" platform=unix compiler="$2" extra_flags="$FLAGS" clean build
}

case "$(uname -s)" in
   MINGW*|MSYS*|CYGWIN*)
      MSYS_BASH=${MSYS_BASH:-$SRC/../_tools/msys2/msys64/usr/bin/bash.exe}
      [ -x "$MSYS_BASH" ] || { echo "MSYS2 bash not found at $MSYS_BASH (set MSYS_BASH)" >&2; exit 1; }
      GCC=$(cygpath -m "$TC" 2>/dev/null || echo "$TC")/bin/arm-none-linux-gnueabihf-gcc
      exec env MSYSTEM=MSYS "$MSYS_BASH" -lc "export MSYS2_ARG_CONV_EXCL='*'; cd '$SRC' \
         && make -C gfx/video_filters platform=unix compiler='$GCC' extra_flags='$FLAGS' clean build \
         && make -C libretro-common/audio/dsp_filters platform=unix compiler='$GCC' extra_flags='$FLAGS' clean build"
      ;;
   *)
      GCC=$TC/bin/arm-none-linux-gnueabihf-gcc
      [ -x "$GCC" ] || { echo "toolchain not found at $TC (set TOOLCHAIN_DIR)" >&2; exit 1; }
      cd "$SRC"
      run_make gfx/video_filters "$GCC"
      run_make libretro-common/audio/dsp_filters "$GCC"
      ;;
esac
