# Neo Geo MVSX port (branch `mvsx-1.22.2`)

RetroArch v1.22.2 for the Neo Geo MVSX arcade cabinet (Actions GS702C,
4x Cortex-A5, Android-hosted glibc binary, framebuffer only). The user-facing
README is at the repository root; this directory holds everything the build
and the package need.

| file / directory | content |
|------------------|---------|
| `README-MVSX.md` | the package README: install, video driver, audio, framebuffer mode, CRT look, with the measurements behind each decision |
| `DEVELOPMENT-NOTES.md` | project notes for whoever continues the work (human or AI assistant): device facts, workspace layout, build/package procedure, full history with measurements, open items |
| `RELEASE-NOTES.md` | the text attached to GitHub releases |
| `NOTICE.md` | origin and licenses of the bundled third-party binaries |
| `build.sh`, `build-filters.sh`, `package.sh` | build the binary and the filter plugins with the Arm GNU toolchain and assemble the package (Linux, or Windows under MSYS2); used by the CI workflow and locally |
| `boot-usb.sh` | the flash-drive boot script: framebuffer mode, CPU clock cap, CRT look settings, starts RetroArch |
| `retroarch.cfg` | the cabinet's working configuration |
| `retroarch.cfg.mvsx-additions` | the config keys that matter for the MVSX, with comments |
| `launchers/` | the `bin/<core>` direct-launch scripts |
| `autoconfig/` | joypad autoconfig for the cabinet's panel |
| `device-lib/` | SDL 1.2, tslib and the other device libraries from the 2021 install: linked against at build time, shipped in the package (see `NOTICE.md`) |
| `sdl12-include/` | SDL 1.2 headers for the cross build |
| `disable-2021-overrides.sh` | retires the 2021 package's per-core overrides on an existing install |

Source changes against upstream v1.22.2 are the commits of this branch
(`git log v1.22.2..mvsx-1.22.2`): `Makefile.mvsx`, the `sdl_mvsx` video
driver (`gfx/drivers/sdl_mvsx_gfx.c`, `gfx/drivers/mvsx/`), MVSX fixes in
`gfx/drivers/sdl_gfx.c` and `audio/drivers/oss.c`, the multithreaded
`scanline2x` filter, and the CI workflow `.github/workflows/MVSX.yml`.
