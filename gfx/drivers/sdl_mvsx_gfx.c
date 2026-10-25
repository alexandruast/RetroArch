/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2011-2017 - Higor Euripedes
 *  Copyright (C) 2019-2021 - James Leaver
 *  Copyright (C)      2021 - John Parton
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

/* SDL 1.2 framebuffer video driver for the SNK Neo Geo MVSX /
 * Arcade Stick Pro (Actions ATM7051, /dev/fb0 through the fbcon
 * backend of libSDL-1.2).
 *
 * Derived from sdl_rs90_gfx.c. Differences:
 *  - output resolution is taken from the configured fullscreen
 *    resolution, else from the current framebuffer mode
 *    (1280x1024 on the MVSX panel, 1280x720 on HDMI);
 *  - real integer upscaling (NEON scalers from the Miyoo Mini port,
 *    as used by acmeplus' RetroArch-ASP) instead of 1:1 centring;
 *  - the framebuffer depth is probed at runtime (32 bpp, 16 bpp as
 *    the fallback); the panel's BGR channel order, which the Actions
 *    fb driver does not report (it advertises RGB masks), is handled
 *    by converting frames before scaling;
 *  - no OpenDingux/IPU specific settings: 'Integer Scale',
 *    'Aspect Ratio', 'Bilinear Filtering' and video_force_aspect
 *    from the normal video settings are used.
 *
 * Environment overrides (for experimenting on the device):
 *  MVSX_FB_BPP=16          try a 16 bpp framebuffer mode (default 32;
 *                          16 gave garbage/flicker on the MVSX)
 *  MVSX_SDL_HWSURFACE=0    draw into an SDL shadow surface that SDL copies
 *                          to the framebuffer on flip, instead of straight
 *                          into the framebuffer (default; the copy costs
 *                          as much as the scaling on the MVSX)
 *  MVSX_SDL_DOUBLEBUF=1    request page flipping (hardware surface only)
 *  MVSX_FB_BGR=0           trust the pixel masks SDL reports instead of
 *                          assuming the BGR panel (colours come out with
 *                          red and blue swapped on the MVSX)
 *  CRT look, all on by default, 0 turns each off (percentages):
 *  MVSX_SCANLINES=65       where a source row is repeated vertically, the
 *                          later copies are written at <pct>% brightness
 *                          (75 = like the Scanline2x filter); unrepeated
 *                          content and the menu stay clean
 *  MVSX_SCANLINE_BOOST=115 content multiplied by <pct>% (saturating) before
 *                          scaling, so scanlines do not just dim the picture
 *  MVSX_MASK=85            aperture mask: every third output column of
 *                          the scanline rows at <pct>% (free, it rides on
 *                          the scanline multiply; needs scanlines on)
 *  MVSX_HBLUR=1            horizontal blur over the content frame before
 *                          scaling (composite video softness, blends dither
 *                          patterns): 1 = light [1 6 1]/8, 2 = [1 2 1]/4,
 *                          3-4 = that many minus one [1 2 1]/4 passes
 *                          Boost and blur work on the content frame (a
 *                          tenth of the output pixels), the mask is folded
 *                          into the scanline pass: per-output-row work is
 *                          the same as with plain scanlines.
 *  MVSX_VIDEO_STATS=0      no diagnostics in the log (by default the
 *                          CPU/framebuffer details are logged at start
 *                          and frame timings + thread CPU use every 10 s)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>

#include <SDL/SDL.h>
#include <SDL/SDL_video.h>

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include <retro_miscellaneous.h>
#include <gfx/video_frame.h>
#include <gfx/scaler/pixconv.h>
#include <string/stdstring.h>
#include <encodings/utf.h>
#include <features/features_cpu.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include "../../verbosity.h"
#include "../../gfx/drivers_font_renderer/bitmap.h"
#include "../../configuration.h"
#include "../../retroarch.h"
#include "../../input/input_driver.h"
#include "../video_driver.h"

#include "mvsx/scaler_neon.h"

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define SDL_MVSX_DEFAULT_WIDTH   1280
#define SDL_MVSX_DEFAULT_HEIGHT  1024
#define SDL_MVSX_NUM_FONT_GLYPHS 256
/* Number of frames to clear fully after a layout change
 * (covers double buffering: both buffers get cleared) */
#define SDL_MVSX_CLEAR_FRAMES    3

typedef struct sdl_mvsx_video sdl_mvsx_video_t;
struct sdl_mvsx_video
{
   retro_time_t last_frame_time;
   retro_time_t ff_frame_time_min;
   SDL_Surface *screen;
   void (*scale_frame16)(sdl_mvsx_video_t *vid,
         uint16_t *src, unsigned width, unsigned height,
         unsigned src_pitch);
   void (*scale_frame32)(sdl_mvsx_video_t *vid,
         uint32_t *src, unsigned width, unsigned height,
         unsigned src_pitch);
   bitmapfont_lut_t *osd_font;
   /* Scratch buffer for pixel format conversion */
   void *conv_buf;
   size_t conv_buf_size;

   /* Point scaler: source column of every output column, rebuilt
    * when the content width or the output width changes */
   uint32_t *xidx;
   unsigned xidx_src_w;
   unsigned xidx_dst_w;
   /* One scaled output row (screen_width pixels, 32 bit): rows are
    * gathered here and memcpy'd to the screen, so repeated rows never
    * read the screen back (slow when it is the framebuffer itself) */
   void *row_buf;
   /* Scanlines done while scaling (MVSX_SCANLINES=<percent>): when a
    * source row is repeated vertically, the later copies are written
    * at scanline_k/256 brightness from this second row buffer. 0 = off. */
   unsigned scanline_k;
   void *row_buf_dark;
   /* CRT look (MVSX_SCANLINE_BOOST, MVSX_MASK, MVSX_HBLUR): content
    * times boost_k/128 (saturating) and hblur'd before scaling; every
    * third output column of the scanline rows times mask_k/128 */
   unsigned boost_k;         /* 128 = off */
   unsigned mask_k;          /* 128 = off */
   unsigned hblur;           /* 0 = off */
   uint8_t *gain_row;        /* scanline-row multiplier per output byte */
   unsigned gain_w;
   unsigned gain_boost;
   unsigned gain_mask;
   void *blur_buf;
   /* Menu (RGUI) texture, RGBA4444 */
   uint16_t *menu_texture;
   size_t menu_texture_size;
   unsigned menu_width;
   unsigned menu_height;
   /* Output mode */
   unsigned screen_width;
   unsigned screen_height;
   unsigned screen_bpp;
   /* Scaling/padding/cropping parameters */
   unsigned content_width;
   unsigned content_height;
   unsigned frame_width;
   unsigned frame_height;
   unsigned frame_padding_x;
   unsigned frame_padding_y;
   unsigned frame_crop_x;
   unsigned frame_crop_y;
   unsigned integer_scale;   /* 0 = non-integer scaling path */
   unsigned clear_frames;
   float aspect_ratio;
   uint32_t font_colour32;
   uint16_t font_colour16;
   bool swap_rb;             /* framebuffer stores red in the low bits */
   bool force_bgr;           /* assume BGR regardless of the reported masks */

   /* Diagnostics (MVSX_VIDEO_STATS, default on): per-frame timings
    * and per-thread CPU use, logged every SDL_MVSX_STATS_FRAMES frames */
   bool stats;
   unsigned stats_frames;
   unsigned stats_dropped;
   uint64_t stats_last_count;
   retro_time_t stats_start;
   retro_time_t stats_blit_sum;
   retro_time_t stats_blit_max;
   retro_time_t stats_flip_sum;
   retro_time_t stats_flip_max;
   retro_time_t stats_fx_sum;    /* part of blit: content-resolution effects */
   retro_time_t stats_fx_max;
   bool sw_surface;
   bool double_buf;
   bool vsync;
   bool keep_aspect;
   bool scale_integer;
   bool smooth;
   bool quitting;
   bool rgb32;
   bool menu_active;
   bool was_in_menu;
};

/* Image interpolation START */

static void sdl_mvsx_scale_generic16(uint16_t *src, uint16_t *dst,
      unsigned w, unsigned h, size_t in_stride, size_t out_stride,
      unsigned n)
{
   unsigned x, y, k;

   for (y = 0; y < h; y++)
   {
      uint16_t *line = dst;
      uint16_t *d    = line;

      for (x = 0; x < w; x++)
      {
         uint16_t p = src[x];
         for (k = 0; k < n; k++)
            *d++ = p;
      }

      for (k = 1; k < n; k++)
         memcpy(line + k * out_stride, line, w * n * sizeof(uint16_t));

      src += in_stride;
      dst += out_stride * n;
   }
}

static void sdl_mvsx_scale_generic32(uint32_t *src, uint32_t *dst,
      unsigned w, unsigned h, size_t in_stride, size_t out_stride,
      unsigned n)
{
   unsigned x, y, k;

   for (y = 0; y < h; y++)
   {
      uint32_t *line = dst;
      uint32_t *d    = line;

      for (x = 0; x < w; x++)
      {
         uint32_t p = src[x];
         for (k = 0; k < n; k++)
            *d++ = p;
      }

      for (k = 1; k < n; k++)
         memcpy(line + k * out_stride, line, w * n * sizeof(uint32_t));

      src += in_stride;
      dst += out_stride * n;
   }
}

static void sdl_mvsx_scale_frame16_integer(sdl_mvsx_video_t *vid,
      uint16_t *src, unsigned width, unsigned height,
      unsigned src_pitch)
{
   unsigned n        = vid->integer_scale ? vid->integer_scale : 1;
   /* 16 bit - divide pitch by 2 */
   size_t in_stride  = (size_t)(src_pitch >> 1);
   size_t out_stride = (size_t)(vid->screen->pitch >> 1);
   /* Manipulate offsets so that padding/crop
    * are applied correctly */
   uint16_t *in_ptr  = src + vid->frame_crop_x + vid->frame_crop_y * in_stride;
   uint16_t *out_ptr = (uint16_t*)(vid->screen->pixels) + vid->frame_padding_x +
         out_stride * vid->frame_padding_y;
   unsigned src_w    = vid->frame_width  / n;
   unsigned src_h    = vid->frame_height / n;

   switch (n)
   {
      case 1:
         scale1x_n16(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      case 2:
         scale2x_n16(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      case 3:
         scale3x_n16(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      case 4:
         scale4x_n16(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      case 5:
         scale5x_n16(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      case 6:
         scale6x_n16(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      default:
         sdl_mvsx_scale_generic16(in_ptr, out_ptr, src_w, src_h,
               in_stride, out_stride, n);
         break;
   }
}

static void sdl_mvsx_scale_frame32_integer(sdl_mvsx_video_t *vid,
      uint32_t *src, unsigned width, unsigned height,
      unsigned src_pitch)
{
   unsigned n        = vid->integer_scale ? vid->integer_scale : 1;
   /* 32 bit - divide pitch by 4 */
   size_t in_stride  = (size_t)(src_pitch >> 2);
   size_t out_stride = (size_t)(vid->screen->pitch >> 2);
   uint32_t *in_ptr  = src + vid->frame_crop_x + vid->frame_crop_y * in_stride;
   uint32_t *out_ptr = (uint32_t*)(vid->screen->pixels) + vid->frame_padding_x +
         out_stride * vid->frame_padding_y;
   unsigned src_w    = vid->frame_width  / n;
   unsigned src_h    = vid->frame_height / n;

   switch (n)
   {
      case 1:
         scale1x_n32(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      case 2:
         scale2x_n32(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      case 3:
         scale3x_n32(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      case 4:
         scale4x_n32(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      case 5:
         scale5x_n32(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      case 6:
         scale6x_n32(in_ptr, out_ptr, src_w, src_h, src_pitch, vid->screen->pitch);
         break;
      default:
         sdl_mvsx_scale_generic32(in_ptr, out_ptr, src_w, src_h,
               in_stride, out_stride, n);
         break;
   }
}

/* Approximate nearest-neighbour scaling using
 * bitshifts and integer math.
 *
 * The source column of every output column is looked up in a table
 * (same 16.16 arithmetic as before, so the picture is unchanged). Each
 * distinct source row is gathered once into a cached row buffer and
 * memcpy'd to every output row it covers; the screen is never read
 * back, so this also works when drawing straight into the framebuffer.
 *
 * Measured on the cabinet: the time is bound by the bytes written
 * (~300 MB/s for streaming stores on this Cortex-A5), not by the
 * per-pixel work, so fewer output bytes is the only real lever. */
static const uint32_t *sdl_mvsx_point_xidx(sdl_mvsx_video_t *vid,
      unsigned src_w, unsigned dst_w)
{
   if (!vid->xidx || (dst_w > vid->screen_width))
      return NULL;

   if ((vid->xidx_src_w != src_w) || (vid->xidx_dst_w != dst_w))
   {
      uint32_t x_step = (((uint32_t)src_w << 16) + 1) / dst_w;
      uint32_t x      = 0;
      unsigned i;

      for (i = 0; i < dst_w; i++, x += x_step)
         vid->xidx[i] = x >> 16;

      vid->xidx_src_w = src_w;
      vid->xidx_dst_w = dst_w;
   }

   return vid->xidx;
}

static void sdl_mvsx_scale_frame16_point(sdl_mvsx_video_t *vid,
      uint16_t *src, unsigned width, unsigned height,
      unsigned src_pitch)
{
   uint32_t y_step        = (((uint32_t)(height) << 16) + 1) / vid->frame_height;
   size_t in_stride       = (size_t)(src_pitch >> 1);
   size_t out_stride      = (size_t)(vid->screen->pitch >> 1);
   uint16_t *out_ptr      = (uint16_t*)(vid->screen->pixels) + vid->frame_padding_x +
         out_stride * vid->frame_padding_y;
   const uint32_t *xidx   = sdl_mvsx_point_xidx(vid, width, vid->frame_width);
   const uint16_t *prev   = NULL;
   uint16_t *row_buf      = (uint16_t*)vid->row_buf;
   unsigned target_w      = vid->frame_width;
   size_t row_bytes       = target_w * sizeof(uint16_t);
   uint32_t y             = 0;
   size_t row;

   if (!xidx || !row_buf)
      return;

   for (row = 0; row < vid->frame_height;
         row++, out_ptr += out_stride, y += y_step)
   {
      const uint16_t *in_ptr = src + (y >> 16) * in_stride;
      uint16_t *out          = row_buf;
      unsigned col           = target_w;

      if (in_ptr != prev)
      {
         prev = in_ptr;

         for (; col >= 4; col -= 4, out += 4, xidx += 4)
         {
            out[0] = in_ptr[xidx[0]];
            out[1] = in_ptr[xidx[1]];
            out[2] = in_ptr[xidx[2]];
            out[3] = in_ptr[xidx[3]];
         }
         for (; col; col--)
            *out++ = in_ptr[*xidx++];

         xidx -= target_w;
      }

      memcpy(out_ptr, row_buf, row_bytes);
   }
}

/* CRT look helpers START */

static int sdl_mvsx_env_int(const char *name, int def)
{
   const char *env = getenv(name);
   return env ? atoi(env) : def;
}

/* One multiplier (x/256) per output byte for the scanline rows: the
 * scanline brightness and, on every third output column, also the
 * aperture mask (free: it rides on the scanline multiply). Rebuilt when
 * the output width or the settings change. */
static const uint8_t *sdl_mvsx_dark_gain_row(sdl_mvsx_video_t *vid,
      unsigned target_w)
{
   if (!vid->gain_row || (target_w > vid->screen_width) || !vid->scanline_k)
      return NULL;

   if ((vid->gain_w != target_w) || (vid->gain_boost != vid->scanline_k) ||
         (vid->gain_mask != vid->mask_k))
   {
      unsigned x, b;

      for (x = 0; x < target_w; x++)
      {
         unsigned g = vid->scanline_k;
         if ((x % 3) == 2)
            g = (g * vid->mask_k) >> 7;
         if (g > 255)
            g = 255;
         for (b = 0; b < 4; b++)
            vid->gain_row[x * 4 + b] = (uint8_t)g;
      }

      vid->gain_w     = target_w;
      vid->gain_boost = vid->scanline_k;
      vid->gain_mask  = vid->mask_k;
   }

   return vid->gain_row;
}

/* One fused pass over a content row: horizontal blur (mode 1: light
 * (l + 6c + r) / 8, mode 2: (l + 2c + r) / 4, 0: none; edges clamped)
 * followed by the boost (k/128, saturating; 0 = none). NEON: one
 * contiguous 16-byte load per 4 pixels, neighbours built with vext, so
 * no unaligned loads (the Cortex-A5 is very slow at those; the first
 * version with three offset loads cost ~40 cycles per pixel). */
#define SDL_MVSX_AVG32(a, b) (((a) & (b)) + ((((a) ^ (b)) >> 1) & 0x7f7f7f7fu))
static INLINE uint32_t sdl_mvsx_fx_px(uint32_t l, uint32_t c, uint32_t r,
      unsigned mode, unsigned k)
{
   uint32_t o = c;

   if (mode)
   {
      o = SDL_MVSX_AVG32(SDL_MVSX_AVG32(l, r), c);
      if (mode == 1)
         o = SDL_MVSX_AVG32(o, c);
   }
   if (k)
   {
      uint8_t *q = (uint8_t*)&o;
      unsigned b;
      for (b = 0; b < 4; b++)
      {
         unsigned v = ((unsigned)q[b] * k) >> 7;
         q[b]       = (v > 255) ? 255 : (uint8_t)v;
      }
   }
   return o;
}

static void sdl_mvsx_fx_row32(uint32_t *dst, const uint32_t *src,
      unsigned n, unsigned mode, unsigned k)
{
   unsigned x = 0;

   if (n < 2)
   {
      if (n)
         *dst = sdl_mvsx_fx_px(*src, *src, *src, mode, k);
      return;
   }

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
   if (n >= 12)
   {
      uint8x8_t vk    = vdup_n_u8((uint8_t)k);
      uint8x16_t prev = vld1q_u8((const uint8_t*)src);
      uint8x16_t cur  = vld1q_u8((const uint8_t*)(src + 4));

      /* pixels 0..3 are done by the scalar code below */
      for (x = 4; x + 8 <= n; x += 4)
      {
         uint8x16_t next = vld1q_u8((const uint8_t*)(src + x + 4));
         uint8x16_t o    = cur;

         if (mode)
         {
            uint8x16_t l = vextq_u8(prev, cur, 12);
            uint8x16_t r = vextq_u8(cur, next, 4);
            o            = vrhaddq_u8(vrhaddq_u8(l, r), cur);
            if (mode == 1)
               o         = vrhaddq_u8(o, cur);
         }
         if (k)
         {
            uint16x8_t lo = vmull_u8(vget_low_u8(o),  vk);
            uint16x8_t hi = vmull_u8(vget_high_u8(o), vk);
            o             = vcombine_u8(vqshrn_n_u16(lo, 7), vqshrn_n_u16(hi, 7));
         }
         vst1q_u8((uint8_t*)(dst + x), o);

         prev = cur;
         cur  = next;
      }

      /* head */
      dst[0] = sdl_mvsx_fx_px(src[0], src[0], src[1], mode, k);
      dst[1] = sdl_mvsx_fx_px(src[0], src[1], src[2], mode, k);
      dst[2] = sdl_mvsx_fx_px(src[1], src[2], src[3], mode, k);
      dst[3] = sdl_mvsx_fx_px(src[2], src[3], src[4], mode, k);
   }
   else
#endif
   {
      dst[0] = sdl_mvsx_fx_px(src[0], src[0], src[1], mode, k);
      x      = 1;
   }

   /* tail */
   for (; x + 1 < n; x++)
      dst[x] = sdl_mvsx_fx_px(src[x - 1], src[x], src[x + 1], mode, k);
   dst[n - 1] = sdl_mvsx_fx_px(src[n - 2], src[n - 1], src[n - 1], mode, k);
}

/* Effects at content resolution on the converted, writable frame:
 * MVSX_HBLUR 1 = one light pass, 2 = one [1 2 1]/4 pass, 3-4 = that
 * many minus one [1 2 1]/4 passes; the boost rides on the last pass. */
static void sdl_mvsx_source_fx(sdl_mvsx_video_t *vid, uint32_t *buf,
      unsigned width, unsigned height, unsigned pitch)
{
   uint32_t *tmp   = (uint32_t*)vid->blur_buf;
   unsigned passes = (vid->hblur <= 2) ? (vid->hblur ? 1 : 0) : (vid->hblur - 1);
   unsigned mode   = (vid->hblur == 1) ? 1 : (vid->hblur ? 2 : 0);
   unsigned boost  = (vid->boost_k != 128) ? vid->boost_k : 0;
   size_t row_len  = width * sizeof(uint32_t);
   uint8_t *row;
   unsigned y, p;

   if (!tmp || (width > vid->screen_width))
      return;

   if (!passes)
   {
      if (!boost)
         return;
      passes = 1;
      mode   = 0;
   }

   for (p = 0; p < passes; p++)
   {
      unsigned k = (p + 1 == passes) ? boost : 0;

      row = (uint8_t*)buf;
      for (y = 0; y < height; y++, row += pitch)
      {
         memcpy(tmp, row, row_len);
         sdl_mvsx_fx_row32((uint32_t*)row, tmp, width, mode, k);
      }
   }
}

/* CRT look helpers END */

/* Scanlines: a source row that is repeated n times vertically has its
 * later copies (the second half of the repeats: 1 of 2, 2 of 4) written
 * from a darkened copy of the row buffer, which is what the Scanline2x
 * filter produces for 2x content. Content that is not repeated (1 row
 * per screen row, e.g. 480-line PSX modes on the 512-line mode) gets
 * no scanlines instead of aliasing them. */
static void sdl_mvsx_darken_row32(uint32_t *dst, const uint32_t *src,
      unsigned n, const uint8_t *gain)
{
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
   for (; n >= 4; n -= 4, src += 4, dst += 4, gain += 16)
   {
      uint8x16_t c  = vld1q_u8((const uint8_t*)src);
      uint8x16_t g  = vld1q_u8(gain);
      uint16x8_t lo = vmull_u8(vget_low_u8(c),  vget_low_u8(g));
      uint16x8_t hi = vmull_u8(vget_high_u8(c), vget_high_u8(g));
      vst1q_u8((uint8_t*)dst,
            vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8)));
   }
#endif
   for (; n; n--, src++, dst++, gain += 4)
   {
      const uint8_t *p = (const uint8_t*)src;
      uint8_t *q       = (uint8_t*)dst;
      unsigned b;
      for (b = 0; b < 4; b++)
         q[b] = (uint8_t)(((unsigned)p[b] * gain[b]) >> 8);
   }
}

/* Writes the current row buffer (or its darkened copy for a scanline)
 * to the screen; 'rep' is how many times this source row was already
 * written, 'n_rep' how many screen rows each source row covers */
static INLINE void sdl_mvsx_emit_row32(sdl_mvsx_video_t *vid,
      uint32_t *out_ptr, size_t row_bytes, unsigned target_w,
      unsigned rep, unsigned n_rep, bool *dark_valid)
{
   if (vid->scanline_k && (n_rep >= 2) && (rep * 2 >= n_rep))
   {
      if (!*dark_valid)
      {
         const uint8_t *gain = sdl_mvsx_dark_gain_row(vid, target_w);
         if (gain)
            sdl_mvsx_darken_row32((uint32_t*)vid->row_buf_dark,
                  (const uint32_t*)vid->row_buf, target_w, gain);
         else
            memcpy(vid->row_buf_dark, vid->row_buf, row_bytes);
         *dark_valid = true;
      }
      memcpy(out_ptr, vid->row_buf_dark, row_bytes);
   }
   else
      memcpy(out_ptr, vid->row_buf, row_bytes);
}

static void sdl_mvsx_scale_frame32_point(sdl_mvsx_video_t *vid,
      uint32_t *src, unsigned width, unsigned height,
      unsigned src_pitch)
{
   uint32_t y_step        = (((uint32_t)(height) << 16) + 1) / vid->frame_height;
   size_t in_stride       = (size_t)(src_pitch >> 2);
   size_t out_stride      = (size_t)(vid->screen->pitch >> 2);
   uint32_t *out_ptr      = (uint32_t*)(vid->screen->pixels) + vid->frame_padding_x +
         out_stride * vid->frame_padding_y;
   const uint32_t *xidx   = sdl_mvsx_point_xidx(vid, width, vid->frame_width);
   const uint32_t *prev   = NULL;
   uint32_t *row_buf      = (uint32_t*)vid->row_buf;
   unsigned target_w      = vid->frame_width;
   size_t row_bytes       = target_w * sizeof(uint32_t);
   /* Scanlines only when every source row covers the same number of
    * screen rows (exact integer vertical scale), and not in the menu:
    * RGUI is stretched to the screen (240 -> 512 rows), so some rows get
    * 2 copies and some 3, and the latter would show two dark lines */
   unsigned n_rep         = (height && !vid->menu_active &&
         ((vid->frame_height % height) == 0)) ? vid->frame_height / height : 0;
   unsigned rep           = 0;
   bool dark_valid        = false;
   uint32_t y             = 0;
   size_t row;

   if (!xidx || !row_buf)
      return;

   for (row = 0; row < vid->frame_height;
         row++, out_ptr += out_stride, y += y_step)
   {
      const uint32_t *in_ptr = src + (y >> 16) * in_stride;
      uint32_t *out          = row_buf;
      unsigned col           = target_w;

      if (in_ptr != prev)
      {
         prev       = in_ptr;
         rep        = 0;
         dark_valid = false;

         for (; col >= 4; col -= 4, out += 4, xidx += 4)
         {
            out[0] = in_ptr[xidx[0]];
            out[1] = in_ptr[xidx[1]];
            out[2] = in_ptr[xidx[2]];
            out[3] = in_ptr[xidx[3]];
         }
         for (; col; col--)
            *out++ = in_ptr[*xidx++];

         xidx -= target_w;
      }
      else
         rep++;

      sdl_mvsx_emit_row32(vid, out_ptr, row_bytes, target_w,
            rep, n_rep, &dark_valid);
   }
}

/* Produces a 50:50 mix of pixels a and b
 * > c.f. "Mixing Packed RGB Pixels Efficiently"
 *        http://blargg.8bitalley.com/info/rgb_mixing.html */
#define SDL_MVSX_PIXEL_AVERAGE_16(a, b) (((a) + (b) + (((a) ^ (b)) & 0x821))   >> 1)
#define SDL_MVSX_PIXEL_AVERAGE_32(a, b) (((a) + (b) + (((a) ^ (b)) & 0x10101)) >> 1)

/* Scales a single horizontal line using approximate
 * linear scaling
 * > c.f. "Image Scaling with Bresenham"
 *        https://www.drdobbs.com/image-scaling-with-bresenham/184405045 */
static void sdl_mvsx_scale_line_bresenham16(uint16_t *target, uint16_t *src,
      unsigned src_width, unsigned target_width,
      unsigned int_part, unsigned fract_part, unsigned midpoint)
{
   unsigned num_pixels = target_width;
   unsigned E = 0;

   if (src_width == target_width)
   {
      memcpy(target, src, target_width * sizeof(uint16_t));
      return;
   }
   else if (target_width > src_width)
      num_pixels--;

   do
   {
      *(target++) = (E >= midpoint) ?
            SDL_MVSX_PIXEL_AVERAGE_16(*src, *(src + 1)) : *src;

      src += int_part;
      E   += fract_part;

      if (E >= target_width)
      {
         E -= target_width;
         src++;
      }
   }
   while (--num_pixels);

   if (target_width > src_width)
      *target = *src;
}

static void sdl_mvsx_scale_frame16_bresenham_horz(sdl_mvsx_video_t *vid,
      uint16_t *src, unsigned width, unsigned height,
      unsigned src_pitch)
{
   size_t in_stride        = (size_t)(src_pitch >> 1);
   size_t out_stride       = (size_t)(vid->screen->pitch >> 1);
   uint16_t *prev_src      = NULL;
   uint16_t *target        = (uint16_t*)(vid->screen->pixels) + vid->frame_padding_x +
         (out_stride * vid->frame_padding_y);
   unsigned target_width   = vid->frame_width;
   unsigned target_height  = vid->frame_height;
   unsigned num_lines      = target_height;
   unsigned int_part       = (height / target_height) * in_stride;
   unsigned fract_part     = height % target_height;
   unsigned E              = 0;
   unsigned col_int_part   = width / target_width;
   unsigned col_fract_part = width % target_width;
   int col_midpoint        = (target_width > width) ?
         ((int)target_width - 3 * ((int)target_width - (int)width)) >> 1 :
               (int)(target_width << 1) - (int)width;

   if (col_midpoint < (int)(target_width >> 1))
      col_midpoint = (int)(target_width >> 1);

   while (num_lines--)
   {
      if (src == prev_src)
         memcpy(target, target - out_stride, target_width * sizeof(uint16_t));
      else
      {
         sdl_mvsx_scale_line_bresenham16(target, src, width, target_width,
               col_int_part, col_fract_part, (unsigned)col_midpoint);
         prev_src = src;
      }

      target += out_stride;
      src    += int_part;
      E      += fract_part;

      if (E >= target_height)
      {
         E   -= target_height;
         src += in_stride;
      }
   }
}

static void sdl_mvsx_scale_line_bresenham32(uint32_t *target, uint32_t *src,
      unsigned src_width, unsigned target_width,
      unsigned int_part, unsigned fract_part, unsigned midpoint)
{
   unsigned num_pixels = target_width;
   unsigned E = 0;

   if (src_width == target_width)
   {
      memcpy(target, src, target_width * sizeof(uint32_t));
      return;
   }
   else if (target_width > src_width)
      num_pixels--;

   do
   {
      *(target++) = (E >= midpoint) ?
            SDL_MVSX_PIXEL_AVERAGE_32(*src, *(src + 1)) : *src;

      src += int_part;
      E   += fract_part;

      if (E >= target_width)
      {
         E -= target_width;
         src++;
      }
   }
   while (--num_pixels);

   if (target_width > src_width)
      *target = *src;
}

static void sdl_mvsx_scale_frame32_bresenham_horz(sdl_mvsx_video_t *vid,
      uint32_t *src, unsigned width, unsigned height,
      unsigned src_pitch)
{
   size_t in_stride        = (size_t)(src_pitch >> 2);
   size_t out_stride       = (size_t)(vid->screen->pitch >> 2);
   uint32_t *prev_src      = NULL;
   uint32_t *target        = (uint32_t*)(vid->screen->pixels) + vid->frame_padding_x +
         (out_stride * vid->frame_padding_y);
   unsigned target_width   = vid->frame_width;
   unsigned target_height  = vid->frame_height;
   unsigned num_lines      = target_height;
   unsigned int_part       = (height / target_height) * in_stride;
   unsigned fract_part     = height % target_height;
   unsigned E              = 0;
   unsigned col_int_part   = width / target_width;
   unsigned col_fract_part = width % target_width;
   int col_midpoint        = (target_width > width) ?
         ((int)target_width - 3 * ((int)target_width - (int)width)) >> 1 :
               (int)(target_width << 1) - (int)width;

   if (col_midpoint < (int)(target_width >> 1))
      col_midpoint = (int)(target_width >> 1);

   while (num_lines--)
   {
      if (src == prev_src)
         memcpy(target, target - out_stride, target_width * sizeof(uint32_t));
      else
      {
         sdl_mvsx_scale_line_bresenham32(target, src, width, target_width,
               col_int_part, col_fract_part, (unsigned)col_midpoint);
         prev_src = src;
      }

      target += out_stride;
      src    += int_part;
      E      += fract_part;

      if (E >= target_height)
      {
         E   -= target_height;
         src += in_stride;
      }
   }
}

static void sdl_mvsx_set_scale_frame_functions(sdl_mvsx_video_t *vid)
{
   if (vid->integer_scale)
   {
      vid->scale_frame16 = sdl_mvsx_scale_frame16_integer;
      vid->scale_frame32 = sdl_mvsx_scale_frame32_integer;
   }
   else if (vid->smooth)
   {
      vid->scale_frame16 = sdl_mvsx_scale_frame16_bresenham_horz;
      vid->scale_frame32 = sdl_mvsx_scale_frame32_bresenham_horz;
   }
   else
   {
      vid->scale_frame16 = sdl_mvsx_scale_frame16_point;
      vid->scale_frame32 = sdl_mvsx_scale_frame32_point;
   }
}

/* Image interpolation END */

/* Pixel format handling START */

/* XRGB8888 -> RGB565 (declared in pixconv.h but not implemented
 * in libretro-common at this version) */
static void sdl_mvsx_conv_argb8888_rgb565(void *output_, const void *input_,
      int width, int height, int out_stride, int in_stride)
{
   int h, w;
   const uint32_t *input = (const uint32_t*)input_;
   uint16_t *output      = (uint16_t*)output_;

   for (h = 0; h < height; h++,
         output += out_stride >> 1, input += in_stride >> 2)
   {
      for (w = 0; w < width; w++)
      {
         uint32_t col = input[w];
         output[w]    = (uint16_t)(
               ((col >> 8) & 0xF800) |
               ((col >> 5) & 0x07E0) |
               ((col >> 3) & 0x001F));
      }
   }
}

static void sdl_mvsx_swap_rb16(uint16_t *p, size_t count)
{
   while (count--)
   {
      uint16_t v = *p;
      *p++ = (uint16_t)(((v & 0x001F) << 11) | (v & 0x07E0) | ((v & 0xF800) >> 11));
   }
}

enum sdl_mvsx_src_fmt
{
   SDL_MVSX_SRC_RGB565 = 0,   /* core frames, 16 bit */
   SDL_MVSX_SRC_XRGB8888,     /* core frames, 32 bit */
   SDL_MVSX_SRC_RGBA4444      /* RGUI menu framebuffer */
};

/* RGUI renders its 16-bit framebuffer as RGBA4444 (R in the top
 * nibble, alpha in the bottom one) for every driver except the
 * OpenDingux ones. Alpha is ignored: the menu replaces the frame. */
static void sdl_mvsx_conv_rgba4444_32(uint32_t *dst, const uint16_t *src,
      unsigned width, unsigned height, unsigned dst_pitch,
      unsigned src_pitch, bool swap_rb)
{
   unsigned x, y;

   for (y = 0; y < height; y++,
         dst = (uint32_t*)((uint8_t*)dst + dst_pitch),
         src = (const uint16_t*)((const uint8_t*)src + src_pitch))
   {
      for (x = 0; x < width; x++)
      {
         uint16_t p = src[x];
         uint32_t r = ((p >> 12) & 0xF) * 17;
         uint32_t g = ((p >>  8) & 0xF) * 17;
         uint32_t b = ((p >>  4) & 0xF) * 17;

         dst[x] = swap_rb ? ((b << 16) | (g << 8) | r)
                          : ((r << 16) | (g << 8) | b);
      }
   }
}

static void sdl_mvsx_conv_rgba4444_16(uint16_t *dst, const uint16_t *src,
      unsigned width, unsigned height, unsigned dst_pitch,
      unsigned src_pitch, bool swap_rb)
{
   unsigned x, y;

   for (y = 0; y < height; y++,
         dst = (uint16_t*)((uint8_t*)dst + dst_pitch),
         src = (const uint16_t*)((const uint8_t*)src + src_pitch))
   {
      for (x = 0; x < width; x++)
      {
         uint16_t p = src[x];
         uint16_t r = (p >> 12) & 0xF;
         uint16_t g = (p >>  8) & 0xF;
         uint16_t b = (p >>  4) & 0xF;
         uint16_t r5 = (r << 1) | (r >> 3);
         uint16_t g6 = (g << 2) | (g >> 2);
         uint16_t b5 = (b << 1) | (b >> 3);

         dst[x] = swap_rb ? (uint16_t)((b5 << 11) | (g6 << 5) | r5)
                          : (uint16_t)((r5 << 11) | (g6 << 5) | b5);
      }
   }
}

/* Converts 'src' into the framebuffer pixel format if required,
 * then scales it onto the screen surface */
static void sdl_mvsx_blit(sdl_mvsx_video_t *vid,
      const void *src, unsigned width, unsigned height,
      unsigned src_pitch, enum sdl_mvsx_src_fmt fmt)
{
   bool dst32     = (vid->screen_bpp == 32);
   bool src_rgb32 = (fmt == SDL_MVSX_SRC_XRGB8888);
   bool need_conv = (fmt == SDL_MVSX_SRC_RGBA4444) ||
         (src_rgb32 != dst32) || vid->swap_rb;

   if (need_conv)
   {
      size_t bpp         = dst32 ? sizeof(uint32_t) : sizeof(uint16_t);
      unsigned dst_pitch = (unsigned)(width * bpp);
      size_t size        = (size_t)dst_pitch * height;

      if (size > vid->conv_buf_size)
      {
         void *buf = realloc(vid->conv_buf, size);
         if (!buf)
            return;
         vid->conv_buf      = buf;
         vid->conv_buf_size = size;
      }

      if (fmt == SDL_MVSX_SRC_RGBA4444)
      {
         if (dst32)
            sdl_mvsx_conv_rgba4444_32((uint32_t*)vid->conv_buf,
                  (const uint16_t*)src, width, height,
                  dst_pitch, src_pitch, vid->swap_rb);
         else
            sdl_mvsx_conv_rgba4444_16((uint16_t*)vid->conv_buf,
                  (const uint16_t*)src, width, height,
                  dst_pitch, src_pitch, vid->swap_rb);
      }
      else if (src_rgb32 && dst32)
         conv_argb8888_abgr8888(vid->conv_buf, src,
               width, height, dst_pitch, src_pitch);
      else if (src_rgb32 && !dst32)
      {
         sdl_mvsx_conv_argb8888_rgb565(vid->conv_buf, src,
               width, height, dst_pitch, src_pitch);
         if (vid->swap_rb)
            sdl_mvsx_swap_rb16((uint16_t*)vid->conv_buf, (size_t)width * height);
      }
      else if (!src_rgb32 && dst32)
      {
         if (vid->swap_rb)
            conv_rgb565_abgr8888(vid->conv_buf, src,
                  width, height, dst_pitch, src_pitch);
         else
            conv_rgb565_argb8888(vid->conv_buf, src,
                  width, height, dst_pitch, src_pitch);
      }
      else
      {
         /* RGB565 -> BGR565 */
         unsigned y;
         const uint8_t *s = (const uint8_t*)src;
         uint8_t *d       = (uint8_t*)vid->conv_buf;

         for (y = 0; y < height; y++)
         {
            memcpy(d, s, dst_pitch);
            sdl_mvsx_swap_rb16((uint16_t*)d, width);
            s += src_pitch;
            d += dst_pitch;
         }
      }

      src       = vid->conv_buf;
      src_pitch = dst_pitch;
   }

   /* Content-resolution effects (horizontal blur, bright-row boost) on
    * the converted, writable frame, before scaling; not for the menu */
   if ((vid->hblur || (vid->boost_k != 128)) && dst32 && !vid->menu_active &&
         (fmt != SDL_MVSX_SRC_RGBA4444) && (src == vid->conv_buf))
   {
      retro_time_t t0 = vid->stats ? cpu_features_get_time_usec() : 0;

      sdl_mvsx_source_fx(vid, (uint32_t*)vid->conv_buf,
            width, height, src_pitch);

      if (vid->stats)
      {
         retro_time_t dt = cpu_features_get_time_usec() - t0;
         vid->stats_fx_sum += dt;
         if (dt > vid->stats_fx_max)
            vid->stats_fx_max = dt;
      }
   }

   if (dst32)
      vid->scale_frame32(vid, (uint32_t*)src, width, height, src_pitch);
   else
      vid->scale_frame16(vid, (uint16_t*)src, width, height, src_pitch);
}

/* Pixel format handling END */

static void sdl_mvsx_init_font_color(sdl_mvsx_video_t *vid)
{
   settings_t *settings = config_get_ptr();
   uint32_t red         = 0xFF;
   uint32_t green       = 0xFF;
   uint32_t blue        = 0xFF;

   if (settings)
   {
      red   = (uint32_t)((settings->floats.video_msg_color_r * 255.0f) + 0.5f) & 0xFF;
      green = (uint32_t)((settings->floats.video_msg_color_g * 255.0f) + 0.5f) & 0xFF;
      blue  = (uint32_t)((settings->floats.video_msg_color_b * 255.0f) + 0.5f) & 0xFF;
   }

   if (vid->swap_rb)
   {
      uint32_t tmp = red;
      red          = blue;
      blue         = tmp;
   }

   /* XRGB8888 (or XBGR8888 after the swap above) */
   vid->font_colour32 = (red << 16) | (green << 8) | blue;

   /* RGB565 */
   vid->font_colour16 = (uint16_t)(((red >> 3) << 11) |
         ((green >> 2) << 5) | (blue >> 3));
}

static void sdl_mvsx_blit_text16(
      sdl_mvsx_video_t *vid,
      unsigned x, unsigned y,
      const char *str)
{
   /* Note: Cannot draw text in padding region
    * (padding region is never cleared, so
    * any text pixels would remain as garbage) */
   uint16_t *screen_buf         = (uint16_t*)vid->screen->pixels;
   bool **font_lut              = vid->osd_font->lut;
   uint16_t screen_stride       = (uint16_t)(vid->screen->pitch >> 1);
   uint16_t screen_width        = vid->screen->w;
   uint16_t screen_height       = vid->screen->h;
   unsigned x_pos               = x + vid->frame_padding_x;
   unsigned y_pos               = (y > (screen_height >> 1)) ?
         (y - vid->frame_padding_y) : (y + vid->frame_padding_y);
   uint16_t shadow_color_buf[2] = {0};
   uint16_t color_buf[2];

   color_buf[0] = vid->font_colour16;
   color_buf[1] = 0;

   if (y_pos + FONT_HEIGHT + 1 >=
         screen_height - vid->frame_padding_y)
      return;

   while (!string_is_empty(str))
   {
      if (x_pos + FONT_WIDTH_STRIDE + 1 >=
            screen_width - vid->frame_padding_x)
         return;

      if (*str == ' ')
         str++;
      else
      {
         uint16_t i, j;
         bool *symbol_lut;
         uint32_t symbol = utf8_walk(&str);

         if (symbol == 339) /* Latin small ligature oe */
            symbol = 156;
         if (symbol == 338) /* Latin capital ligature oe */
            symbol = 140;

         if (symbol >= SDL_MVSX_NUM_FONT_GLYPHS)
            continue;

         symbol_lut = font_lut[symbol];

         for (j = 0; j < FONT_HEIGHT; j++)
         {
            uint32_t buff_offset = ((y_pos + j) * screen_stride) + x_pos;

            for (i = 0; i < FONT_WIDTH; i++)
            {
               if (*(symbol_lut + i + (j * FONT_WIDTH)))
               {
                  uint16_t *screen_buf_ptr = screen_buf + buff_offset + i;

                  memcpy(screen_buf_ptr, color_buf, sizeof(uint16_t));

                  screen_buf_ptr += screen_stride;
                  memcpy(screen_buf_ptr, shadow_color_buf, sizeof(uint16_t));
               }
            }
         }
      }

      x_pos += FONT_WIDTH_STRIDE;
   }
}

static void sdl_mvsx_blit_text32(
      sdl_mvsx_video_t *vid,
      unsigned x, unsigned y,
      const char *str)
{
   uint32_t *screen_buf         = (uint32_t*)vid->screen->pixels;
   bool **font_lut              = vid->osd_font->lut;
   uint32_t screen_stride       = (uint32_t)(vid->screen->pitch >> 2);
   uint32_t screen_width        = vid->screen->w;
   uint32_t screen_height       = vid->screen->h;
   unsigned x_pos               = x + vid->frame_padding_x;
   unsigned y_pos               = (y > (screen_height >> 1)) ?
         (y - vid->frame_padding_y) : (y + vid->frame_padding_y);
   uint32_t shadow_color_buf[2] = {0};
   uint32_t color_buf[2];

   color_buf[0] = vid->font_colour32;
   color_buf[1] = 0;

   if (y_pos + FONT_HEIGHT + 1 >=
         screen_height - vid->frame_padding_y)
      return;

   while (!string_is_empty(str))
   {
      if (x_pos + FONT_WIDTH_STRIDE + 1 >=
            screen_width - vid->frame_padding_x)
         return;

      if (*str == ' ')
         str++;
      else
      {
         uint32_t i, j;
         bool *symbol_lut;
         uint32_t symbol = utf8_walk(&str);

         if (symbol == 339) /* Latin small ligature oe */
            symbol = 156;
         if (symbol == 338) /* Latin capital ligature oe */
            symbol = 140;

         if (symbol >= SDL_MVSX_NUM_FONT_GLYPHS)
            continue;

         symbol_lut = font_lut[symbol];

         for (j = 0; j < FONT_HEIGHT; j++)
         {
            uint32_t buff_offset = ((y_pos + j) * screen_stride) + x_pos;

            for (i = 0; i < FONT_WIDTH; i++)
            {
               if (*(symbol_lut + i + (j * FONT_WIDTH)))
               {
                  uint32_t *screen_buf_ptr = screen_buf + buff_offset + i;

                  memcpy(screen_buf_ptr, color_buf, sizeof(uint32_t));

                  screen_buf_ptr += screen_stride;
                  memcpy(screen_buf_ptr, shadow_color_buf, sizeof(uint32_t));
               }
            }
         }
      }

      x_pos += FONT_WIDTH_STRIDE;
   }
}

static void sdl_mvsx_gfx_free(void *data)
{
   sdl_mvsx_video_t *vid = (sdl_mvsx_video_t*)data;

   if (!vid)
      return;

   if (vid->osd_font)
      bitmapfont_free_lut(vid->osd_font);

   if (vid->conv_buf)
      free(vid->conv_buf);
   if (vid->xidx)
      free(vid->xidx);
   if (vid->row_buf)
      free(vid->row_buf);
   if (vid->row_buf_dark)
      free(vid->row_buf_dark);
   if (vid->gain_row)
      free(vid->gain_row);
   if (vid->blur_buf)
      free(vid->blur_buf);

   if (vid->menu_texture)
      free(vid->menu_texture);

   free(vid);
}

/* Diagnostics START */

/* Frames between two statistics lines (10 s at 60 fps) */
#define SDL_MVSX_STATS_FRAMES   600
#define SDL_MVSX_MAX_THREADS    32

/* Logs a small procfs/sysfs text file, one log line per line */
static void sdl_mvsx_diag_cat(const char *path, const char *tag)
{
   char line[256];
   FILE *f = fopen(path, "r");

   if (!f)
      return;

   while (fgets(line, sizeof(line), f))
   {
      size_t len = strlen(line);
      while (len && ((line[len - 1] == '\n') || (line[len - 1] == '\r')))
         line[--len] = '\0';
      if (len)
         RARCH_LOG("[SDL1] [MVSX] [diag] %s: %s\n", tag, line);
   }

   fclose(f);
}

/* Dumps what we would otherwise need a shell on the device for:
 * CPU type/count/frequency and the framebuffer's capabilities */
static void sdl_mvsx_diag_system(void)
{
   static const struct
   {
      const char *path;
      const char *tag;
   } files[] = {
      { "/proc/cpuinfo",                                              "cpuinfo" },
      { "/sys/devices/system/cpu/online",                             "cpu online" },
      { "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",      "cpu0 governor" },
      { "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",      "cpu0 max kHz" },
      { "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",      "cpu0 cur kHz" },
      { "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq",      "cpu0 min kHz" },
      { "/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies", "cpu0 freqs kHz" },
      { "/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors",   "cpu0 governors" },
      { "/sys/class/graphics/fb0/name",                               "fb0 name" },
      { "/sys/class/graphics/fb0/bits_per_pixel",                     "fb0 bpp" },
      { "/sys/class/graphics/fb0/virtual_size",                       "fb0 virtual_size" },
      { "/sys/class/graphics/fb0/stride",                             "fb0 stride" },
      { "/sys/class/graphics/fb0/mode",                               "fb0 mode" },
      { "/sys/class/graphics/fb0/modes",                              "fb0 modes" },
      { "/sys/class/graphics/fb0/pan",                                "fb0 pan" },
      { "/sys/class/graphics/fb0/state",                              "fb0 state" },
      { "/sys/class/graphics/fb0/rotate",                             "fb0 rotate" },
      { "/sys/class/graphics/fb1/name",                               "fb1 name" },
      { "/proc/meminfo",                                              "meminfo" },
      { "/proc/version",                                              "kernel" },
      { "/proc/cmdline",                                              "cmdline" },
   };
   size_t i;

   for (i = 0; i < ARRAY_SIZE(files); i++)
      sdl_mvsx_diag_cat(files[i].path, files[i].tag);
}

struct sdl_mvsx_thread_sample
{
   int tid;
   unsigned long ticks;
   char comm[20];
};

static struct sdl_mvsx_thread_sample sdl_mvsx_thr_prev[SDL_MVSX_MAX_THREADS];
static unsigned sdl_mvsx_thr_prev_n;

/* Reads utime+stime of every thread of this process */
static unsigned sdl_mvsx_diag_sample_threads(
      struct sdl_mvsx_thread_sample *out, unsigned max)
{
   unsigned n = 0;
   struct dirent *e;
   DIR *d = opendir("/proc/self/task");

   if (!d)
      return 0;

   while ((e = readdir(d)) && (n < max))
   {
      char path[128];
      char buf[512];
      char *p, *q;
      unsigned long ut, st;
      FILE *f;

      if ((e->d_name[0] < '0') || (e->d_name[0] > '9'))
         continue;

      snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
      f = fopen(path, "r");
      if (!f)
         continue;
      if (!fgets(buf, sizeof(buf), f))
      {
         fclose(f);
         continue;
      }
      fclose(f);

      /* pid (comm) state ppid pgrp session tty tpgid flags
       * minflt cminflt majflt cmajflt utime stime ...
       * comm may contain spaces, so locate it by the parentheses */
      p = strchr(buf, '(');
      q = strrchr(buf, ')');
      if (!p || !q)
         continue;
      *q = '\0';
      strlcpy(out[n].comm, p + 1, sizeof(out[n].comm));

      if (sscanf(q + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
               &ut, &st) != 2)
         continue;

      out[n].tid   = atoi(e->d_name);
      out[n].ticks = ut + st;
      n++;
   }

   closedir(d);
   return n;
}

/* Logs the CPU use of every thread since the previous call
 * (percent of one core) */
static void sdl_mvsx_diag_log_threads(retro_time_t interval_us)
{
   struct sdl_mvsx_thread_sample cur[SDL_MVSX_MAX_THREADS];
   unsigned n = sdl_mvsx_diag_sample_threads(cur, SDL_MVSX_MAX_THREADS);
   long hz    = sysconf(_SC_CLK_TCK);
   unsigned i, j;

   if (hz <= 0)
      hz = 100;

   for (i = 0; i < n; i++)
   {
      for (j = 0; j < sdl_mvsx_thr_prev_n; j++)
         if (sdl_mvsx_thr_prev[j].tid == cur[i].tid)
            break;

      if ((j < sdl_mvsx_thr_prev_n) && (interval_us > 0))
      {
         uint64_t dticks = cur[i].ticks - sdl_mvsx_thr_prev[j].ticks;
         unsigned pct    = (unsigned)((dticks * 100 * 1000000) /
               ((uint64_t)hz * (uint64_t)interval_us));

         if (pct)
            RARCH_LOG("[SDL1] [MVSX] [stats] thread %d (%s): %u%% CPU.\n",
                  cur[i].tid, cur[i].comm, pct);
      }
   }

   memcpy(sdl_mvsx_thr_prev, cur, n * sizeof(cur[0]));
   sdl_mvsx_thr_prev_n = n;
}

/* Accounts one presented frame; every SDL_MVSX_STATS_FRAMES frames
 * logs how long conversion+scaling and the SDL flip took, how many
 * core frames never reached the driver (dropped by the threaded
 * video wrapper when we were too slow) and the thread CPU use */
static void sdl_mvsx_stats_frame(sdl_mvsx_video_t *vid, uint64_t frame_count,
      retro_time_t t_start, retro_time_t t_blit, retro_time_t t_flip)
{
   retro_time_t blit = t_blit - t_start;
   retro_time_t flip = t_flip - t_blit;

   if (vid->stats_last_count && (frame_count > vid->stats_last_count + 1))
      vid->stats_dropped += (unsigned)(frame_count - vid->stats_last_count - 1);
   vid->stats_last_count = frame_count;

   if (!vid->stats_frames)
      vid->stats_start = t_start;

   vid->stats_blit_sum += blit;
   if (blit > vid->stats_blit_max)
      vid->stats_blit_max = blit;
   vid->stats_flip_sum += flip;
   if (flip > vid->stats_flip_max)
      vid->stats_flip_max = flip;
   vid->stats_frames++;

   if (vid->stats_frames >= SDL_MVSX_STATS_FRAMES)
   {
      retro_time_t elapsed = t_flip - vid->stats_start;
      unsigned n           = vid->stats_frames;

      RARCH_LOG("[SDL1] [MVSX] [stats] %u frames in %u ms (%.1f fps shown, "
            "%u core frames dropped): conv+scale avg %u max %u us "
            "(of which source fx avg %u max %u us), "
            "flip avg %u max %u us, content %ux%u -> %ux%u (%s%ux).\n",
            n, (unsigned)(elapsed / 1000),
            elapsed > 0 ? (double)n * 1000000.0 / (double)elapsed : 0.0,
            vid->stats_dropped,
            (unsigned)(vid->stats_blit_sum / n), (unsigned)vid->stats_blit_max,
            (unsigned)(vid->stats_fx_sum / n), (unsigned)vid->stats_fx_max,
            (unsigned)(vid->stats_flip_sum / n), (unsigned)vid->stats_flip_max,
            vid->content_width, vid->content_height,
            vid->frame_width, vid->frame_height,
            vid->integer_scale ? "NEON " : "point/linear scaler, integer ",
            vid->integer_scale ? vid->integer_scale :
                  (vid->content_height ? vid->frame_height / vid->content_height : 0));

      sdl_mvsx_diag_log_threads(elapsed);

      vid->stats_frames   = 0;
      vid->stats_dropped  = 0;
      vid->stats_blit_sum = vid->stats_blit_max = 0;
      vid->stats_flip_sum = vid->stats_flip_max = 0;
      vid->stats_fx_sum   = vid->stats_fx_max   = 0;
   }
}

/* Diagnostics END */

/* (Re)applies the SDL video mode. The output resolution and
 * colour depth are fixed for the lifetime of the driver; only
 * the surface flags (vsync) can change. */
static bool sdl_mvsx_set_video_mode(sdl_mvsx_video_t *vid)
{
   uint32_t flags = (vid->sw_surface ? SDL_SWSURFACE : SDL_HWSURFACE) |
         SDL_FULLSCREEN;

   /* Page flipping only on explicit request: on the MVSX fb it
    * yields a flickering mismatched shadow surface */
   if (vid->vsync && !vid->sw_surface && vid->double_buf)
      flags |= SDL_DOUBLEBUF;

   vid->screen = SDL_SetVideoMode(
         vid->screen_width, vid->screen_height,
         vid->screen_bpp, flags);

   if (!vid->screen)
   {
      RARCH_WARN("[SDL1] [MVSX] SDL_SetVideoMode(%ux%u, %u bpp) failed: %s.\n",
            vid->screen_width, vid->screen_height, vid->screen_bpp,
            SDL_GetError());
      return false;
   }

   vid->screen_bpp = vid->screen->format->BitsPerPixel;

   if ((vid->screen_bpp != 16) && (vid->screen_bpp != 32))
   {
      RARCH_ERR("[SDL1] [MVSX] Unsupported framebuffer depth: %u bpp.\n",
            vid->screen_bpp);
      return false;
   }

   /* The MVSX panel is BGR ordered (red in the low byte) but the
    * Actions fb driver reports standard RGB masks, so the SDL format
    * cannot be trusted: swap unconditionally like acmeplus' 1.9.0
    * build does (SCALER_FMT_ABGR8888 output), unless MVSX_FB_BGR=0.
    * A format that does report red in the low bits is BGR anyway. */
   vid->swap_rb      = vid->force_bgr || (vid->screen->format->Rshift == 0);
   vid->clear_frames = SDL_MVSX_CLEAR_FRAMES;

   RARCH_LOG("[SDL1] [MVSX] Video mode %dx%d, %u bpp, pitch %u, masks R=%08x G=%08x B=%08x%s, %s%s.\n",
         vid->screen->w, vid->screen->h, vid->screen_bpp,
         (unsigned)vid->screen->pitch,
         (unsigned)vid->screen->format->Rmask,
         (unsigned)vid->screen->format->Gmask,
         (unsigned)vid->screen->format->Bmask,
         vid->swap_rb ? (vid->force_bgr ? " (BGR panel, swapping)"
                                        : " (BGR order, swapping)") : "",
         (vid->screen->flags & SDL_HWSURFACE) ? "hardware surface" : "shadow surface",
         (vid->screen->flags & SDL_DOUBLEBUF) ? ", double buffered" : "");

   return true;
}

/* Computes frame size/padding/cropping for content
 * of size width x height */
static void sdl_mvsx_set_output(sdl_mvsx_video_t *vid,
      unsigned width, unsigned height)
{
   unsigned sw = vid->screen_width;
   unsigned sh = vid->screen_height;

   if (!width || !height)
   {
      width  = sw;
      height = sh;
   }

   vid->content_width  = width;
   vid->content_height = height;
   vid->integer_scale  = 0;
   vid->frame_crop_x   = 0;
   vid->frame_crop_y   = 0;

   bool use_integer = vid->scale_integer;
   bool keep_aspect = vid->keep_aspect;

   /* The menu follows RGUI's 'Lock Menu Aspect Ratio' setting, like
    * on every other driver: Off/Fill Screen stretch it over the
    * whole panel, Fit Screen keeps square pixels, Integer Scale
    * uses whole multiples. */
   if (vid->menu_active)
   {
      settings_t *settings = config_get_ptr();
      unsigned lock        = settings ?
            settings->uints.menu_rgui_aspect_ratio_lock :
            RGUI_ASPECT_RATIO_LOCK_NONE;

      switch (lock)
      {
         case RGUI_ASPECT_RATIO_LOCK_INTEGER:
            use_integer = true;
            keep_aspect = true;
            break;
         case RGUI_ASPECT_RATIO_LOCK_FIT_SCREEN:
            use_integer = false;
            keep_aspect = true;
            break;
         case RGUI_ASPECT_RATIO_LOCK_NONE:
         case RGUI_ASPECT_RATIO_LOCK_FILL_SCREEN:
         default:
            use_integer = false;
            keep_aspect = false;
            break;
      }
   }

   /* Integer scaling only when at least 1x fits; taller content
    * (e.g. a 4x filter on a small screen) falls through to the
    * aspect-fit downscaling path instead of being cropped. */
   if (use_integer && (sh / height) >= 1)
   {
      unsigned ny = sh / height;

      {
         /* Like RetroArch's own integer scaling: the height is an
          * integer multiple, the width follows the aspect ratio
          * (menu: square pixels). If the width also happens to be
          * an exact multiple, the NEON NxN path is used. */
         float aspect = vid->menu_active ?
               ((float)width / (float)height) : vid->aspect_ratio;

         if (aspect <= 0.0f)
            aspect = (float)width / (float)height;

         for (;;)
         {
            vid->frame_height = height * ny;
            vid->frame_width  = (unsigned)(((float)vid->frame_height * aspect) + 0.5f) & ~1u;

            if ((vid->frame_width <= sw) || (ny == 1))
               break;
            ny--;
         }

         if (vid->frame_width > sw)
            vid->frame_width = sw;
         if (vid->frame_width < 2)
            vid->frame_width = 2;

         vid->integer_scale = (vid->frame_width == width * ny) ? ny : 0;
      }
   }
   else
   {
      if (keep_aspect)
      {
         /* The menu is drawn with square pixels; content
          * follows the configured aspect ratio */
         float aspect = vid->menu_active ?
               ((float)width / (float)height) : vid->aspect_ratio;

         if (aspect <= 0.0f)
            aspect = (float)width / (float)height;

         if (((float)sh * aspect) <= (float)sw)
         {
            vid->frame_height = sh;
            vid->frame_width  = (unsigned)(((float)sh * aspect) + 0.5f);
         }
         else
         {
            vid->frame_width  = sw;
            vid->frame_height = (unsigned)(((float)sw / aspect) + 0.5f);
         }

         if (vid->frame_width > sw)
            vid->frame_width = sw;
         if (vid->frame_height > sh)
            vid->frame_height = sh;

         /* Keep the frame width even (16 bpp alignment) */
         vid->frame_width &= ~1u;

         if (vid->frame_width < 2)
            vid->frame_width = 2;
         if (vid->frame_height < 1)
            vid->frame_height = 1;
      }
      else
      {
         vid->frame_width  = sw;
         vid->frame_height = sh;
      }

      /* Fast path: nothing to scale */
      if ((vid->frame_width == width) && (vid->frame_height == height))
         vid->integer_scale = 1;
   }

   vid->frame_padding_x = (sw - vid->frame_width)  >> 1;
   vid->frame_padding_y = (sh - vid->frame_height) >> 1;

   sdl_mvsx_set_scale_frame_functions(vid);

   /* Padding region must be cleared */
   vid->clear_frames = SDL_MVSX_CLEAR_FRAMES;
}

static void sdl_mvsx_input_driver_init(
      const char *input_drv_name, const char *joypad_drv_name,
      input_driver_t **input, void **input_data)
{
   if (!input || !input_data)
      return;

   *input      = NULL;
   *input_data = NULL;

   /* Only the SDL input driver is tied to this video driver;
    * anything else (linuxraw, udev...) is initialised by the
    * frontend as usual */
   if (string_is_equal(input_drv_name, "sdl"))
   {
      *input_data = input_driver_init_wrap(&input_sdl, joypad_drv_name);

      if (*input_data)
         *input = &input_sdl;
   }
}

static void *sdl_mvsx_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   sdl_mvsx_video_t *vid        = NULL;
   uint32_t sdl_subsystem_flags = SDL_WasInit(0);
   settings_t *settings         = config_get_ptr();
   const SDL_VideoInfo *info    = NULL;
   const char *env              = NULL;
   unsigned preferred_bpp       = 32;

   /* Initialise graphics subsystem, if required */
   if (sdl_subsystem_flags == 0)
   {
      if (SDL_Init(SDL_INIT_VIDEO) < 0)
         return NULL;
   }
   else if ((sdl_subsystem_flags & SDL_INIT_VIDEO) == 0)
   {
      if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
         return NULL;
   }

   vid = (sdl_mvsx_video_t*)calloc(1, sizeof(*vid));
   if (!vid)
      return NULL;

   /* Output resolution: configured fullscreen size if set,
    * otherwise whatever the framebuffer is currently running */
   info               = SDL_GetVideoInfo();
   vid->screen_width  = video->width;
   vid->screen_height = video->height;

   if (info)
      RARCH_LOG("[SDL1] [MVSX] Current framebuffer mode: %dx%d, %u bpp.\n",
            info->current_w, info->current_h,
            info->vfmt ? info->vfmt->BitsPerPixel : 0);

   if (!vid->screen_width || !vid->screen_height)
   {
      if (info && (info->current_w > 0) && (info->current_h > 0))
      {
         vid->screen_width  = (unsigned)info->current_w;
         vid->screen_height = (unsigned)info->current_h;
      }
      else
      {
         vid->screen_width  = SDL_MVSX_DEFAULT_WIDTH;
         vid->screen_height = SDL_MVSX_DEFAULT_HEIGHT;
      }
   }

   /* Defaults match what works on the MVSX framebuffer: 32 bpp, no
    * double buffering. 16 bpp gives a mismatched shadow surface with
    * garbage/flicker and leaves the fb in a bad state. */
   env = getenv("MVSX_FB_BPP");
   if (env && (atoi(env) == 16))
      preferred_bpp = 16;

   /* Draw straight into the framebuffer: on the MVSX that halves the
    * bytes written per frame (no shadow surface copy) and fb writes are
    * no slower than RAM writes (measured 4.7 ms instead of 8.1 + 6.8 ms
    * for a 1194x448 frame). No tearing was visible. MVSX_SDL_HWSURFACE=0
    * restores the shadow surface. */
   vid->sw_surface = false;
   env = getenv("MVSX_SDL_HWSURFACE");
   if (env && (atoi(env) == 0))
      vid->sw_surface = true;

   env = getenv("MVSX_SDL_DOUBLEBUF");
   if (env && (atoi(env) != 0))
      vid->double_buf = true;

   /* The panel is BGR but the fb driver advertises RGB masks */
   vid->force_bgr = true;
   env = getenv("MVSX_FB_BGR");
   if (env && (atoi(env) == 0))
      vid->force_bgr = false;

   /* Timing/CPU statistics in the log, on unless MVSX_VIDEO_STATS=0 */
   vid->stats = true;
   env = getenv("MVSX_VIDEO_STATS");
   if (env && (atoi(env) == 0))
      vid->stats = false;
   if (vid->stats)
      sdl_mvsx_diag_system();

   /* CRT look, on by default; each setting is a percentage, 0 turns
    * it off (see the header). The defaults were chosen on the panel. */
   {
      int scan  = sdl_mvsx_env_int("MVSX_SCANLINES",      65);
      int boost = sdl_mvsx_env_int("MVSX_SCANLINE_BOOST", 115);
      int mask  = sdl_mvsx_env_int("MVSX_MASK",           85);
      int blur  = sdl_mvsx_env_int("MVSX_HBLUR",          1);

      vid->scanline_k = ((scan > 0) && (scan < 100)) ? (unsigned)(scan * 256 / 100) : 0;
      vid->boost_k    = ((boost > 100) && (boost <= 199)) ? (unsigned)(boost * 128 / 100) : 128;
      vid->mask_k     = ((mask > 0) && (mask < 100)) ? (unsigned)(mask * 128 / 100) : 128;
      vid->hblur      = ((blur > 0) && (blur <= 4)) ? (unsigned)blur : 0;

      RARCH_LOG("[SDL1] [MVSX] CRT look (0 = off): scanlines %u%%, bright rows "
            "x%u%%, mask %u%%, horizontal blur %u pass(es).\n",
            vid->scanline_k * 100 / 256,
            vid->boost_k * 100 / 128,
            (vid->mask_k != 128) ? vid->mask_k * 100 / 128 : 0,
            vid->hblur);
   }

   vid->ff_frame_time_min = 16667;
   vid->rgb32             = video->rgb32;
   vid->vsync             = video->vsync;
   vid->smooth            = video->smooth;
   vid->keep_aspect       = video->force_aspect;
   vid->scale_integer     = settings ? settings->bools.video_scale_integer : false;
   vid->aspect_ratio      = video_driver_get_aspect_ratio();
   vid->menu_active       = false;
   vid->was_in_menu       = false;
   vid->quitting          = false;
   vid->last_frame_time   = 0;

   vid->screen_bpp = preferred_bpp;
   if (!sdl_mvsx_set_video_mode(vid))
   {
      vid->screen_bpp = (preferred_bpp == 16) ? 32 : 16;
      RARCH_WARN("[SDL1] [MVSX] Retrying with %u bpp.\n", vid->screen_bpp);

      if (!sdl_mvsx_set_video_mode(vid))
      {
         RARCH_ERR("[SDL1] [MVSX] Failed to init SDL surface: %s.\n", SDL_GetError());
         goto error;
      }
   }

   SDL_ShowCursor(SDL_DISABLE);

   if (settings)
      sdl_mvsx_input_driver_init(
            settings->arrays.input_driver,
            settings->arrays.input_joypad_driver,
            input, input_data);
   else if (input && input_data)
   {
      *input      = NULL;
      *input_data = NULL;
   }

   /* Initialise OSD font */
   sdl_mvsx_init_font_color(vid);

   vid->osd_font = bitmapfont_get_lut();

   if (!vid->osd_font ||
       vid->osd_font->glyph_max <
            (SDL_MVSX_NUM_FONT_GLYPHS - 1))
   {
      RARCH_ERR("[SDL1] [MVSX] Failed to init OSD font.\n");
      goto error;
   }

   vid->xidx         = (uint32_t*)malloc(vid->screen_width * sizeof(uint32_t));
   vid->row_buf      = malloc(vid->screen_width * sizeof(uint32_t));
   vid->row_buf_dark = malloc(vid->screen_width * sizeof(uint32_t));
   vid->gain_row     = (uint8_t*)malloc(vid->screen_width * sizeof(uint32_t));
   vid->blur_buf     = malloc(vid->screen_width * sizeof(uint32_t));
   if (!vid->xidx || !vid->row_buf || !vid->row_buf_dark ||
         !vid->gain_row || !vid->blur_buf)
   {
      RARCH_ERR("[SDL1] [MVSX] Failed to allocate the scaler buffers.\n");
      goto error;
   }

   sdl_mvsx_set_output(vid, vid->screen_width, vid->screen_height);

   return vid;

error:
   sdl_mvsx_gfx_free(vid);
   return NULL;
}

static bool sdl_mvsx_gfx_frame(void *data, const void *frame,
      unsigned width, unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   sdl_mvsx_video_t* vid = (sdl_mvsx_video_t*)data;
#ifdef HAVE_MENU
   bool menu_is_alive    = (video_info->menu_st_flags & MENU_ST_FLAG_ALIVE) ? true : false;
#endif

   if (unlikely(!vid || !vid->screen))
      return true;

   /* If fast forward is currently active, we may
    * push frames at an 'unlimited' rate. Since the
    * display has a fixed refresh rate of 60 Hz, this
    * represents wasted effort. We therefore drop any
    * 'excess' frames in this case. */
   if (unlikely(video_info->input_driver_nonblock_state))
   {
      retro_time_t current_time = cpu_features_get_time_usec();

      if ((current_time - vid->last_frame_time) <
            vid->ff_frame_time_min)
         return true;

      vid->last_frame_time = current_time;
   }

#ifdef HAVE_MENU
   menu_driver_frame(menu_is_alive, video_info);
#endif

   if (likely(!vid->menu_active))
   {
      /* Update layout if we were in the menu on the
       * previous frame, or width/height have changed */
      if (unlikely(
            vid->was_in_menu ||
            (vid->content_width  != width) ||
            (vid->content_height != height)))
         sdl_mvsx_set_output(vid, width, height);

      vid->was_in_menu = false;
   }
   else
   {
      if (!vid->was_in_menu ||
          (vid->content_width  != vid->menu_width) ||
          (vid->content_height != vid->menu_height))
      {
         sdl_mvsx_set_output(vid, vid->menu_width, vid->menu_height);
         vid->was_in_menu = true;
      }
   }

   retro_time_t t_start = vid->stats ? cpu_features_get_time_usec() : 0;
   retro_time_t t_blit  = 0;

   /* Must always lock SDL surface before
    * manipulating raw pixel buffer */
   if (SDL_MUSTLOCK(vid->screen))
      SDL_LockSurface(vid->screen);

   if (unlikely(vid->clear_frames > 0))
   {
      memset(vid->screen->pixels, 0,
            vid->screen->pitch * vid->screen->h);
      vid->clear_frames--;
   }

   if (likely(!vid->menu_active))
   {
      if (likely(frame))
         sdl_mvsx_blit(vid, frame, width, height, pitch,
               vid->rgb32 ? SDL_MVSX_SRC_XRGB8888 : SDL_MVSX_SRC_RGB565);
   }
   else if (vid->menu_texture && vid->menu_width && vid->menu_height)
      sdl_mvsx_blit(vid, vid->menu_texture,
            vid->menu_width, vid->menu_height,
            vid->menu_width * sizeof(uint16_t), SDL_MVSX_SRC_RGBA4444);

   if (vid->stats)
      t_blit = cpu_features_get_time_usec();

   /* Print OSD text, if required */
   if (msg)
   {
      if (vid->screen_bpp == 32)
         sdl_mvsx_blit_text32(vid, FONT_WIDTH_STRIDE,
               vid->screen->h - (FONT_HEIGHT + FONT_WIDTH_STRIDE), msg);
      else
         sdl_mvsx_blit_text16(vid, FONT_WIDTH_STRIDE,
               vid->screen->h - (FONT_HEIGHT + FONT_WIDTH_STRIDE), msg);
   }

   if (SDL_MUSTLOCK(vid->screen))
      SDL_UnlockSurface(vid->screen);

   SDL_Flip(vid->screen);

   if (vid->stats && !vid->menu_active)
      sdl_mvsx_stats_frame(vid, frame_count, t_start, t_blit,
            cpu_features_get_time_usec());

   return true;
}

static void sdl_mvsx_set_texture_enable(void *data, bool state, bool full_screen)
{
   sdl_mvsx_video_t *vid = (sdl_mvsx_video_t*)data;

   if (unlikely(!vid))
      return;

   vid->menu_active = state;
}

static void sdl_mvsx_set_texture_frame(void *data, const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha)
{
   sdl_mvsx_video_t *vid = (sdl_mvsx_video_t*)data;
   size_t size;

   if (unlikely(!vid || rgb32 || !frame || !width || !height))
      return;

   size = (size_t)width * height * sizeof(uint16_t);

   if (size > vid->menu_texture_size)
   {
      uint16_t *buf = (uint16_t*)realloc(vid->menu_texture, size);
      if (!buf)
         return;
      vid->menu_texture      = buf;
      vid->menu_texture_size = size;
   }

   memcpy(vid->menu_texture, frame, size);
   vid->menu_width  = width;
   vid->menu_height = height;
}

static void sdl_mvsx_gfx_set_nonblock_state(void *data, bool toggle,
      bool adaptive_vsync_enabled, unsigned swap_interval)
{
   sdl_mvsx_video_t *vid = (sdl_mvsx_video_t*)data;
   bool vsync            = !toggle;

   if (unlikely(!vid))
      return;

   if (vid->vsync != vsync)
   {
      vid->vsync = vsync;

      /* Re-apply video mode with new surface flags */
      if (!sdl_mvsx_set_video_mode(vid))
         RARCH_ERR("[SDL1] [MVSX] Failed to toggle vsync: %s.\n", SDL_GetError());

      sdl_mvsx_set_output(vid, vid->content_width, vid->content_height);
   }
}

static void sdl_mvsx_gfx_check_window(sdl_mvsx_video_t *vid)
{
   SDL_Event event;

   SDL_PumpEvents();
   while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_QUITMASK))
   {
      if (event.type != SDL_QUIT)
         continue;

      vid->quitting = true;
      break;
   }
}

static bool sdl_mvsx_gfx_alive(void *data)
{
   sdl_mvsx_video_t *vid = (sdl_mvsx_video_t*)data;

   if (unlikely(!vid))
      return false;

   sdl_mvsx_gfx_check_window(vid);
   return !vid->quitting;
}

static bool sdl_mvsx_gfx_focus(void *data)
{
   return true;
}

static bool sdl_mvsx_gfx_suppress_screensaver(void *data, bool enable)
{
   return false;
}

static bool sdl_mvsx_gfx_has_windowed(void *data)
{
   return false;
}

static void sdl_mvsx_gfx_viewport_info(void *data, struct video_viewport *vp)
{
   sdl_mvsx_video_t *vid = (sdl_mvsx_video_t*)data;

   if (unlikely(!vid))
      return;

   vp->x           = (int)vid->frame_padding_x;
   vp->y           = (int)vid->frame_padding_y;
   vp->width       = vid->frame_width;
   vp->height      = vid->frame_height;
   vp->full_width  = vid->screen_width;
   vp->full_height = vid->screen_height;
}

static float sdl_mvsx_get_refresh_rate(void *data)
{
   return 60.0f;
}

static void sdl_mvsx_set_filtering(void *data, unsigned index, bool smooth, bool ctx_scaling)
{
   sdl_mvsx_video_t *vid = (sdl_mvsx_video_t*)data;

   if (!vid)
      return;

   if (vid->smooth != smooth)
   {
      vid->smooth = smooth;
      sdl_mvsx_set_scale_frame_functions(vid);
   }
}

static void sdl_mvsx_apply_state_changes(void *data)
{
   sdl_mvsx_video_t *vid  = (sdl_mvsx_video_t*)data;
   settings_t *settings   = config_get_ptr();
   bool keep_aspect       = (settings) ? settings->bools.video_force_aspect : true;
   bool integer_scaling   = (settings) ? settings->bools.video_scale_integer : false;
   float aspect_ratio     = video_driver_get_aspect_ratio();

   if (!vid)
      return;

   if ((vid->keep_aspect != keep_aspect) ||
       (vid->scale_integer != integer_scaling) ||
       (vid->aspect_ratio != aspect_ratio))
   {
      vid->keep_aspect   = keep_aspect;
      vid->scale_integer = integer_scaling;
      vid->aspect_ratio  = aspect_ratio;

      sdl_mvsx_set_output(vid, vid->content_width, vid->content_height);
   }
}

static void sdl_mvsx_set_aspect_ratio(void *data, unsigned aspect_ratio_idx)
{
   sdl_mvsx_video_t *vid = (sdl_mvsx_video_t*)data;

   if (!vid)
      return;

   vid->aspect_ratio = video_driver_get_aspect_ratio();
   sdl_mvsx_set_output(vid, vid->content_width, vid->content_height);
}

static uint32_t sdl_mvsx_get_flags(void *data)
{
   return 0;
}

static const video_poke_interface_t sdl_mvsx_poke_interface = {
   sdl_mvsx_get_flags,
   NULL, /* load_texture */
   NULL, /* unload_texture */
   NULL, /* set_video_mode */
   sdl_mvsx_get_refresh_rate,
   sdl_mvsx_set_filtering,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_current_framebuffer */
   NULL, /* get_proc_address */
   sdl_mvsx_set_aspect_ratio,
   sdl_mvsx_apply_state_changes,
   sdl_mvsx_set_texture_frame,
   sdl_mvsx_set_texture_enable,
   NULL, /* set_osd_msg */
   NULL, /* show_mouse */
   NULL, /* grab_mouse_toggle */
   NULL, /* get_current_shader */
   NULL, /* get_current_software_framebuffer */
   NULL, /* get_hw_render_interface */
   NULL, /* set_hdr_max_nits */
   NULL, /* set_hdr_paper_white_nits */
   NULL, /* set_hdr_contrast */
   NULL  /* set_hdr_expand_gamut */
};

static void sdl_mvsx_get_poke_interface(void *data, const video_poke_interface_t **iface)
{
   *iface = &sdl_mvsx_poke_interface;
}

static bool sdl_mvsx_gfx_set_shader(void *data,
      enum rarch_shader_type type, const char *path)
{
   return false;
}

video_driver_t video_sdl_mvsx = {
   sdl_mvsx_gfx_init,
   sdl_mvsx_gfx_frame,
   sdl_mvsx_gfx_set_nonblock_state,
   sdl_mvsx_gfx_alive,
   sdl_mvsx_gfx_focus,
   sdl_mvsx_gfx_suppress_screensaver,
   sdl_mvsx_gfx_has_windowed,
   sdl_mvsx_gfx_set_shader,
   sdl_mvsx_gfx_free,
   "sdl_mvsx",
   NULL, /* set_viewport */
   NULL, /* set_rotation */
   sdl_mvsx_gfx_viewport_info,
   NULL, /* read_viewport  */
   NULL, /* read_frame_raw */
#ifdef HAVE_OVERLAY
   NULL, /* get_overlay_interface */
#endif
   sdl_mvsx_get_poke_interface,
   NULL, /* wrap_type_to_enum */
#ifdef HAVE_GFX_WIDGETS
   NULL  /* gfx_widgets_enabled */
#endif
};
