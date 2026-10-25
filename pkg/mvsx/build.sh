#!/bin/bash
# Builds RetroArch for the Neo Geo MVSX with the Arm GNU toolchain
# (arm-none-linux-gnueabihf, 14.2.Rel1 or compatible).
#
# Usage: pkg/mvsx/build.sh [extra make arguments, e.g. clean]
#   TOOLCHAIN_DIR  toolchain root (default: <workspace>/_tools/arm-gnu next
#                  to the repository, see Makefile.mvsx)
#   JOBS           parallel jobs (default: number of CPUs)
#
# On Windows the build runs under MSYS2 (expected in <workspace>/_tools/msys2
# or MSYS_BASH) with its path conversion disabled, because the Makefile bakes
# the device's /mnt/hdisk dynamic-linker path into the binary.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
MAKEVARS=${TOOLCHAIN_DIR:+TOOLCHAIN_DIR=$TOOLCHAIN_DIR}

case "$(uname -s)" in
   MINGW*|MSYS*|CYGWIN*)
      MSYS_BASH=${MSYS_BASH:-$SRC/../_tools/msys2/msys64/usr/bin/bash.exe}
      [ -x "$MSYS_BASH" ] || { echo "MSYS2 bash not found at $MSYS_BASH (set MSYS_BASH)" >&2; exit 1; }
      exec env MSYSTEM=MSYS "$MSYS_BASH" -lc "export MSYS2_ARG_CONV_EXCL='*'; cd '$SRC' && make -f Makefile.mvsx -j$JOBS $MAKEVARS $*"
      ;;
   *)
      cd "$SRC"
      exec make -f Makefile.mvsx -j"$JOBS" $MAKEVARS "$@"
      ;;
esac
