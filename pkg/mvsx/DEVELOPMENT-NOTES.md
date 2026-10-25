# RetroArch for the Neo Geo MVSX cabinet — project notes for future sessions

Read this before touching anything. It records what was found, changed and
measured on the real cabinet so the next session does not start from zero.
Last updated 2026-10-25.

## The target

A Neo Geo MVSX arcade cabinet (Unico, 2020) running its stock Android
firmware. RetroArch is a **glibc ARMv7 binary** (not an Android app) started
from a shell script on a USB flash drive mounted at `/mnt/hdisk`.

- SoC: Actions **GS702C**, 4x **Cortex-A5** at 1.13 GHz (table allows 1.31 GHz;
  capped via cpufreq, governor `performance`), 256 MB RAM, NEON/VFPv4.
  Kernel 3.4.0, Android `/system/bin/sh` + busybox. Built with
  `-mcpu=cortex-a5 -mfpu=neon-vfpv4` since commit (squashed).
- Panel 1280x1024 (5:4), framebuffer `/dev/fb0` (`lcd`), **32 bpp only**.
  Kernel `lcd` driver advertises modes 1280x1024 and 1280x720. It accepts
  other heights but **always keeps a 1280-pixel line stride**: 640x480 shows
  zoomed with every other line stale; **1280x512 works** and the display
  engine doubles it vertically to fill the panel.
- The panel is **BGR** but the kernel reports standard RGB masks. Every
  working build swaps red/blue unconditionally (acmeplus commit
  (squashed)); detection from SDL masks does not work.
- Audio: OSS on `/dev/snd/actsnd` (Actions driver). No `GETOSPACE`, `SETFMT`
  rejected, `SNDCTL_DSP_RESET` breaks the device, non-blocking writes return
  `ERESTARTSYS`. Blocking writes only; "rate control not supported" in the
  log is expected.
- No GPU userspace for glibc: framebuffer only, no shaders ever. CPU video
  filters work.
- **The user has no shell/SSH on the cabinet.** Everything is done by editing
  files on the flash drive from the PC (it mounts as a drive letter) and reading the
  log back. Diagnostics must go through RetroArch's log.

## Workspace map (the `retroarch` workspace directory, `<workspace>` below)

| Path | What |
|------|------|
| `RetroArch-libretro/` | upstream `libretro/RetroArch` clone |
| `RetroArch-1.22.2-mvsx/` | **git worktree** of the above at tag `v1.22.2`, branch `mvsx-1.22.2`: one squashed MVSX commit on top of `v1.22.2` (see "Git remotes / fork"); commit further work on this branch. No git identity is configured on this PC: commit with `git -c user.name=... -c user.email=...` (use a noreply address). This is what gets built. |
| `RetroArch-ASP/` | acmeplus' fork; branches `asp_mvsx_legacy` (the 1.9.0 build from Feb 2021 that the cabinet originally ran) and `mvsx_wip` (2024, RS-90 driver + NEON scalers). Reference only. |
| `retroarch-mvsx/` | copy of the original 2021 install from the cabinet (old binary, libs, config, 2021 overrides). Reference and source of unchanged launcher scripts/autoconfig for packaging. |
| `retroarch-mvsx-1.22.2-update/` + `.zip` | the **built package** (what goes on the drive). Reassembled by `_tools/pkg/package.sh`. |
| `_tools/arm-gnu/` | Arm GNU 14.2 toolchain (`arm-none-linux-gnueabihf`, glibc 2.40) |
| `_tools/msys2/` | MSYS2 used to run `make` |
| `_tools/pkg/` | package sources: `package.sh`, `build.sh`, `build-filters.sh`, `README-MVSX.md`, `retroarch.cfg`, `retroarch.cfg.mvsx-additions`, `boot-usb.sh`, `info/` (patch + override-retire script). |
| the flash drive (a drive letter when plugged into the PC; `<drive>` below) | the cabinet's flash drive: `boot-usb.sh` at the root, `retroarch/` = `/mnt/hdisk/retroarch` |

Source changes vs upstream v1.22.2 (all in the worktree, see
`_tools/pkg/info/retroarch-1.22.2-mvsx.patch`): `Makefile.mvsx`,
`Makefile.common`, `gfx/drivers/sdl_mvsx_gfx.c` (new driver),
`gfx/drivers/mvsx/scaler_neon.[ch]`, `gfx/drivers/sdl_gfx.c`,
`gfx/video_driver.[ch]`, `gfx/video_filter.c`, `audio/drivers/oss.c`.

## Git remotes / fork

The worktree's repo has `origin` = https://github.com/libretro/RetroArch
(upstream) and `fork` = https://github.com/alexandruast/RetroArch.git
(the user's fork). `mvsx-1.22.2` tracks `fork/mvsx-1.22.2`. On 2026-10-25 the branch's
18 development commits were squashed into **one commit on top of
`v1.22.2`** before publishing, so that no earlier revision of the files
(with machine-specific paths) remains in the history; the local
pre-squash history is kept only in the local branch
`mvsx-1.22.2-history-backup` (never push it; delete it when no longer
needed). Commits are authored with a GitHub noreply address. Later work
goes on as normal commits on top; `git push` from the worktree updates
the fork (the first push after the squash needs `--force-with-lease`); the user runs the push (it
authenticates interactively over HTTPS). `pkg/mvsx/` in the
branch holds copies of `_tools/pkg/*` plus `DEVELOPMENT-NOTES.md` (= this
file) — keep both in sync when either changes, and push after.

## Build, package, ship

```
MSYSTEM=MSYS _tools/msys2/msys64/usr/bin/bash.exe -lc '
  export MSYS2_ARG_CONV_EXCL="*"
  cd <workspace>/RetroArch-1.22.2-mvsx
  make -f Makefile.mvsx -j16'
```
`MSYS2_ARG_CONV_EXCL="*"` is mandatory (otherwise the `/mnt/hdisk/...`
dynamic-linker path gets rewritten). Incremental builds take under a minute.
`bash _tools/pkg/build.sh` wraps exactly this (exit status = make's) — use
it; inline `bash.exe -lc '...'` with nested quotes broke twice. Chain the
rest with `&&` only; a heredoc in the middle of a chain ends the chain.

Then, from `retroarch/`:
1. regenerate the patch: in the worktree `git add -N . && git diff HEAD -- . ':!retroarch' > ../_tools/pkg/info/retroarch-1.22.2-mvsx.patch; git reset -q`
2. `bash _tools/pkg/package.sh retroarch-mvsx-1.22.2-update` (strips the binary, assembles lib/, filters/, launchers, config, boot script, README) and copy the patch into `retroarch-mvsx-1.22.2-update/info/`
3. zip with **bsdtar**, not PowerShell (PowerShell writes backslash paths):
   `_tools/msys2/msys64/usr/bin/bsdtar.exe -a -cf retroarch-mvsx-1.22.2-update.zip retroarch-mvsx-1.22.2-update`
4. verify with `grep -c -a '<new log string>' retroarch-mvsx-1.22.2-update/bin/retroarch` (no `strings` on this PC).

Installing = copying the changed files onto the flash drive (usually just
`retroarch\bin\retroarch`, sometimes `retroarch\retroarch.cfg` and
`boot-usb.sh`). Keep `.bak-*` copies next to edited drive files.
`retroarch-core-options.cfg` is the cabinet's own state and is not shipped.

## How the cabinet runs it

`<drive>\boot-usb.sh` runs `busybox fbset -fb /dev/fb0 -g 1280 512 1280 512 32`
(falls back to 1280x720) and starts `retroarch/bin/retroarch` with the menu,
`--log-file retroarch/retroarch.log` (truncated at each start) and stderr to
`retroarch/error_retroarch.log`. Cores live in `/mnt/hdisk/cores`, ROMs in
`/mnt/hdisk/roms`. The `bin/<core>` scripts are an alternative direct-launch
path (log `/mnt/hdisk/ra_<core>.log`); the package patches their `fbset`
line to the same 1280x512.

Current `retroarch.cfg` on the drive (= package `retroarch.cfg`):
`video_driver = sdl_mvsx`, `video_fullscreen_x/y = 0` (use the active fb
mode), `aspect_ratio_index = 14` (8:3 — correct 4:3 in the half-height
mode), `video_refresh_rate = 60`, `vrr_runloop_enable = false`,
`fps_show = false`, `video_filter = ""`, `video_threaded = true`,
`video_scale_integer = true`, `global_core_options = true`. RetroArch
rewrites the file on clean exit (`config_save_on_exit`).

The five 2021 full-config core overrides (`config/<Core>/<Core>.cfg` for
FinalBurn Neo, PicoDrive, PCSX-ReARMed, Beetle NeoPop, Beetle SuperGrafx)
were deleted from the drive on 2026-10-25; originals remain in
`retroarch-mvsx/config/`.

## Diagnostics built into the driver

`sdl_mvsx` logs (unless `MVSX_VIDEO_STATS=0`): at init `[diag]` lines with
`/proc/cpuinfo`, cpufreq and `/sys/class/graphics/fb0/*`; every 600 frames a
`[stats]` line (fps shown, core frames dropped by the threaded wrapper,
avg/max µs for conv+scale and for `SDL_Flip`, content and output size, which
scaler ran) followed by per-thread CPU% from `/proc/self/task`. Main thread =
emulator core, the other busy thread = video. This is the only measurement
channel; use it before guessing.

Other env switches (set with `export` in `boot-usb.sh`): `MVSX_FB_BGR=0`,
`MVSX_SDL_HWSURFACE=1`, `MVSX_SDL_DOUBLEBUF=1`, `MVSX_FB_BPP=16`,
`MVSX_AUDIO_SETFMT=1`, `MVSX_AUDIO_OUT_MODE`, `MVSX_AUDIO_SPEAKER`.

## History (what was done, in order)

The steps below were separate commits during development; they were
squashed into one commit before the branch was published, so there are no
per-step hashes any more. The measurements are the record.

1. Port of acmeplus' 1.9.0 MVSX build to upstream v1.22.2 with a new
   `sdl_mvsx` driver (earlier sessions; see README-MVSX.md).
2. **Red/blue swapped** in the new build (sky orange, Mario blue). Cause: the
   port made the BGR swap conditional on SDL's reported masks, which are
   always RGB on this kernel. Fix: swap unconditionally (`force_bgr`), in
   both `sdl_mvsx_gfx.c` and `sdl_gfx.c`; `MVSX_FB_BGR=0` disables.
3. **Scanline2x** was enabled globally in the package config; it dims
   colours (75% every second line). Default is now no filter.
4. **Lag/stutter.** Diagnostics added to the driver, then measured on the
   cabinet (NES, 256x224):
   - 1280x1024: conv+scale **14.5 ms** (scalar point scaler — the NEON n×n
     path is never used because integer scaling makes width follow the
     aspect: 1194 ≠ 256×4) + flip **22.1 ms** (5.2 MB shadow→fb at
     ~235 MB/s) → 21.5 fps shown, 64% frames dropped.
   - 640x480: 4.0 + 3.5 ms, 60 fps, but the picture is broken (stride).
   - **1280x512**: 4.4 + 8.9 ms, **60 fps, ~0 drops** → adopted, with 8:3
     aspect so the picture is 1194x896 on the panel like native 4:3.
   Config fixes along the way: `video_refresh_rate` 30→60 (the 2021 value
   disabled A/V sync tuning), `vrr_runloop_enable` true→false, `fps_show`
   off.
5. Package updated to the drive's state (config, `boot-usb.sh`, launcher
   `fbset`, README with the measurements).
6. **Build round, scaler** (squashed): index
   table + row reuse made **no difference** (8.1 ms for 1194x448, same
   ~15 ns per output pixel as before) → the cost is bytes written
   (~300 MB/s streaming stores on this A5), not arithmetic. Row reuse now
   goes through a cached row buffer (never reads the screen).
7. **Direct rendering** (`MVSX_SDL_HWSURFACE=1`, exported in
   `boot-usb.sh`): scale straight into `/dev/fb0`, no shadow copy. NES:
   conv+scale **4.7 ms**, flip 0.02 ms (was 8.1 + 6.8), 60 fps, 0 drops,
   video thread 29% CPU. PSX 320x240→1280x480: 6–7 ms, 60 fps, 0 drops
   (was 41–54 fps). Framebuffer writes are *faster* than RAM writes here.
   Tearing in motion: to be confirmed by the user.
8. Found `input_fps_toggle_btn = "0"` (joypad button 0 = X on the panel)
   and `input_enable_hotkey_btn = "nul"`, so pressing X toggled
   RetroArch's FPS counter. Unbound (`nul`) on the drive and in the
   package. `input_menu_toggle_btn = "1"` (= Select) was the same kind
   of leftover; also `nul` now — the menu opens with Start+Select only
   (`input_menu_toggle_gamepad_combo = 4`).
9. **Direct rendering is the driver default** (squashed):
   `sw_surface = false` unless `MVSX_SDL_HWSURFACE=0`; the export was
   removed from `boot-usb.sh`; README updated. User confirmed no tearing.
   Drive binary backups: `bin/retroarch.bak-step0/1/2`.
10. **Native 1280x1024 in direct mode + CPU cap lifted** (config-only test,
    `boot-usb.sh` writes 1308000 to `scaling_max_freq`, confirmed
    `cpu0 cur kHz: 1308000`): NES 1194x896 conv+scale **7.7 ms**, 60 fps,
    0 drops, both threads ~45%. PSX 1280x960 (NEON 4x) 8–10.7 ms, 60 fps,
    0–1 drops, core thread up to 80%. **Scanline2x on PSX** (640x480 →
    NEON 2x): 18.6 ms, 37 fps, 245 drops, main thread saturated — the
    filter runs single-threaded on the main thread (`scanline2x.c` hard-
    codes `threads = 1`) and doubles the conversion input; filters are
    fine for NES-size content, too heavy at PSX size in 1024 mode.
    User saw no difference between 512 and 1024 → **1280x512 is final**
    (headroom for filters), `aspect_ratio_index = 14`, CPU cap lift kept
    in `boot-usb.sh`. User enabled **Scanline2x globally** in the menu
    (`video_filter` in the config, package follows).
11. **Scanline2x multithreaded** (squashed): upstream forces
    one thread (on the main/emulator thread); now splits rows across the
    frontend's threads (4). Rebuilt with the ARM toolchain
    (`make -C gfx/video_filters platform=unix compiler=<arm-gcc>
    extra_flags="-march=armv7-a -mtune=cortex-a5 -mfpu=neon
    -mfloat-abi=hard -O2" scanline2x.so`), installed on the drive
    (`filters/video/scanline2x.so`, backup `.bak-step3`). Measured at 512:
    `Using 4 threads`, filter threads 3–6% CPU each; NES+scanlines
    (512x448 → 1194x448) 9.6 ms, 60 fps, 0 drops; **PSX+scanlines**
    (640x480 → 1280x480) 11–15 ms, 54–58 fps, 5–45 drops — the cost moved
    to the driver's separate RGB565→32 conversion pass (1.2 MB) plus a
    gather with no repeated rows.
12. **Fused convert+scale** (squashed): RGB565 content on the
    point-scaler path is gathered straight from the 16-bit frame and
    expanded with 3 per-channel LUTs (`lut_r/g/b`, panel channel order);
    no intermediate 32-bit copy. Installed on the drive (`bin/retroarch`,
    backup `.bak-step3`). Expected PSX+scanlines ~8–9 ms. **Not yet
    measured** on its own (superseded by 13 for the scanline case; still
    active for any other RGB565 content on the point path).
13. **Scanline2x broke PSX cutscenes at 512**: 480-line PSX modes make
    the filter output 960 rows, down-scaled 2:1 into the 512-line buffer →
    dark rows alias into bands. Replaced by **driver scanlines** (commit
    (squashed)): `MVSX_SCANLINES=75` in `boot-usb.sh`; the point
    scalers write the later copies of a repeated source row from a
    darkened row buffer (`sdl_mvsx_emit_row32`), nothing for unrepeated
    content. `video_filter = ""` on the drive and in the package. Binary
    installed (`bin/retroarch`, backup `.bak-step4`). **User: "works
    perfect"** in games.
14. Menu showed uneven scanlines (RGUI 240 rows stretched to 512: some
    rows 2 copies, some 3 → double dark lines). Fix (commit
    (squashed)): scanlines only when `frame_height % height == 0` and
    never while `menu_active`. Installed (`bin/retroarch`, backup
    `.bak-step5`). Not yet confirmed by the user.
15. **Regression found in the scanline session's log**: NES 10.6 ms, PSX
    12–15 ms with drops (vs 4.7 / 6–7). Cause: the fused per-pixel LUT
    convert+gather (item 12) was never measured on unfiltered content;
    three dependent loads per pixel are slow on the in-order A5. The
    NEON `conv_rgb565_abgr8888` pass + plain gather is faster. **Fused
    path removed** (squashed), installed on the drive
    (`bin/retroarch`; `.bak-step6` = the step-5 binary). Expect NES
    ~5 ms with scanlines. Lesson: measure every build, including on the
    content it was not written for.
16. Measured after 15: NES **7.9 ms**, PSX 9.6–11 ms, 60 fps, menu OK —
    the ~3 ms over the no-scanline numbers was the scalar row darkening
    (2 multiplies per pixel, 267k px per NES frame). **NEON darkening**
    (`vmull_u8`, commit (squashed)) installed (`bin/retroarch`, backup
    `.bak-step7`). Measured: NES **6.2 ms**, PSX **7.2–9 ms**, 60 fps,
    **0 drops** over a 3-minute PSX session, both threads <40% CPU.
    Performance work considered finished at this point (2026-10-25).
17. **CRT look** (squashed), all in the driver, env-switchable,
    on by default: `MVSX_SCANLINES=65`, `MVSX_SCANLINE_BOOST=115` (bright
    rows ×1.15 saturating), `MVSX_MASK=85` (every 3rd output column),
    `MVSX_HBLUR=1` ([1 2 1]/4 passes over the converted content frame
    before scaling). Boost+mask = one NEON byte multiply with a
    per-column gain table (`sdl_mvsx_gain_row`/`apply_gain32`), once per
    distinct row; blur = NEON `vrhaddq_u8` on 256–320 px rows
    (`sdl_mvsx_hblur_frame`, in `sdl_mvsx_blit` after conversion, only
    when `src == conv_buf`). Menu untouched. `boot-usb.sh` (drive and
    package) lists all four. `_tools/pkg/build.sh` added: wraps the MSYS2
    make (use it instead of inline `bash -lc` quoting, which bit twice).
    **Measured**: user "perfect but a bit too blurry"; NES **9.5 ms**,
    PSX **12–13.6 ms with 5–36 drops/600** — each per-output-row NEON
    pass costs ~1.5–2 ms on the A5, not the ~0.3 ms I assumed.
18. **CRT look v2** (squashed): boost moved to the content
    frame (`sdl_mvsx_boost_row32` in `sdl_mvsx_source_fx`), mask folded
    into the scanline multiply as a per-column table
    (`sdl_mvsx_dark_gain_row`; the mask now shows on scanline rows only),
    blur levels: `MVSX_HBLUR=1` light [1 6 1]/8 (default), 2 = [1 2 1]/4,
    3–4 passes. Per-output-row work = plain scanlines again. Stats line
    now has `(of which source fx avg/max)`. Installed (`bin/retroarch`,
    backup `.bak-step9`). **Not yet measured**; expect NES ~6.5–7 ms,
    PSX ~8–9.5 ms, source fx <1 ms. Rule learned: on this SoC only the
    content frame (256–320 px rows) is cheap to touch; anything per
    output row beyond one memcpy costs ~1.5–2 ms per pass.
19. **Cortex-A5 build** (squashed): `Makefile.mvsx`
    `DEF_FLAGS := -mcpu=cortex-a5 -mtune=cortex-a5 -mfpu=neon-vfpv4
    -mfloat-abi=hard -marm` (was armv7-a/cortex-a9/neon); full rebuild
    (`rm -rf obj-mvsx` first — objects are not flag-tracked). Filters and
    audio DSP plugins rebuilt with the same flags via the new
    `_tools/pkg/build-filters.sh`. Binary + all `filters/*/*.so` installed
    on the drive (`bin/retroarch.bak-step10` = step-9 binary).
    **Measured (with item 18)**: user "looks good now" (lighter blur,
    mask on dark lines accepted). NES 8.6 ms of which **source fx
    1.9 ms**; PSX 11–13 ms, source fx 2.8–3.9 ms, 2–65 drops/600 — the
    content-resolution passes cost ~40 cycles/px: two passes, each with
    three 16-byte loads at ±4-byte offsets (unaligned loads are very slow
    on the A5).
20. **Fused fx pass** (squashed): `sdl_mvsx_fx_row32` does
    blur + boost in one pass with one contiguous 16-byte load per 4 px
    and `vextq_u8` neighbours; scalar head/tail. Installed
    (`bin/retroarch`, backup `.bak-step11`). **Measured**: source fx
    1.45 ms NES / 2.1–2.8 ms PSX (only ~25% better: the A5's NEON is
    simply slow, ~4–5 cycles per q-register instruction, no reordering).
    NES 8.2 ms, 60 fps, 0 drops; PSX 10.2–12.7 ms, 57–60 fps, 0–31
    drops/600 (≤5%), core thread 41%, video 49%. Spikes (max 20–30 ms)
    appear in every PSX run regardless of effects — likely scheduling /
    CPU hotplug (cpu2 was seen offline once), not the driver.
    User: "everything feels smooth and perfect". **State accepted as
    final** unless the user wants PSX at strictly 0 drops; the lever for
    that is per-core CRT settings (the driver can read the core's
    `library_name` from the runloop and apply overrides from a small
    text file on the drive, e.g. `hblur=0 boost=100` for PCSX-ReARMed),
    not yet implemented. The NEON fx cost cannot be reduced much further
    on this SoC; the 16-bit-before-conversion idea is not obviously
    cheaper.
    Final state: 1280x512 direct rendering, 8:3 aspect, CRT look as
    above, CPU at 1.31 GHz, no CPU filter, menu on Start+Select.

## Open items / next steps (not started)

Ordered by expected payoff. Working rule agreed with the user: **one change
at a time, config-only before builds, measure with the `[stats]` line.**

1. ~~Make direct rendering the default~~ — done ((squashed)). If
   tearing ever shows up, try `MVSX_SDL_DOUBLEBUF=1` in direct mode
   (kernel `pan` support unknown; `virtual_size` = visible size).
2. With the write cost gone, **native 1280x1024** is worth re-measuring in
   direct mode (1194x896 = 4.3 MB per frame ≈ 9–10 ms at the measured
   ~450 MB/s fb write rate): probably 60 fps for NES, tight for PSX. The
   user then picks 512 (half the writes, 2x vertical) vs 1024 (sharper).
   `aspect_ratio_index` must go back to 0 (4:3) at 1024.
3. Scaler arithmetic is irrelevant on this SoC (write-bound); the NEON
   n×n path and "pixel-perfect" modes are not worth the effort for speed.
   Content-rectangle `SDL_UpdateRect` is moot in direct mode.
4. Lift the CPU cap in `boot-usb.sh` (root): write `1308000` to
   `/sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq` (+16%).
5. `-mtune=cortex-a5` (or a7) in `Makefile.mvsx`; `-O3` for the gfx files.
6. Rebuild cores with the GCC 14 toolchain (`-O3 -mfpu=neon`); the cabinet's
   Snes9x 2002 is an old build without the frameskip options.
7. Per-core aspect overrides for non-4:3 cores in the 1280x512 mode (needs
   the doubled ratio); vertical arcade games.
8. 640x480 could work by writing with a 5120-byte stride (hack; low value).
9. Page flipping (`FBIOPAN_DISPLAY`) would only fix tearing, not speed;
   fb `virtual_size` equals the visible size, so it would need a custom
   driver and kernel cooperation.

## Things that bit us

- Windows Python does not understand Git Bash `/d/...` paths; use `D:/...` style drive paths.
  Backslash-continuation lines in heredoc-fed Python came out as literal
  `\n` — keep shell edits on one line.
- `cmp -l` on the config is meaningless after a length-changing edit; use
  `diff` to confirm exactly which lines changed.
- Scripts for the cabinet need **LF** line endings and `#!/system/bin/sh`.
- RetroArch truncates its log on start: have the user play, quit from the
  menu, then read the log.
- Photos from the user are the only view of the screen; ask for one when
  geometry/colour matters.
