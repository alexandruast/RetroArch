#!/bin/bash
# Assembles the MVSX package (the directory tree that goes on the cabinet's
# flash drive) from a finished build of this repository.
#
# Usage: pkg/mvsx/package.sh <output dir> [zip]
#   TOOLCHAIN_DIR  Arm GNU toolchain (arm-none-linux-gnueabihf) that built the
#                  binary; its glibc runtime goes into the package. Default:
#                  <workspace>/_tools/arm-gnu next to the repository.
#   zip            also write <output dir>.zip next to the output directory.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
TC=${TOOLCHAIN_DIR:-$SRC/../_tools/arm-gnu}
OUT=${1:?usage: package.sh <output dir> [zip]}
SYSLIB=$TC/arm-none-linux-gnueabihf/libc/lib
GXXLIB=$TC/arm-none-linux-gnueabihf/lib
STRIP=$TC/bin/arm-none-linux-gnueabihf-strip
[ -x "$STRIP" ] || STRIP=$STRIP.exe
[ -x "$STRIP" ] || { echo "toolchain not found at $TC (set TOOLCHAIN_DIR)" >&2; exit 1; }
[ -f "$SRC/retroarch" ] || { echo "no $SRC/retroarch: run pkg/mvsx/build.sh first" >&2; exit 1; }
ls "$SRC"/gfx/video_filters/*.so >/dev/null 2>&1 || { echo "no filter plugins: run pkg/mvsx/build-filters.sh first" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/filters/video" "$OUT/filters/audio" "$OUT/info" "$OUT/autoconfig"

# Main binary (stripped copy; the unstripped one stays next to the build)
cp "$SRC/retroarch" "$OUT/bin/retroarch"
"$STRIP" --strip-unneeded "$OUT/bin/retroarch"
chmod 755 "$OUT/bin/retroarch"

# glibc runtime of the toolchain that built the binary (must be replaced as a
# whole set on the device) and the matching C++ runtime
for f in ld-linux-armhf.so.3 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 \
         librt.so.1 libresolv.so.2 libnss_files.so.2 libnss_dns.so.2 libutil.so.1 libanl.so.1; do
   cp "$SYSLIB/$f" "$OUT/lib/$f"
done
cp "$GXXLIB/libstdc++.so.6" "$GXXLIB/libgcc_s.so.1" "$OUT/lib/"
# Device libraries (SDL 1.2, tslib, zlib, libpng, ...) from the cabinet's
# previous install; see pkg/mvsx/NOTICE.md
cp "$HERE"/device-lib/*.so* "$OUT/lib/"
"$STRIP" --strip-unneeded "$OUT"/lib/*.so* 2>/dev/null || true

# Video / audio filter plugins
cp "$SRC"/gfx/video_filters/*.so "$SRC"/gfx/video_filters/*.filt "$OUT/filters/video/"
cp "$SRC"/libretro-common/audio/dsp_filters/*.so "$SRC"/libretro-common/audio/dsp_filters/*.dsp "$OUT/filters/audio/"
"$STRIP" --strip-unneeded "$OUT"/filters/video/*.so "$OUT"/filters/audio/*.so

# Launcher scripts, autoconfig, config, boot script, docs
cp "$HERE"/launchers/* "$OUT/bin/"
chmod 755 "$OUT"/bin/*
cp "$HERE"/autoconfig/*.cfg "$OUT/autoconfig/"
cp "$HERE/retroarch.cfg" "$OUT/retroarch.cfg"
cp "$HERE/retroarch.cfg.mvsx-additions" "$OUT/retroarch.cfg.mvsx-additions"
cp "$HERE/boot-usb.sh" "$OUT/boot-usb.sh"
chmod 755 "$OUT/boot-usb.sh"
cp "$HERE/README-MVSX.md" "$OUT/README-MVSX.md"
cp "$HERE/NOTICE.md" "$OUT/info/NOTICE.md"
cp "$HERE/disable-2021-overrides.sh" "$OUT/info/disable-2021-overrides.sh"
# Complete source patch against upstream v1.22.2 (when the tag is available)
(cd "$SRC" && git diff v1.22.2 -- . ':!retroarch' > "$OUT/info/retroarch-1.22.2-mvsx.patch" 2>/dev/null) || \
   rm -f "$OUT/info/retroarch-1.22.2-mvsx.patch"

echo "Package assembled in $OUT"
du -sh "$OUT"

if [ "$2" = "zip" ]; then
   name=$(basename "$OUT")
   ( cd "$(dirname "$OUT")" && rm -f "$name.zip" && \
     if command -v zip >/dev/null 2>&1; then zip -q -r -9 "$name.zip" "$name"; \
     elif command -v bsdtar >/dev/null 2>&1; then bsdtar -a -cf "$name.zip" "$name"; \
     else python -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', '.', sys.argv[1])" "$name"; fi )
   echo "Zip: $(dirname "$OUT")/$name.zip"
fi
