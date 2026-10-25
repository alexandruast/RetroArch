#!/bin/bash
# Cross-compiles the CPU video filters and audio DSP plugins for the MVSX.
# Usage (from Git Bash on the development PC): bash _tools/pkg/build-filters.sh
# Workspace directory: contains RetroArch-1.22.2-mvsx/, _tools/ and
# retroarch-mvsx/. Found from this script's location (works for both the
# _tools/pkg/ copy and the pkg/mvsx/ copy inside the repository); MVSX_ROOT
# overrides it.
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -n "$MVSX_ROOT" ]; then ROOT=$MVSX_ROOT
elif [ -d "$HERE/../../_tools" ]; then ROOT=$(cd "$HERE/../.." && pwd)
elif [ -d "$HERE/../../../_tools" ]; then ROOT=$(cd "$HERE/../../.." && pwd)
else echo "set MVSX_ROOT to the workspace directory" >&2; exit 1; fi
GCC=$ROOT/_tools/arm-gnu/bin/arm-none-linux-gnueabihf-gcc
FLAGS="-mcpu=cortex-a5 -mtune=cortex-a5 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2"
MSYSTEM=MSYS "$ROOT/_tools/msys2/msys64/usr/bin/bash.exe" -lc "export MSYS2_ARG_CONV_EXCL='*'; cd $ROOT/RetroArch-1.22.2-mvsx && make -C gfx/video_filters platform=unix compiler=$GCC extra_flags=\"$FLAGS\" clean build 2>&1 | grep -i 'error' ; make -C libretro-common/audio/dsp_filters platform=unix compiler=$GCC extra_flags=\"$FLAGS\" clean build 2>&1 | grep -i 'error'; ls gfx/video_filters/*.so libretro-common/audio/dsp_filters/*.so | wc -l"
