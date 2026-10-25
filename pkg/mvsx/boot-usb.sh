#!/system/bin/sh

LD_LIBRARY_PATH=/mnt/hdisk/retroarch/lib:$LD_LIBRARY_PATH
RAPATH=/mnt/hdisk/retroarch
CORESPATH=/mnt/hdisk/cores
RACFG=${RAPATH}/retroarch.cfg

export HOME=/mnt/hdisk
export XDG_CONFIG_HOME=/mnt/hdisk

# CRT look, drawn by the video driver while scaling at almost no cost
# (leave video_filter empty in retroarch.cfg). Percentages; 0 turns one off.
# These are also the driver defaults, listed here so they are easy to tune.
export MVSX_SCANLINES=65        # dark line brightness (content that is not
                                # line-doubled, e.g. 480-line PSX, gets none)
export MVSX_SCANLINE_BOOST=115  # bright line x1.15, keeps the picture lively
export MVSX_MASK=85             # mask: every 3rd column of the dark lines at 85%
export MVSX_HBLUR=1             # horizontal blur: 1 light, 2 medium, 3-4 passes

# Framebuffer mode; the display engine upscales to the 1280x1024 panel.
# The kernel keeps a 1280-pixel line stride whatever is asked (640x480
# came out zoomed with every other line stale), so keep the width at
# 1280 and halve the height: 1280x512 is doubled vertically by the
# display engine. Falls back to 1280x720 if the kernel refuses it.
# retroarch.cfg has video_fullscreen_x/y = 0 (use the active mode).
# 1280x1024 native also runs at 60 fps (direct rendering) but leaves no
# headroom for CPU filters on PSX-size content; 512 looks the same here.
busybox fbset -fb /dev/fb0 -g 1280 512 1280 512 32 || busybox fbset -fb /dev/fb0 -g 1280 720 1280 720 32

# Lift the CPU clock cap (performance governor, but scaling_max_freq was
# 1128000 of a possible 1308000 kHz). Offline cores have no cpufreq dir.
for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_max_freq; do
    [ -w "$f" ] && echo 1308000 > "$f"
done

"${RAPATH}/bin/retroarch" \
    --verbose \
    --log-file="${RAPATH}/retroarch.log" \
    --config="${RACFG}"  \
    &> "${RAPATH}/error_retroarch.log"
