RetroArch 1.22.2 for the Neo Geo MVSX cabinet, built by GitHub Actions from
the `mvsx-1.22.2` branch with the Arm GNU Toolchain 14.2.Rel1.

**New in v1.22.2-mvsx.2**: the cabinet's volume knob now works in
RetroArch (it did nothing before), and the menu is drawn at 640x512
instead of 320x240, so playlist thumbnails are legible and the text is
even (`MVSX_MENU_HIRES=0` in `boot-usb.sh` brings back the old menu).
Updating from v1.22.2-mvsx.1 only needs the new `bin/retroarch` (the
README is updated too).

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
