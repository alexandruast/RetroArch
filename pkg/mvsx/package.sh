#!/bin/bash
# Assemble the MVSX update package from the build outputs.
# Usage: package.sh <output dir>
set -e
# Workspace directory: contains RetroArch-1.22.2-mvsx/, _tools/ and
# retroarch-mvsx/. Found from this script's location (works for both the
# _tools/pkg/ copy and the pkg/mvsx/ copy inside the repository); MVSX_ROOT
# overrides it.
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -n "$MVSX_ROOT" ]; then ROOT=$MVSX_ROOT
elif [ -d "$HERE/../../_tools" ]; then ROOT=$(cd "$HERE/../.." && pwd)
elif [ -d "$HERE/../../../_tools" ]; then ROOT=$(cd "$HERE/../../.." && pwd)
else echo "set MVSX_ROOT to the workspace directory" >&2; exit 1; fi
SRC=$ROOT/RetroArch-1.22.2-mvsx
TC=$ROOT/_tools/arm-gnu
SYSLIB=$TC/arm-none-linux-gnueabihf/libc/lib
GXXLIB=$TC/arm-none-linux-gnueabihf/lib
OLD=$ROOT/retroarch-mvsx/lib
OUT=${1:?output dir}
STRIP=$TC/bin/arm-none-linux-gnueabihf-strip.exe

mkdir -p "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/filters/video" "$OUT/filters/audio" "$OUT/info"

# Main binary (stripped copy; keep the unstripped one next to the build)
cp "$SRC/retroarch" "$OUT/bin/retroarch"
$STRIP --strip-unneeded "$OUT/bin/retroarch"
chmod 755 "$OUT/bin/retroarch"

# New glibc runtime (must be replaced as a whole set)
for f in ld-linux-armhf.so.3 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 \
         librt.so.1 libresolv.so.2 libnss_files.so.2 libnss_dns.so.2 libutil.so.1 libanl.so.1; do
   cp "$SYSLIB/$f" "$OUT/lib/$f"
done
# C++ runtime from the same GCC
cp "$GXXLIB/libstdc++.so.6" "$GXXLIB/libgcc_s.so.1" "$OUT/lib/"
# Proven device libraries kept from the existing install (built against the
# older glibc, forward compatible with the new one)
for f in libSDL-1.2.so.0 libts.so.0 libz.so.1 libpng16.so.16 libfreetype.so.6 \
         libexpat.so.1 libfontconfig.so.1 libuuid.so.1 libudev.so.1 \
         libusb-1.0.so.0 libxkbcommon.so.0; do
   cp "$OLD/$f" "$OUT/lib/$f"
done
$STRIP --strip-unneeded "$OUT"/lib/*.so* 2>/dev/null || true

# Video / audio filter plugins
cp "$SRC"/gfx/video_filters/*.so "$SRC"/gfx/video_filters/*.filt "$OUT/filters/video/"
cp "$SRC"/libretro-common/audio/dsp_filters/*.so "$SRC"/libretro-common/audio/dsp_filters/*.dsp "$OUT/filters/audio/"
$STRIP --strip-unneeded "$OUT"/filters/video/*.so "$OUT"/filters/audio/*.so

# Launcher scripts and the autoconfig from the current install
mkdir -p "$OUT/autoconfig"
cp "$ROOT"/retroarch-mvsx/bin/{fbneo,gpsp,handy,neocd,nes,ngp,pcfx,pcsx,segacd} "$OUT/bin/"
# ... except for the framebuffer mode: same 1280x512 as boot-usb.sh, since
# retroarch.cfg uses the active mode and the 8:3 aspect assumes half height
sed -i 's/busybox fbset -fb \/dev\/fb0 -g 1280 1024 1280 1024 32/busybox fbset -fb \/dev\/fb0 -g 1280 512 1280 512 32/'    "$OUT"/bin/{fbneo,gpsp,handy,neocd,nes,ngp,pcfx,pcsx,segacd}
cp "$ROOT"/retroarch-mvsx/autoconfig/*.cfg "$OUT/autoconfig/"
# Boot script for the flash drive root (starts RetroArch with the menu)
cp "$ROOT/_tools/pkg/boot-usb.sh" "$OUT/boot-usb.sh"
chmod 755 "$OUT/boot-usb.sh"

cp "$ROOT/_tools/pkg/README-MVSX.md" "$OUT/README-MVSX.md"
cp "$ROOT/_tools/pkg/info/disable-2021-overrides.sh" "$OUT/info/disable-2021-overrides.sh"
cp "$ROOT/_tools/pkg/retroarch.cfg" "$OUT/retroarch.cfg"
cp "$ROOT/_tools/pkg/retroarch.cfg.mvsx-additions" "$OUT/retroarch.cfg.mvsx-additions"

echo "Package assembled in $OUT"
du -sh "$OUT"
