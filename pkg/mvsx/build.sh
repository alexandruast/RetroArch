#!/bin/bash
# Builds RetroArch for the MVSX with the MSYS2 make and the Arm GNU toolchain.
# Usage (from Git Bash on the development PC): bash _tools/pkg/build.sh
# Workspace directory: contains RetroArch-1.22.2-mvsx/, _tools/ and
# retroarch-mvsx/. Found from this script's location (works for both the
# _tools/pkg/ copy and the pkg/mvsx/ copy inside the repository); MVSX_ROOT
# overrides it.
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -n "$MVSX_ROOT" ]; then ROOT=$MVSX_ROOT
elif [ -d "$HERE/../../_tools" ]; then ROOT=$(cd "$HERE/../.." && pwd)
elif [ -d "$HERE/../../../_tools" ]; then ROOT=$(cd "$HERE/../../.." && pwd)
else echo "set MVSX_ROOT to the workspace directory" >&2; exit 1; fi
MSYSTEM=MSYS "$ROOT/_tools/msys2/msys64/usr/bin/bash.exe" -lc "export MSYS2_ARG_CONV_EXCL='*'; cd $ROOT/RetroArch-1.22.2-mvsx && make -f Makefile.mvsx -j16 2>&1 | grep -i -B2 -A6 ' error\|error:' | head -40; exit \${PIPESTATUS[0]}"
