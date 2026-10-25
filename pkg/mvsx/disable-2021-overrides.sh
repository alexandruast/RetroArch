#!/system/bin/sh
# Retires the full-config core overrides that came with the 2021 MVSX
# RetroArch beta. They pin five cores to the 2021 setup (linuxraw input,
# sdl video at 1280x720, no filter) and block saving menu changes.
# Core option files (*.opt) are left in place.
CFG=/mnt/hdisk/retroarch/config
for c in "Beetle NeoPop" "Beetle SuperGrafx" "FinalBurn Neo" "PCSX-ReARMed" "PicoDrive"; do
   if [ -f "$CFG/$c/$c.cfg" ]; then
      mv "$CFG/$c/$c.cfg" "$CFG/$c/$c.cfg.2021-override.bak" && echo "retired: $c"
   fi
done
echo "done"
