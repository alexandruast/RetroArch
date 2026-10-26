/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#ifdef HAVE_OSS_BSD
#include <soundcard.h>
#else
#include <sys/soundcard.h>
#endif

#include <retro_endianness.h>
#include <compat/strl.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../audio_driver.h"
#include "../../verbosity.h"

#if defined(MVSX)
/* Actions Semi (ATM7051) OSS-style sound device on the
 * Neo Geo MVSX / Arcade Stick Pro */
#define DEFAULT_OSS_DEV "/dev/snd/actsnd"
#elif defined(HAVE_OSS_BSD)
#define DEFAULT_OSS_DEV "/dev/audio"
#else
#define DEFAULT_OSS_DEV "/dev/dsp"
#endif

typedef struct oss_audio
{
   int fd;
   bool is_paused;
#if defined(MVSX)
   bool nonblock;
   bool warned_stall;
   unsigned rate;
   unsigned latency;
   unsigned err_count;
   int knob_level;
   unsigned knob_glitches;
   bool knob_off;
   size_t knob_bytes;
   char device[256];
#endif
} oss_audio_t;

#if defined(MVSX)
/* Actions Semi audio driver private ioctls (see the Android HAL,
 * hardware/actions/audio): select the output path. Without them the
 * device opens fine but stays silent. The 1.9.0 MVSX build used
 * O_MODE_HDMI with the speaker flag off. */
#define SNDRV_SOUTMODE 0xffff0000
#define SNDRV_SSPEAKER 0xffff0001

enum
{
   O_MODE_I2S_2        = 0,
   O_MODE_I2S_51_TDM_A = 1,
   O_MODE_I2S_71_TDM_A = 3,
   O_MODE_SPDIF        = 4,
   O_MODE_HDMI         = 5
};

/* Only touches the output path when asked to via the environment
 * (MVSX_AUDIO_OUT_MODE=<0..5>, MVSX_AUDIO_SPEAKER=<0|1>): on the
 * MVSX issuing these ioctls coincided with joypad input dying. */
static void oss_mvsx_set_output_path(int fd)
{
   const char *env = getenv("MVSX_AUDIO_OUT_MODE");

   if (env && *env)
   {
      int out_mode = atoi(env);

      if (ioctl(fd, SNDRV_SOUTMODE, &out_mode) < 0)
         RARCH_WARN("[OSS] SNDRV_SOUTMODE(%d) failed: %s.\n", out_mode, strerror(errno));
      else
         RARCH_LOG("[OSS] Output mode set to %d.\n", out_mode);
   }
   else
      RARCH_LOG("[OSS] Output mode left as is (MVSX_AUDIO_OUT_MODE not set).\n");

   env = getenv("MVSX_AUDIO_SPEAKER");
   if (env && *env)
   {
      int speaker_on = atoi(env);

      if (ioctl(fd, SNDRV_SSPEAKER, &speaker_on) < 0)
         RARCH_WARN("[OSS] SNDRV_SSPEAKER(%d) failed: %s.\n", speaker_on, strerror(errno));
      else
         RARCH_LOG("[OSS] Speaker flag set to %d.\n", speaker_on);
   }
   else
      RARCH_LOG("[OSS] Speaker flag left as is (MVSX_AUDIO_SPEAKER not set).\n");
}

/* Fragment/channel/rate setup for the Actions driver. It rejects
 * some of the standard OSS ioctls (SETFMT returns EINVAL; the 1.9.0
 * build never issued it because of a brace bug, and worked), so
 * failures are warnings and the device's native S16LE stereo is
 * assumed. Returns the rate the device settled on. */
static int oss_mvsx_configure(oss_audio_t *ossaudio)
{
   int frags, frag, channels, format, new_rate;
   const char *env = NULL;

   frags = (ossaudio->latency * ossaudio->rate * 4) / (1000 * (1 << 10));
   frag  = (frags << 16) | 10;

   if (ioctl(ossaudio->fd, SNDCTL_DSP_SETFRAGMENT, &frag) < 0)
      RARCH_WARN("[OSS] Could not set fragment sizes. Latency might not be as expected.\n");

   channels = 2;
   if (ioctl(ossaudio->fd, SNDCTL_DSP_CHANNELS, &channels) < 0)
      RARCH_WARN("[OSS] SNDCTL_DSP_CHANNELS failed: %s.\n", strerror(errno));
   else
      RARCH_LOG("[OSS] Channels: %d.\n", channels);

   format = is_little_endian() ? AFMT_S16_LE : AFMT_S16_BE;
   env    = getenv("MVSX_AUDIO_SETFMT");
   if (env && atoi(env) != 0)
   {
      if (ioctl(ossaudio->fd, SNDCTL_DSP_SETFMT, &format) < 0)
         RARCH_WARN("[OSS] SNDCTL_DSP_SETFMT failed: %s.\n", strerror(errno));
      else
         RARCH_LOG("[OSS] Format: 0x%x.\n", format);
   }
   else
      RARCH_LOG("[OSS] Skipping SNDCTL_DSP_SETFMT (assuming S16LE).\n");

   new_rate = (int)ossaudio->rate;
   if (ioctl(ossaudio->fd, SNDCTL_DSP_SPEED, &new_rate) < 0)
   {
      RARCH_WARN("[OSS] SNDCTL_DSP_SPEED(%u) failed: %s.\n",
            ossaudio->rate, strerror(errno));
      new_rate = (int)ossaudio->rate;
   }
   else
      RARCH_LOG("[OSS] Rate: %d Hz.\n", new_rate);

   return new_rate;
}

/* Cabinet volume knob. The kernel exposes its position in sysfs and
 * the stock software (SNK_MAIN, hylostash mvsnjemu) polls it every
 * 200 ms and sets the hardware level with the standard OSS mixer
 * ioctl on the sound device; nothing does that for RetroArch, so the
 * knob had no effect. Same mapping as mvsnjemu: level 0..35 from
 * 35 - value / 2.325, level 0 mutes, the rest index this table.
 * MVSX_VOLUME_KNOB=0 turns it off. */
#define OSS_MVSX_KNOB_PATH "/sys/class/volume/value"
/* The knob reads 2 (loudest) .. 83 (muted); a stray reading far
 * outside that (175 was seen once) would blip the volume to mute
 * until the next poll, so anything above this is ignored. */
#define OSS_MVSX_KNOB_MAX_VALUE 90

static const int oss_mvsx_knob_mixer[35] = {
   0x0a0a, 0x0e0e, 0x1010, 0x1212, 0x1414, 0x1616,
   0x1717, 0x1717, 0x1818, 0x1818, 0x1919, 0x1919,
   0x1a1a, 0x1a1a, 0x1b1b, 0x1b1b, 0x1c1c, 0x1c1c,
   0x1d1d, 0x1d1d, 0x1e1e, 0x1e1e, 0x1f1f, 0x1f1f,
   0x2020, 0x2020, 0x2121, 0x2121, 0x2222, 0x2222,
   0x2323, 0x2323, 0x2424, 0x2424, 0x2525
};

static void oss_mvsx_knob_poll(oss_audio_t *ossaudio)
{
   char buf[20];
   ssize_t n;
   int value, level, mixer;
   int fd;

   if (ossaudio->knob_off)
      return;

   if ((fd = open(OSS_MVSX_KNOB_PATH, O_RDONLY)) < 0)
   {
      RARCH_WARN("[OSS] Volume knob: cannot open \"%s\": %s. Knob disabled.\n",
            OSS_MVSX_KNOB_PATH, strerror(errno));
      ossaudio->knob_off = true;
      return;
   }
   n = read(fd, buf, sizeof(buf) - 1);
   close(fd);
   if (n <= 0)
      return;
   buf[n] = '\0';

   value = (int)strtol(buf, NULL, 10);
   if (value < 0 || value > OSS_MVSX_KNOB_MAX_VALUE)
   {
      ossaudio->knob_glitches++;
      if (ossaudio->knob_glitches <= 5)
         RARCH_LOG("[OSS] Volume knob: ignoring out-of-range value %d (%u so far).\n",
               value, ossaudio->knob_glitches);
      return;
   }
   level = 35 - (int)((float)value / 2.325f);
   if (level > 35)
      level = 35;
   if (level < 0)
      level = 0;

   if (level == ossaudio->knob_level)
      return;

   mixer = level ? oss_mvsx_knob_mixer[level - 1] : 0;
   if (ioctl(ossaudio->fd, SOUND_MIXER_WRITE_VOLUME, &mixer) < 0)
   {
      RARCH_WARN("[OSS] Volume knob: SOUND_MIXER_WRITE_VOLUME(0x%04x) failed: %s. Knob disabled.\n",
            mixer, strerror(errno));
      ossaudio->knob_off = true;
      return;
   }

   RARCH_LOG("[OSS] Volume knob: value %d -> level %d/35 (mixer 0x%04x).\n",
         value, level, mixer);
   ossaudio->knob_level = level;
}

/* The Actions driver refuses every write (EPERM) once it has been
 * reset or otherwise upset; the only way back is a fresh open. */
static bool oss_mvsx_reopen(oss_audio_t *ossaudio)
{
   if (ossaudio->fd >= 0)
      close(ossaudio->fd);

   ossaudio->fd = open(ossaudio->device, O_WRONLY);
   if (ossaudio->fd < 0)
   {
      RARCH_ERR("[OSS] Reopening \"%s\" failed: %s.\n",
            ossaudio->device, strerror(errno));
      return false;
   }

   RARCH_LOG("[OSS] Reopened device \"%s\".\n", ossaudio->device);
   oss_mvsx_set_output_path(ossaudio->fd);
   oss_mvsx_configure(ossaudio);
   ossaudio->knob_level = -1;
   oss_mvsx_knob_poll(ossaudio);
   return true;
}
#endif

static void *oss_init(const char *device,
      unsigned rate, unsigned latency,
      unsigned block_frames,
      unsigned *new_out_rate)
{
   int frags, frag, channels, format, new_rate;
   oss_audio_t *ossaudio  = (oss_audio_t*)calloc(1, sizeof(oss_audio_t));
   const char *oss_device = device ? device : DEFAULT_OSS_DEV;

   if (!ossaudio)
      return NULL;

   if ((ossaudio->fd = open(oss_device, O_WRONLY)) < 0)
   {
      RARCH_ERR("[OSS] Failed to open device \"%s\": %s.\n",
            oss_device, strerror(errno));
      free(ossaudio);
      perror("open");
      return NULL;
   }

   RARCH_LOG("[OSS] Opened device \"%s\".\n", oss_device);

#if defined(MVSX)
   strlcpy(ossaudio->device, oss_device, sizeof(ossaudio->device));
   ossaudio->rate    = rate;
   ossaudio->latency = latency;

   oss_mvsx_set_output_path(ossaudio->fd);
   new_rate = oss_mvsx_configure(ossaudio);

   ossaudio->knob_level = -1;
   {
      const char *env    = getenv("MVSX_VOLUME_KNOB");
      ossaudio->knob_off = env && atoi(env) == 0;
   }
   if (ossaudio->knob_off)
      RARCH_LOG("[OSS] Volume knob disabled (MVSX_VOLUME_KNOB=0).\n");
   oss_mvsx_knob_poll(ossaudio);

   /* Blocking writes, as in the 1.9.0 build: the Actions driver
    * does not implement O_NONBLOCK (it returns ERESTARTSYS). */
   (void)frags;
   (void)frag;
   (void)channels;
   (void)format;
#else
   frags = (latency * rate * 4) / (1000 * (1 << 10));
   frag  = (frags << 16) | 10;

   if (ioctl(ossaudio->fd, SNDCTL_DSP_SETFRAGMENT, &frag) < 0)
      RARCH_WARN("[OSS] Could not set fragment sizes. Latency might not be as expected.\n");

   channels = 2;
   format   = is_little_endian() ? AFMT_S16_LE : AFMT_S16_BE;

   if (ioctl(ossaudio->fd, SNDCTL_DSP_CHANNELS, &channels) < 0)
      goto error;

   if (ioctl(ossaudio->fd, SNDCTL_DSP_SETFMT, &format) < 0)
      goto error;

   new_rate = rate;

   if (ioctl(ossaudio->fd, SNDCTL_DSP_SPEED, &new_rate) < 0)
      goto error;
#endif

   if (new_rate != (int)rate)
   {
      RARCH_WARN("[OSS] Requested sample rate not supported. Adjusting output rate to %d Hz.\n", new_rate);
      *new_out_rate = new_rate;
   }

   return ossaudio;

#if !defined(MVSX)
error:
   close(ossaudio->fd);
   if (ossaudio)
      free(ossaudio);
   perror("ioctl");
   return NULL;
#endif
}

#if defined(MVSX)
/* Upper bound on how long one write may wait for the device to
 * drain before the remaining samples are dropped (microseconds).
 * Normal operation never gets near this: a 512-sample chunk is
 * ~11 ms at 44.1 kHz. */
#define OSS_MVSX_WRITE_TIMEOUT_US 200000

static ssize_t oss_write(void *data, const void *s, size_t len)
{
   oss_audio_t *ossaudio  = (oss_audio_t*)data;
   const uint8_t *buf     = (const uint8_t*)s;
   size_t written         = 0;
   unsigned waited_us     = 0;
   bool reopened          = false;

   if (len == 0)
      return 0;

   /* The device cannot do non-blocking writes, so in fast-forward
    * the audio is simply dropped (the frontend is then paced by
    * video instead of by the audio device). */
   if (ossaudio->nonblock)
      return (ssize_t)len;

   /* Poll the volume knob every ~200 ms of audio (S16 stereo),
    * like the stock software does */
   ossaudio->knob_bytes += len;
   if (ossaudio->knob_bytes >= ossaudio->rate * 4 / 5)
   {
      ossaudio->knob_bytes = 0;
      oss_mvsx_knob_poll(ossaudio);
   }

   while (written < len)
   {
      ssize_t _len = write(ossaudio->fd, buf + written, len - written);

      if (_len < 0)
      {
         if (errno == EINTR)
            continue;

         /* 512 = ERESTARTSYS leaking out of the Actions driver;
          * treat it like "try again" with a bounded wait */
         if (errno == EAGAIN || errno == 512)
         {
            if (waited_us >= OSS_MVSX_WRITE_TIMEOUT_US)
            {
               if (!ossaudio->warned_stall)
               {
                  RARCH_WARN("[OSS] Device not draining; dropping audio to keep the frontend responsive.\n");
                  ossaudio->warned_stall = true;
               }
               return (ssize_t)len;
            }

            usleep(1000);
            waited_us += 1000;
            continue;
         }

         /* Anything else (EPERM after a reset, EBADF, EIO...):
          * reopen the device once, then give up on this chunk. */
         ossaudio->err_count++;
         if (ossaudio->err_count <= 3 || (ossaudio->err_count % 600) == 0)
            RARCH_ERR("[OSS] write failed: %s (%u so far).\n",
                  strerror(errno), ossaudio->err_count);

         if (!reopened && oss_mvsx_reopen(ossaudio))
         {
            reopened = true;
            continue;
         }

         /* Keep the driver alive; returning -1 would disable
          * audio for the rest of the session. */
         return (ssize_t)len;
      }

      written += (size_t)_len;
   }

   return (ssize_t)written;
}
#else
static ssize_t oss_write(void *data, const void *s, size_t len)
{
   ssize_t _len;
   oss_audio_t *ossaudio  = (oss_audio_t*)data;
   if (len == 0)
      return 0;
   if ((_len = write(ossaudio->fd, s, len)) < 0)
   {
      if (errno == EAGAIN && (fcntl(ossaudio->fd, F_GETFL) & O_NONBLOCK))
         return 0;
      return -1;
   }
   return _len;
}
#endif

static bool oss_stop(void *data)
{
   oss_audio_t *ossaudio  = (oss_audio_t*)data;

   /* MVSX: SNDCTL_DSP_RESET leaves the Actions driver refusing all
    * further writes (EPERM) - the "audio stops after opening the
    * menu" issue of the earlier builds. Just stop feeding it. */
#if !defined(RETROFW) && !defined(MVSX)
   if (ioctl(ossaudio->fd, SNDCTL_DSP_RESET, 0) < 0)
      return false;
#endif

   ossaudio->is_paused = true;
   return true;
}

static bool oss_start(void *data, bool is_shutdown)
{
   oss_audio_t *ossaudio  = (oss_audio_t*)data;
   if (!ossaudio)
      return false;
   ossaudio->is_paused = false;
   return true;
}

static bool oss_alive(void *data)
{
   oss_audio_t *ossaudio  = (oss_audio_t*)data;
   return !ossaudio->is_paused;
}

static void oss_set_nonblock_state(void *data, bool state)
{
   int rc;
   oss_audio_t *ossaudio  = (oss_audio_t*)data;

#if defined(MVSX)
   /* The Actions driver cannot do O_NONBLOCK; oss_write() drops
    * audio in fast-forward instead. */
   ossaudio->nonblock = state;
   return;
#endif

   if (state)
      rc =  fcntl(ossaudio->fd, F_SETFL,
            fcntl(ossaudio->fd, F_GETFL) | O_NONBLOCK);
   else
      rc =  fcntl(ossaudio->fd, F_SETFL,
            fcntl(ossaudio->fd, F_GETFL) & (~O_NONBLOCK));
   if (rc != 0)
      RARCH_WARN("[OSS] Could not set nonblocking on OSS file descriptor. Will not be able to fast-forward.\n");
}

static void oss_free(void *data)
{
   oss_audio_t *ossaudio  = (oss_audio_t*)data;

/*RETROFW IOCTL always returns EINVAL; MVSX: see oss_stop() */
#if !defined(RETROFW) && !defined(MVSX)
   if (ioctl(ossaudio->fd, SNDCTL_DSP_RESET, 0) < 0)
      return;
#endif

   if (ossaudio->fd >= 0)
      close(ossaudio->fd);
   free(data);
}

#if !defined(MVSX)
static size_t oss_write_avail(void *data)
{
   audio_buf_info info;
   oss_audio_t *ossaudio  = (oss_audio_t*)data;

   if (ioctl(ossaudio->fd, SNDCTL_DSP_GETOSPACE, &info) < 0)
   {
      RARCH_ERR("[OSS] SNDCTL_DSP_GETOSPACE failed.\n");
      return 0;
   }

   return info.bytes;
}

static size_t oss_buffer_size(void *data)
{
   audio_buf_info info;
   oss_audio_t *ossaudio  = (oss_audio_t*)data;

   if (ioctl(ossaudio->fd, SNDCTL_DSP_GETOSPACE, &info) < 0)
   {
      RARCH_ERR("[OSS] SNDCTL_DSP_GETOSPACE failed.\n");
      return 1; /* Return something non-zero to avoid SIGFPE. */
   }

   return info.fragsize * info.fragstotal;
}
#endif

static bool oss_use_float(void *data)
{
   return false;
}

audio_driver_t audio_oss = {
   oss_init,
   oss_write,
   oss_stop,
   oss_start,
   oss_alive,
   oss_set_nonblock_state,
   oss_free,
   oss_use_float,
   "oss",
   NULL,
   NULL,
#if defined(MVSX)
   /* SNDCTL_DSP_GETOSPACE is not usable on the Actions driver;
    * run with plain blocking writes (as acmeplus' builds do) */
   NULL,
   NULL,
#else
   oss_write_avail,
   oss_buffer_size,
#endif
};
