RetroArch 1.22.2 for the Neo Geo MVSX cabinet, built by GitHub Actions from
the `mvsx-1.22.2` branch with the Arm GNU Toolchain 14.2.Rel1.

**Package contents**: `bin/retroarch` and the launcher scripts, `lib/` (glibc
2.40 runtime and the device libraries), `filters/` (CPU video and audio
filter plugins), `autoconfig/`, `retroarch.cfg`, `boot-usb.sh`,
`README-MVSX.md`, and `info/` with the source patch against upstream v1.22.2.

**Install**: back up `retroarch/bin`, `retroarch/lib`, `retroarch/retroarch.cfg`
and `boot-usb.sh` on the flash drive; copy `bin/`, `lib/` (as a whole),
`filters/` and `autoconfig/` into `retroarch/`, `retroarch.cfg` into
`retroarch/`, and `boot-usb.sh` to the root of the drive; boot. The menu
opens with Start+Select. Full instructions and the picture settings are in
`README-MVSX.md` inside the package.

**Rollback**: restore the backed-up files.
