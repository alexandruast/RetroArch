# Third-party files bundled with the MVSX package

The package assembled by `package.sh` contains, besides the RetroArch binary
and plugins built from this repository (GNU GPLv3, see `COPYING`):

## From the Arm GNU Toolchain 14.2.Rel1 (`lib/`)

`ld-linux-armhf.so.3`, `libc.so.6`, `libm.so.6`, `libpthread.so.0`,
`libdl.so.2`, `librt.so.1`, `libresolv.so.2`, `libnss_files.so.2`,
`libnss_dns.so.2`, `libutil.so.1`, `libanl.so.1` — the GNU C Library 2.40,
LGPL 2.1; `libstdc++.so.6`, `libgcc_s.so.1` — GCC 14.2 runtime, GPLv3 with
the GCC Runtime Library Exception. Sources: https://www.gnu.org/software/libc/
and https://gcc.gnu.org/; the toolchain is at
https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads.

## From the cabinet's previous RetroArch install (`pkg/mvsx/device-lib/`, shipped in `lib/`)

Binaries built for the MVSX by the 2021 acmeplus package, kept because the
new build is linked against this exact SDL and the rest are their
dependencies on the device:

| file | project | license |
|------|---------|---------|
| `libSDL-1.2.so.0` | SDL 1.2 (https://www.libsdl.org) | LGPL 2.1 |
| `libts.so.0` | tslib (https://github.com/libts/tslib) | LGPL 2.1 |
| `libz.so.1` | zlib | zlib |
| `libpng16.so.16` | libpng | libpng |
| `libfreetype.so.6` | FreeType | FTL / GPLv2 |
| `libexpat.so.1` | Expat | MIT |
| `libfontconfig.so.1` | fontconfig | MIT-style |
| `libuuid.so.1` | util-linux | BSD-3 |
| `libudev.so.1` | systemd/eudev | LGPL 2.1 |
| `libusb-1.0.so.0` | libusb | LGPL 2.1 |
| `libxkbcommon.so.0` | xkbcommon | MIT |

## SDL 1.2 headers (`pkg/mvsx/sdl12-include/`)

The SDL 1.2.15 public headers with an `SDL_config.h` for this target, used
only to compile the driver; LGPL 2.1.

## Scripts and configuration

`launchers/*`, `autoconfig/*.cfg` and `retroarch.cfg` originate from the
cabinet's 2021 install and are included for a working out-of-the-box setup.
