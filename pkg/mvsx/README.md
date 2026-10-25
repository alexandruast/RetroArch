# Neo Geo MVSX port (branch `mvsx-1.22.2`)

RetroArch v1.22.2 for the Neo Geo MVSX arcade cabinet (Actions GS702C,
4x Cortex-A5, Android-hosted glibc binary, framebuffer only).

- `README-MVSX.md` - what the package is, how to install it on the cabinet's
  flash drive, the video driver, audio, the framebuffer mode and the CRT look,
  with the measurements behind each decision.
- `DEVELOPMENT-NOTES.md` - project notes: device facts, workspace layout on
  the development PC, build/package procedure, full history of changes with
  measurements, open items. Written for whoever continues the work (human or
  AI assistant).
- `boot-usb.sh` - the flash-drive boot script (framebuffer mode, CPU clock,
  CRT look settings, starts RetroArch).
- `retroarch.cfg.mvsx-additions` - the config keys that matter for the MVSX.
- `build.sh`, `build-filters.sh`, `package.sh` - build with the Arm GNU 14.2
  toolchain under MSYS2 on Windows and assemble the package. They assume the
  layout described in `DEVELOPMENT-NOTES.md` (toolchain and MSYS2 under
  `_tools/`, the 2021 install under `retroarch-mvsx/`, this repository next
  to them); the workspace is found from the script location or `MVSX_ROOT`.
- `disable-2021-overrides.sh` - retires the 2021 package's per-core overrides.

Source changes against upstream v1.22.2 are the commits of this branch
(`git log v1.22.2..mvsx-1.22.2`): `Makefile.mvsx`, the `sdl_mvsx` video
driver (`gfx/drivers/sdl_mvsx_gfx.c`, `gfx/drivers/mvsx/`), MVSX fixes in
`gfx/drivers/sdl_gfx.c` and `audio/drivers/oss.c`, and the multithreaded
`scanline2x` filter.
