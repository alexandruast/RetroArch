# RetroArch 1.22.2 for the Neo Geo MVSX (Android-hosted glibc build)

This package updates the RetroArch that lives in `/mnt/hdisk/retroarch` on
the MVSX (the acmeplus 1.9.0 "asp_mvsx" build from February 2021) to
upstream RetroArch v1.22.2, built from a git worktree of
`libretro/RetroArch` at tag `v1.22.2` plus a small MVSX patch set
(branch `mvsx-1.22.2` in the `RetroArch-libretro` clone).

It keeps the same layout and launch method (`boot-usb.sh` at the root of
the flash drive starts RetroArch with its menu; the `bin/<core>` scripts
start one core directly), so cores, BIOS, saves and autoconfig keep
working.

## What is in the package

| Path | Content |
|------|---------|
| `boot-usb.sh` | the flash-drive boot script: sets the 1280x512 framebuffer mode, lifts the CPU clock cap (1.13 -> 1.31 GHz) and starts RetroArch (menu) |
| `retroarch.cfg` | the cabinet's working config (see Install) |
| `bin/retroarch` | RetroArch 1.22.2, ARMv7 hard-float, NEON, stripped |
| `bin/<core>` | the existing launcher scripts; only the `fbset` line changed to the same 1280x512 mode |
| `lib/` | glibc 2.40 runtime (from the Arm GNU 14.2 toolchain), GCC 14 `libstdc++`/`libgcc_s`, and the proven device libraries from the old install (`libSDL-1.2.so.0`, `libts.so.0`, `libz`, `libpng16`, `libfreetype`, ...) |
| `filters/video/*.so, *.filt` | 27 CPU video filters (Scanline2x, Blargg NTSC, LQ2x, 2xBR, Phosphor2x, Grid, Dot Matrix, Gameboy, ...); Scanline2x is rebuilt to use all four cores (upstream forces one thread) |
| `filters/audio/*.so, *.dsp` | 11 audio DSP filters (EQ, Reverb, Echo, Bass boost, ...) |
| `autoconfig/` | the existing autoconfig file(s), unchanged |
| `info/retroarch-1.22.2-mvsx.patch` | the complete source patch against upstream `v1.22.2` |

The binary's dynamic linker path is baked in as
`/mnt/hdisk/retroarch/lib/ld-linux-armhf.so.3` and its rpath is
`/mnt/hdisk/retroarch/lib`, exactly like the old binary, so it works with
the existing launcher scripts.

## Install

1. Back up the current `/mnt/hdisk/retroarch/bin` and
   `/mnt/hdisk/retroarch/lib` directories (for example rename them to
   `bin.old` and `lib.old`).
2. Copy `bin/`, `lib/` and `filters/` from this package into
   `/mnt/hdisk/retroarch/` (overwriting). `lib/` must be replaced as a
   whole: the old `libc.so.6`, `libpthread.so.0`, `libdl.so.2`,
   `librt.so.1`, `libm.so.6`, `ld-linux-armhf.so.3`, `libstdc++.so.6`
   and `libgcc_s.so.1` must not stay next to the new ones.
3. Make sure `bin/retroarch` is executable (`chmod 755`).
4. Replace `/mnt/hdisk/retroarch/retroarch.cfg` with the `retroarch.cfg`
   from this package (keep a copy of the old one). It is the cabinet's
   working config of 2026-10-25 as RetroArch 1.22.2 writes it; the
   MVSX-specific values are `video_driver = "sdl_mvsx"`,
   `video_fullscreen_x/y = "0"` (use the framebuffer mode the boot
   script sets), `aspect_ratio_index = "14"` (8:3, see "Framebuffer
   mode" below), `video_refresh_rate = "60"`, `vrr_runloop_enable =
   "false"`, `fps_show = "false"`, `video_filter` empty (no filter, true
   colours like the 1.9.0 build; pick Scanline2x or another filter under
   Settings > Video > Video Filter or per core via Quick Menu > Overrides;
   Scanline2x darkens every second line to 75%), the filter directories
   under `~/retroarch/filters/video|audio`, `screenshot_directory` at
   `~/retroarch/screenshots`, and the Online Updater / Core Updater menu
   entries hidden because this build has no SSL and the libretro buildbot
   is HTTPS-only. If you prefer to keep your own file,
   `retroarch.cfg.mvsx-additions` lists these values.
5. Copy `boot-usb.sh` to the root of the flash drive (next to the
   `retroarch/` directory), replacing the old one. The only change is the
   `fbset` line: 1280x512 instead of 1280x720.
6. Boot. The log is `/mnt/hdisk/retroarch/retroarch.log` (the `bin/<core>`
   scripts log to `/mnt/hdisk/ra_<core>.log` instead).

Cores do not need to be rebuilt: the old cores were linked against glibc
2.32 and run unchanged on glibc 2.40.

To roll back, restore `bin.old` and `lib.old`.

## Old core overrides from the 2021 package

The 2021 MVSX beta shipped full-config core overrides in
`config/Beetle NeoPop/`, `config/Beetle SuperGrafx/`, `config/FinalBurn Neo/`,
`config/PCSX-ReARMed/` and `config/PicoDrive/` (`<Core>/<Core>.cfg`,
~3000 lines each). They force the 2021 setup for those cores (`linuxraw`
input, `sdl` video at 1280x720, no filter) and make RetroArch discard
menu changes made while one of them is running. On the cabinet's flash
drive they have been deleted (2026-10-25); on another install retire them
with `info/disable-2021-overrides.sh` (it renames them to
`*.2021-override.bak`). The `.opt` core-option files next to them are
kept, although with `global_core_options = "true"` RetroArch reads
`retroarch-core-options.cfg` instead. Redo per-core button
layouts with Quick Menu > Controls > Save Core Remap, and set the volume
gain globally if those cores now sound quieter (they ran at +12 dB).

Settings changed in the menu are written to `retroarch.cfg` only on a
clean exit (Quit RetroArch) or via Main Menu > Configuration File > Save
Current Configuration; killing the process from the cabinet menu does
not save.

## Video drivers

Two framebuffer drivers are compiled in. Pick one with `video_driver` in
`retroarch.cfg`.

### `sdl` (fallback)

The generic SDL 1.2 software driver, with the two acmeplus fixes for this
hardware applied (shadow surface instead of a double-buffered hardware
surface; ABGR output for the BGR panel). It
behaves like the 1.9.0 build: 32 bpp, resolution from
`video_fullscreen_x/y`, full RetroArch aspect-ratio and integer-scaling
logic.

### `sdl_mvsx` (default)

A port of the driver acmeplus wrote for the 2024 "Sauron Key" build
(RS-90 driver + NEON integer upscalers), brought to 1.22.2 and made
self-contained:

* scales straight into the framebuffer (`/dev/fb0`, 32 bpp, no page
  flipping): on this SoC every byte written costs the same wherever it
  goes, so the usual shadow-surface-plus-copy doubled the work
  (measured 8.1 + 6.8 ms per frame against 4.7 ms direct, see below);
  no tearing is visible on the panel. `MVSX_SDL_HWSURFACE=0` restores
  the shadow surface. 16 bpp and double buffering gave garbage and
  flicker on this framebuffer. Frames are written in BGR order for the
  panel (the Actions fb driver advertises RGB masks, so this is not
  detected but assumed, as in the 1.9.0 build; set `MVSX_FB_BGR=0` in
  the launcher to trust the reported masks instead);
* real integer upscaling (NEON 1x-6x, generic beyond) when
  `video_scale_integer = "true"`; otherwise nearest-neighbour or, with
  `video_smooth = "true"`, horizontal linear filtering;
* follows the normal Aspect Ratio setting (core provided, 4:3, custom...)
  and `video_force_aspect`;
* resolution from `video_fullscreen_x/y`, or the current framebuffer
  mode when those are 0 (the shipped config: the mode comes from the
  `fbset` line in `boot-usb.sh`).

### Framebuffer mode (why 1280x512)

Measured on the cabinet (Actions GS702C, 4x Cortex-A5 at 1.13 GHz, 32 bpp
framebuffer only) with the `[stats]` lines described below, NES at
256x224:

| fb mode, drawing | per frame: scale + copy to fb | result |
|------------------|-------------------------------|--------|
| 1280x1024, shadow surface | 14.5 ms + 22.1 ms | 21 fps shown, 64% of frames dropped |
| 1280x512, shadow surface | 8.1 ms + 6.8 ms (4:3 at 1194x448) | 60 fps, a few drops; PSX 41-54 fps |
| 1280x512, direct (default now) | 4.7 ms + 0 | 60 fps, no drops, video thread 29% CPU; PSX 6-7 ms, 60 fps |

Streaming writes from this Cortex-A5 run at roughly 300 MB/s into RAM and
450 MB/s into the framebuffer, whatever the arithmetic (an index-table
scaler with row reuse measured the same as the per-pixel one), so the
cost is proportional to the bytes written per frame, and writing the
scaled frame once, straight into the framebuffer, is the big win.
The kernel's `lcd` driver advertises 1280x1024 and 1280x720 and accepts
other heights, but always keeps a 1280-pixel line stride: 640x480 is
shown zoomed with every other line stale, while 1280x512 is doubled
vertically by the display engine and fills the panel. In that mode a
framebuffer pixel is 1x2 on the panel, hence `aspect_ratio_index = "14"`
(8:3): a 4:3 game is drawn 1194x448 in the buffer and shows as 1194x896,
the same picture as 4:3 at 1280x1024. Horizontally the picture is as
sharp as native, vertically each line is doubled. To go back to the
native mode, set `-g 1280 1024 1280 1024 32` in `boot-usb.sh` and
`aspect_ratio_index = "0"`; at 1280x720 use the same 4:3 index.

The RGUI menu is unaffected by the aspect setting (it follows its own
"Lock Menu Aspect Ratio" option). Cores whose picture is not 4:3 need the
doubled ratio too, per core via Quick Menu > Overrides.

Environment overrides for experimenting from the launcher script:
`MVSX_FB_BPP=16` tries a 16 bpp mode, `MVSX_SDL_HWSURFACE=0` draws into
an SDL shadow surface instead of straight into `/dev/fb0`,
`MVSX_SDL_DOUBLEBUF=1` requests page flipping, `MVSX_FB_BGR=0` disables
the red/blue swap, `MVSX_VIDEO_STATS=0` turns off the diagnostics below,
`MVSX_SCANLINES=<percent>` draws scanlines while scaling (see below).

### CRT look (scanlines, boost, mask, blur)

Drawn by the driver while scaling. Boost and blur work on the content
frame (a tenth of the output pixels) and the mask is folded into the
scanline multiply, so the per-output-row work is that of plain scanlines;
measured cost of the whole look is in the `[stats]` line (`source fx`). All on by default; set in
`boot-usb.sh` (the shipped script lists them), `0` turns one off:

| variable | default | effect |
|----------|---------|--------|
| `MVSX_SCANLINES` | 65 | where a source row is repeated vertically (every 224/240-line system on the 1280x512 mode), the later copies are written at this brightness. Content that is not line-doubled (480-line PSX menus and cutscenes) and the RGUI menu get none, instead of aliased ones. 75 = the Scanline2x filter look. |
| `MVSX_SCANLINE_BOOST` | 115 | content multiplied by this percentage (saturating) before scaling, so the scanlines add contrast instead of only dimming |
| `MVSX_MASK` | 85 | aperture mask: every third output column of the scanline rows at this brightness (free, it rides on the scanline multiply; needs scanlines on) |
| `MVSX_HBLUR` | 1 | horizontal blur over the content frame before scaling, composite-video softness that blends dither patterns (PSX, Genesis) into shades: 1 = light [1 6 1]/8, 2 = [1 2 1]/4, 3-4 = that many minus one [1 2 1]/4 passes |

Leave `video_filter` empty. The Scanline2x plugin is still shipped
(rebuilt to use all four cores) for the 1280x1024 mode, where 480-line
content fits; on 1280x512 its 960-row output for 480-line content is
down-scaled and the dark lines alias into bands.

The driver logs diagnostics by default: at start the CPU type, core
count and frequency (`[diag] cpuinfo`, `cpu online`, `cpu0 ...`) and the
framebuffer's properties (`[diag] fb0 ...`); then every 10 seconds a
`[stats]` line with the presented frame rate, how many core frames the
threaded video wrapper dropped, the average/maximum time spent converting
and scaling a frame and in the SDL flip (the copy to the framebuffer),
followed by the CPU use of each RetroArch thread. The lines land in the
RetroArch log (`/mnt/hdisk/retroarch/retroarch.log` when started from
`boot-usb.sh`) with everything else.

The log line `[SDL1] [MVSX] Video mode ...` reports the depth, pixel masks
and whether a hardware surface was obtained.

## Audio

`oss` on `/dev/snd/actsnd`, as before. Like both acmeplus builds, the
driver runs with plain blocking writes (the Actions driver does not
implement `SNDCTL_DSP_GETOSPACE`), so "Audio rate control was desired,
but driver does not support needed features" in the log is expected.

The Actions driver is picky, and the setup sequence was tuned on the
device (October 2026):

* Only `SETFRAGMENT`, `CHANNELS` and `SPEED` are issued. `SETFMT` is
  rejected with EINVAL and is skipped (set `MVSX_AUDIO_SETFMT=1` in the
  launcher to force it). S16LE stereo is assumed.
* The Actions-specific output-path ioctls that acmeplus' 1.9.0 build
  issued (`SNDRV_SOUTMODE`, `SNDRV_SSPEAKER`) are **not** sent by
  default: on the MVSX they coincided with joypad input dying, and the
  speakers work with the device's default routing. They can be sent on
  request with `MVSX_AUDIO_OUT_MODE=<0..5>` / `MVSX_AUDIO_SPEAKER=<0|1>`.
* `SNDCTL_DSP_RESET` is never issued (pause just stops feeding the
  device): after a reset the driver refuses every write with EPERM,
  which was the "audio stops after opening the menu" problem of the
  earlier builds. If writes do start failing, the driver reopens the
  device once and continues.
* The device cannot do non-blocking writes (it returns `ERESTARTSYS`),
  so writes are blocking as in 1.9.0, and during fast-forward audio is
  dropped instead.

## Shaders: what is and is not possible

RetroArch GLSL/slang shaders run on the GPU through OpenGL ES or Vulkan.
The MVSX's PowerVR SGX544 has no Linux (glibc) userspace driver; its only
GLES driver is the Android (bionic) one, which a glibc process cannot
load. Every RetroArch build for this device (acmeplus 2021, acmeplus 2024,
this one) is therefore framebuffer-only, with `HAVE_OPENGL`, `HAVE_EGL`
and `HAVE_GLSL` off, and the Shaders menu stays empty.

What this build gives instead is the CPU **video filter** system, which
covers the usual retro looks:

* `Scanline2x`, `Phosphor2x`, `Blargg_NTSC_SNES_*` (composite, S-Video,
  RGB, RF), `Grid2x/3x/4x`, `Dot_Matrix_*`, `Gameboy*`, `LQ2x`, `2xBR`,
  `2xSaI`, `Super2xSaI`, `SuperEagle`, `EPX`, `Scale2x`, `Normal2x/4x`,
  `Darken`, `Upscale_*`, `Picoscale_*`.

Select one in `Settings > Video > Video Filter` or set
`video_filter = "~/retroarch/filters/video/Scanline2x.filt"`. Filters run
on the CPU before scaling; the `2x` filters are cheap at Neo Geo
resolutions, the Blargg NTSC ones cost more.

If GPU shaders are a must, the only routes are outside this glibc
environment:

1. Run RetroArch as an Android app on the stock Android framework (if
   SurfaceFlinger is alive on the MVSX firmware) and launch it from the
   menu scripts with `am start`; that gets GLES2 and GLSL shaders.
   Needs an APK built for the device's Android API level.
2. libhybris to load the Android GLES stack into a glibc process; a
   large, device-specific effort with no existing MVSX port.

## Features enabled in this build

RGUI menu, threaded video, rewind, run-ahead, screenshots, save states,
cheats, soft-patching (IPS/BPS/UPS), 7-Zip/ZIP content, CHD, playlists
and the libretro database, netplay/network commands (no SSL, so no online
updater or achievements), video and audio filter plugins, CC and nearest
resamplers, translations.

Not built: XMB/Ozone/MaterialUI (need a GPU), overlays, OpenGL, Vulkan,
achievements, online updater, FreeType fonts (RGUI uses its bitmap font).

## Rebuilding

The GitHub Actions workflow `.github/workflows/MVSX.yml` in the repository
builds every push to an `mvsx-*` branch (the package zip is the run's
artifact) and publishes a release for tags named `v1.22.2-mvsx.N`.

Locally, on Linux or on Windows with MSYS2, with the Arm GNU Toolchain
14.2.Rel1 for `arm-none-linux-gnueabihf` (the package ships that
toolchain's glibc, so stay on a 14.2 release):

```
TOOLCHAIN_DIR=<toolchain> pkg/mvsx/build.sh            # the binary
TOOLCHAIN_DIR=<toolchain> pkg/mvsx/build-filters.sh    # the plugins
TOOLCHAIN_DIR=<toolchain> pkg/mvsx/package.sh <out> zip
```

Without `TOOLCHAIN_DIR` the scripts and `Makefile.mvsx` look for the
toolchain in `<workspace>/_tools/arm-gnu` next to the repository (and, on
Windows, for MSYS2 in `<workspace>/_tools/msys2`). The MSYS2 path
conversion must be off (`MSYS2_ARG_CONV_EXCL="*"`, the scripts do that):
otherwise MSYS rewrites the `/mnt/hdisk/...` dynamic-linker path that is
baked into the binary.

## Source changes relative to upstream v1.22.2

* `Makefile.mvsx` - build configuration for this target.
* `Makefile.common` - builds `sdl_mvsx_gfx.o` and the NEON scaler when
  `MVSX=1`.
* `gfx/drivers/sdl_mvsx_gfx.c` (new) and `gfx/drivers/mvsx/scaler_neon.[ch]`
  (from the Miyoo Mini port, via acmeplus' `mvsx_wip` branch).
* `gfx/video_driver.c/.h` - register the `sdl_mvsx` driver.
* `gfx/drivers/sdl_gfx.c` - MVSX: shadow surface, runtime BGR detection,
  point-sampled menu (acmeplus `asp_mvsx_legacy` fixes).
* `audio/drivers/oss.c` - MVSX: `/dev/snd/actsnd` default, no
  `GETOSPACE`-based rate control, clearer open errors.
