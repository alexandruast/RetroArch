# RetroArch for the Neo Geo MVSX

[![MVSX build](https://github.com/alexandruast/RetroArch/actions/workflows/MVSX.yml/badge.svg?branch=mvsx-1.22.2)](https://github.com/alexandruast/RetroArch/actions/workflows/MVSX.yml)

A port of [RetroArch](https://github.com/libretro/RetroArch) v1.22.2 to the
**SNK Neo Geo MVSX** arcade cabinet (Unico, 2020): a glibc ARM binary that
runs on the cabinet's Android firmware from a USB flash drive, drawing
straight into the framebuffer. It replaces the acmeplus 1.9.0 "asp_mvsx"
build from 2021, keeps its layout and launch method, and runs the same
cores, saves and settings.

This branch (`mvsx-1.22.2`) is upstream `v1.22.2` plus one MVSX commit; the
upstream README is at
[libretro/RetroArch](https://github.com/libretro/RetroArch#readme).

## What you get

- RetroArch 1.22.2 with the RGUI menu, CPU video/audio filters, rewind,
  run-ahead, save states, cheats, soft-patching, 7-Zip/ZIP/CHD content,
  playlists, the libretro database and netplay (no SSL, so no online updater
  or achievements; the libretro buildbot is HTTPS-only).
- A video driver written for this hardware (`sdl_mvsx`): renders directly
  into `/dev/fb0`, correct colours on the cabinet's BGR panel, NEON
  scalers, and a CRT look (scanlines, bright-line boost, aperture mask,
  composite-style blur) done while scaling at almost no cost. All games the
  cabinet ran at 21 fps with the original setup now run at a steady 60 fps.
- Built-in diagnostics: the RetroArch log reports the CPU and framebuffer
  details and, every 10 seconds, frame timings and per-thread CPU use.
- Audio through the Actions OSS device with the driver's quirks handled.

Hardware facts and every decision's measurement are in
[`pkg/mvsx/README-MVSX.md`](pkg/mvsx/README-MVSX.md).

## Install

1. Download the latest `retroarch-*.zip` from the
   [Releases](https://github.com/alexandruast/RetroArch/releases) page and
   unpack it.
2. On the cabinet's flash drive (the one with `/mnt/hdisk` content:
   `retroarch/`, `cores/`, `roms/`), back up `retroarch/bin`,
   `retroarch/lib`, `retroarch/retroarch.cfg` and `boot-usb.sh`.
3. Copy `bin/`, `lib/`, `filters/` and `autoconfig/` from the package into
   `retroarch/` (replacing `lib/` as a whole), `retroarch.cfg` into
   `retroarch/`, and `boot-usb.sh` to the root of the drive.
4. Boot the cabinet. The menu opens with Start+Select; the log is
   `retroarch/retroarch.log`.

The step-by-step version, the rollback, and what the config changes are in
[`pkg/mvsx/README-MVSX.md`](pkg/mvsx/README-MVSX.md#install). Cores are not
part of the package: the cabinet's existing `cores/` keep working.

## Tuning the picture

Everything is set in `boot-usb.sh` on the drive, no rebuild needed:

| line | what it does |
|------|--------------|
| `busybox fbset ... 1280 512` | the framebuffer mode: 1280x512, doubled vertically by the display engine, is the fast mode that still looks like native; 1280x1024 also runs at 60 fps on 8/16-bit systems but leaves no headroom |
| `MVSX_SCANLINES=65` | dark-line brightness in percent; `0` = none |
| `MVSX_SCANLINE_BOOST=115` | bright lines multiplied by this percentage |
| `MVSX_MASK=85` | aperture mask on every third column of the dark lines |
| `MVSX_HBLUR=1` | horizontal blur: `1` light, `2` medium, `3`-`4` passes, `0` off |
| `scaling_max_freq` | the CPU clock cap lifted to the SoC's 1.31 GHz |

With the half-height mode the aspect ratio in `retroarch.cfg` is 8:3
(`aspect_ratio_index = "14"`), which is 4:3 on the panel; use 4:3 (`0`) at
1280x1024.

## Building

The GitHub Actions workflow ([`.github/workflows/MVSX.yml`](.github/workflows/MVSX.yml))
builds every push to an `mvsx-*` branch (download the zip from the run's
artifacts) and publishes a release for tags named `v1.22.2-mvsx.N`.

Locally, on Linux or on Windows with MSYS2, with the
[Arm GNU Toolchain 14.2.Rel1](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
for `arm-none-linux-gnueabihf`:

```
TOOLCHAIN_DIR=/path/to/arm-gnu-toolchain pkg/mvsx/build.sh
TOOLCHAIN_DIR=/path/to/arm-gnu-toolchain pkg/mvsx/build-filters.sh
TOOLCHAIN_DIR=/path/to/arm-gnu-toolchain pkg/mvsx/package.sh out/retroarch-mvsx zip
```

The toolchain's glibc 2.40 ships inside the package (the cabinet's own glibc
is older), which is why the dynamic linker path is baked into the binary.
Details, the development history with all measurements and the open items
are in [`pkg/mvsx/DEVELOPMENT-NOTES.md`](pkg/mvsx/DEVELOPMENT-NOTES.md).

## Credits

- [libretro/RetroArch](https://github.com/libretro/RetroArch), the thing
  itself.
- [acmeplus](https://github.com/acmeplus), whose 2021 MVSX build and 2024
  "Sauron Key" work (the RS-90 driver and the NEON upscalers from the Miyoo
  Mini port) this port started from, and whose device libraries it still
  ships.

## License

RetroArch is released under the GNU GPLv3 (see [COPYING](COPYING)). The
third-party binaries bundled in the package are listed with their licenses in
[`pkg/mvsx/NOTICE.md`](pkg/mvsx/NOTICE.md).
