/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.


    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/openmp_maths.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <strings.h>

DT_MODULE_INTROSPECTION(2, dt_iop_rawdenoise_params_t)

#define DT_IOP_RAWDENOISE_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_IOP_RAWDENOISE_RES 64
#define DT_IOP_RAWDENOISE_BANDS 5

typedef enum dt_iop_rawdenoise_channel_t
{
  DT_RAWDENOISE_ALL = 0,
  DT_RAWDENOISE_R = 1,
  DT_RAWDENOISE_G = 2,
  DT_RAWDENOISE_B = 3,
  DT_RAWDENOISE_NONE = 4
} dt_iop_rawdenoise_channel_t;

typedef struct dt_iop_rawdenoise_params_t
{
  float threshold; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.01 $DESCRIPTION: "noise threshold"
  float x[DT_RAWDENOISE_NONE][DT_IOP_RAWDENOISE_BANDS];
  float y[DT_RAWDENOISE_NONE][DT_IOP_RAWDENOISE_BANDS]; // $DEFAULT: 0.5
} dt_iop_rawdenoise_params_t;

typedef struct dt_iop_rawdenoise_gui_data_t
{
  dt_draw_curve_t *transition_curve; // curve for gui to draw

  GtkWidget *threshold;
  GtkDrawingArea *area;
  GtkNotebook *channel_tabs;
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_rawdenoise_params_t drag_params;
  int dragging;
  int x_move;
  dt_iop_rawdenoise_channel_t channel;
  float draw_xs[DT_IOP_RAWDENOISE_RES], draw_ys[DT_IOP_RAWDENOISE_RES];
  float draw_min_xs[DT_IOP_RAWDENOISE_RES], draw_min_ys[DT_IOP_RAWDENOISE_RES];
  float draw_max_xs[DT_IOP_RAWDENOISE_RES], draw_max_ys[DT_IOP_RAWDENOISE_RES];
} dt_iop_rawdenoise_gui_data_t;

typedef struct dt_iop_rawdenoise_data_t
{
  float threshold;
  dt_draw_curve_t *curve[DT_RAWDENOISE_NONE];
  dt_iop_rawdenoise_channel_t channel;
  float force[DT_RAWDENOISE_NONE][DT_IOP_RAWDENOISE_BANDS];
} dt_iop_rawdenoise_data_t;

typedef struct dt_iop_rawdenoise_global_data_t
{
} dt_iop_rawdenoise_global_data_t;

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    // Since first version, the dt_iop_params_t struct have new members
    // at the end of the struct.
    // Yet, the beginning of the struct is exactly the same:
    // threshold is still the first member of the struct.
    // This allows to define the variable o with dt_iop_rawdenoise_params_t
    // as long as we don't try to access new members on o.
    // In other words, o can be seen as a dt_iop_rawdenoise_params_t
    // with no allocated space for the new member.
    dt_iop_rawdenoise_params_t *o = (dt_iop_rawdenoise_params_t *)old_params;
    dt_iop_rawdenoise_params_t *n = (dt_iop_rawdenoise_params_t *)new_params;
    n->threshold = o->threshold;
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
    {
      for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++)
      {
        n->x[ch][k] = k / (DT_IOP_RAWDENOISE_BANDS - 1.0);
        n->y[ch][k] = 0.5f;
      }
    }
    return 0;
  }
  return 1;
}


const char *name()
{
  return _("raw denoise");
}

const char *description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("denoise the raw picture early in the pipeline"),
                                      _("corrective"),
                                      _("linear, raw, scene-referred"),
                                      _("linear, raw"),
                                      _("linear, raw, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_RAW;
}

// transposes image, it is faster to read columns than to write them.
static void hat_transform(float *temp, const float *const base, int stride, int size, int scale)
{
  int i;
  const float *basep0;
  const float *basep1;
  const float *basep2;
  const size_t stxsc = (size_t)stride * scale;

  basep0 = base;
  basep1 = base + stxsc;
  basep2 = base + stxsc;

  for(i = 0; i < scale; i++, basep0 += stride, basep1 -= stride, basep2 += stride)
    temp[i] = (*basep0 + *basep0 + *basep1 + *basep2) * 0.25f;

  for(; i < size - scale; i++, basep0 += stride)
    temp[i] = ((*basep0) * 2 + *(basep0 - stxsc) + *(basep0 + stxsc)) * 0.25f;

  basep1 = basep0 - stxsc;
  basep2 = base + stride * (size - 2);

  for(; i < size; i++, basep0 += stride, basep1 += stride, basep2 -= stride)
    temp[i] = (*basep0 + *basep0 + *basep1 + *basep2) * 0.25f;
}

#define BIT16 65536.0

static void compute_channel_noise(float *const noise, int color, const dt_iop_rawdenoise_data_t *const data)
{
  // note that these constants are the same for X-Trans and Bayer, as they are proportional to image detail on
  // each channel, not the sensor pattern
  static const float noise_all[] = { 0.8002, 0.2735, 0.1202, 0.0585, 0.0291, 0.0152, 0.0080, 0.0044 };
  for(int i = 0; i < DT_IOP_RAWDENOISE_BANDS; i++)
  {
    // scale the value from [0,1] to [0,16],
    // and makes the "0.5" neutral value become 1
    float chan_threshold_exp_4;
    switch(color)
    {
    case 0:
      chan_threshold_exp_4 = data->force[DT_RAWDENOISE_R][DT_IOP_RAWDENOISE_BANDS - i - 1];
      break;
    case 2:
      chan_threshold_exp_4 = data->force[DT_RAWDENOISE_B][DT_IOP_RAWDENOISE_BANDS - i - 1];
      break;
    default:
      chan_threshold_exp_4 = data->force[DT_RAWDENOISE_G][DT_IOP_RAWDENOISE_BANDS - i - 1];
      break;
    }
    chan_threshold_exp_4 *= chan_threshold_exp_4;
    chan_threshold_exp_4 *= chan_threshold_exp_4;
    // repeat for the overall all-channels thresholds
    float all_threshold_exp_4 = data->force[DT_RAWDENOISE_ALL][DT_IOP_RAWDENOISE_BANDS - i - 1];
    all_threshold_exp_4 *= all_threshold_exp_4;
    all_threshold_exp_4 *= all_threshold_exp_4;
    noise[i] = noise_all[i] * all_threshold_exp_4 * chan_threshold_exp_4 * 16.0f * 16.0f;
    // the following multiplication needs to stay separate from the above line, because merging the two changes
    // the results on the integration test!
    noise[i] *= data->threshold;
  }
}

//TODO: merge back into src/common/dwt.c where this was copied from
static inline int rowid_to_row(const int rowid, const int height, const int scale)
{
  // to make this algorithm as cache-friendly as possible, we want to interleave the actual processing of rows
  // such that the next iteration processes the row 'scale' pixels below the current one, which will already
  // be in L2 cache (if not L1) from having been accessed on this iteration so if vscale is 16, we want to
  // process rows 0, 16, 32, ..., then 1, 17, 33, ..., 2, 18, 34, ..., etc.
  if (height <= scale)
    return rowid;
  const int per_pass = ((height + scale - 1) / scale);
  const int long_passes = height % scale;
  // adjust for the fact that we have some passes with one fewer iteration when height is not a multiple of scale
  if (long_passes == 0 || rowid < long_passes * per_pass)
    return (rowid / per_pass) + scale * (rowid % per_pass);
  const int rowid2 = rowid - long_passes * per_pass;
  return long_passes + (rowid2 / (per_pass-1)) + scale * (rowid2 % (per_pass-1));
}

//TODO: merge back into src/common/dwt.c
// first, "vertical" pass of wavelet decomposition
static void dwt_denoise_vert_1ch(float *const restrict out, const float *const restrict in,
                                 const int height, const int width, const int lev)
{
  const int vscale = MIN(1 << lev, height);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, width, vscale) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(int rowid = 0; rowid < height ; rowid++)
  {
    const int row = rowid_to_row(rowid,height,vscale);
    // perform a weighted sum of the current pixel row with the rows 'scale' pixels above and below
    // if either of those is beyond the edge of the image, we use reflection to get a value for averaging,
    // i.e. we move as many rows in from the edge as we would have been beyond the edge
    // for the top edge, this means we can simply use the absolute value of row-vscale; for the bottom edge,
    //   we need to reflect around height
    const size_t rowstart = row * width;
    const int below_row = (row + vscale < height) ? (row + vscale) : 2*(height-1) - (row + vscale);
    const float *const restrict center = in + rowstart;
    const float *const restrict above =  in + abs(row - vscale) * width;
    const float *const restrict below = in + below_row * width;
    float* const restrict outrow = out + rowstart;
#ifdef _OPENMP
#pragma omp simd
#endif
    for (int col= 0; col < width; col++)
    {
      outrow[col] = 2.f * center[col] + above[col] + below[col];
    }
  }
}

// second, horizontal pass of wavelet decomposition; generates 'coarse' into the output buffer and overwrites
//   the input buffer with 'details'
static void dwt_denoise_horiz_1ch(float *const restrict out, float *const restrict in,
                                  float *const restrict accum, const int height, const int width,
                                  const int lev, const float thold, const int last)
{
  const int hscale = MIN(1 << lev, width);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, width, hscale, thold, last) \
  dt_omp_sharedconst(in, out, accum) \
  schedule(static)
#endif
  for(int row = 0; row < height ; row++)
  {
    // perform a weighted sum of the current pixel with the ones 'scale' pixels to the left and right, using
    // reflection to get a value if either of those positions is out of bounds, i.e. we move as many columns
    // in from the edge as we would have been beyond the edge to avoid an additional pass, we also rescale the
    // final sum and split the original input into 'coarse' and 'details' by subtracting the scaled sum from
    // the original input.
    const int rowindex = (row * width);
    float *const restrict details = in + rowindex;
    float *const restrict coarse = out + rowindex;
    float *const restrict accum_row = accum + rowindex;
    // handle reflection at left edge
#ifdef _OPENMP
#pragma omp simd
#endif
    for (int col = 0; col < hscale; col++)
    {
      // add up left/center/right, and renormalize by dividing by the total weight of all numbers added together
      const float hat = (2.f * coarse[col] + coarse[hscale-col] + coarse[col+hscale]) / 16.f;
      // the normalized value is our 'coarse' result; 'diff' is the difference between original input and 'coarse'
      // (which would ordinarily be stored as the details scale, but we don't need it any further)
      const float diff = details[col] - hat;
      details[col] = hat;		// done with original input, so we can overwrite it with 'coarse'
      // GCC8 won't vectorize if we use the following line, but it turns out that just adding the two conditional
      // alternatives produces exactly the same result, and *that* does get vectorized
      //const float excess = diff < 0.0 ? MIN(diff + thold, 0.0f) : MAX(diff - thold, 0.0f);
      accum_row[col] += MAX(diff - thold,0.0f) + MIN(diff + thold, 0.0f);
    }
#ifdef _OPENMP
#pragma omp simd
#endif
    for (int col = hscale; col < width - hscale; col++)
    {
      // add up left/center/right, and renormalize by dividing by the total weight of all numbers added together
      const float hat = (2.f * coarse[col] + coarse[col-hscale] + coarse[col+hscale]) / 16.f;
      // the normalized value is our 'coarse' result; 'diff' is the difference between original input and 'coarse'
      // (which would ordinarily be stored as the details scale, but we don't need it any further)
      const float diff = details[col] - hat;
      details[col] = hat;		// done with original input, so we can overwrite it with 'coarse'
      // GCC8 won't vectorize if we use the following line, but it turns out that just adding the two conditional
      // alternatives produces exactly the same result, and *that* does get vectorized
      //const float excess = diff < 0.0 ? MIN(diff + thold, 0.0f) : MAX(diff - thold, 0.0f);
      accum_row[col] += MAX(diff - thold,0.0f) + MIN(diff + thold, 0.0f);
    }
    // handle reflection at right edge
#ifdef _OPENMP
#pragma omp simd
#endif
    for (int col = width - hscale; col < width; col++)
    {
      const float right = coarse[2*width - 2 - (col+hscale)];
      // add up left/center/right, and renormalize by dividing by the total weight of all numbers added together
      const float hat = (2.f * coarse[col] + coarse[col-hscale] + right) / 16.f;
      // the normalized value is our 'coarse' result; 'diff' is the difference between original input and 'coarse'
      // (which would ordinarily be stored as the details scale, but we don't need it any further)
      const float diff = details[col] - hat;
      details[col] = hat;		// done with original input, so we can overwrite it with 'coarse'
//      accum_row[col] += copysignf(fmaxf(fabsf(diff) - thold, 0.0f), diff);
      accum_row[col] += MAX(diff - thold,0.0f) + MIN(diff + thold, 0.0f);
    }
    if (last)
    {
      // add the details to the residue to create the final denoised result
      for (int col = 0; col < width; col++)
      {
        details[col] += accum_row[col];
      }
    }
  }
}

//TODO: merge into src/common/dwt.c, where it can make use of existing code without duplicating it into this file
static void dwt_denoise(float *const img, const int width, const int height,
                        const int bands, const float *const noise)
{
  float *const details = dt_alloc_sse_ps(2 * width * height);
  float *const interm = details + width * height;	// temporary storage for use during each pass

  // zero the accumulator
  memset(details, 0, width * height * sizeof(float));

  for(int lev = 0; lev < bands; lev++)
  {
    const int last = (lev+1) == bands;

    // "vertical" pass, averages pixels with those 'scale' rows above and below and puts result in 'interm'
    dwt_denoise_vert_1ch(interm, img, height, width, lev);
    // horizontal filtering pass, averages pixels in 'interm' with those 'scale' rows to the left and right
    // accumulates the portion of the detail scale that is above the noise threshold into 'details'; this
    // will be added to the residue left in 'img' on the last iteration
    dwt_denoise_horiz_1ch(interm, img, details, height, width, lev, noise[lev], last);
  }
  dt_free_align(details);
}

/*static*/ void wavelet_denoise(const float *const restrict in, float *const restrict out, const dt_iop_roi_t *const roi,
                            const dt_iop_rawdenoise_data_t * const data, const uint32_t filters)
{
  const size_t size = (size_t)(roi->width / 2 + 1) * (roi->height / 2 + 1);
  float *const restrict fimg = dt_alloc_sse_ps(size);

  const int nc = 4;
  for(int c = 0; c < nc; c++) /* denoise R,G1,B,G3 individually */
  {
    const int color = FC(c % 2, c / 2, filters);
    float noise[DT_IOP_RAWDENOISE_BANDS];
    compute_channel_noise(noise,color,data);

    // adjust for odd width and height
    const int halfwidth = roi->width / 2 + (roi->width & (~(c >> 1)) & 1);
    const int halfheight = roi->height / 2 + (roi->height & (~c) & 1);

    // collect one of the R/G1/G2/B channels into a monochrome image, applying sqrt() to the values as a
    // variance-stabilizing transform
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, fimg, roi, halfwidth) \
    shared(c) \
    schedule(static)
#endif
    for(int row = c & 1; row < roi->height; row += 2)
    {
      float *const restrict fimgp = fimg + (size_t)row / 2 * halfwidth;
      const int offset = (c & 2) >> 1;
      const float *const restrict inp = in + (size_t)row * roi->width + offset;
      const int senselwidth = (roi->width-offset+1)/2;
      for(int col = 0; col < senselwidth; col++)
        fimgp[col] = sqrtf(MAX(0.0f, inp[2*col]));
    }

    // perform the wavelet decomposition and denoising
    dwt_denoise(fimg,halfwidth,halfheight,DT_IOP_RAWDENOISE_BANDS,noise);

    // distribute the denoised data back out to the original R/G1/G2/B channel, squaring the resulting values to
    // undo the original transform
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(fimg, halfwidth, out, roi, size) \
    shared(c) \
    schedule(static)
#endif
    for(int row = c & 1; row < roi->height; row += 2)
    {
      const float *restrict fimgp = fimg + (size_t)row / 2 * halfwidth;
      const int offset = (c & 2) >> 1;
      float *const restrict outp = out + (size_t)row * roi->width + offset;
      const int senselwidth = (roi->width-offset+1)/2;
      for(int col = 0; col < senselwidth; col++)
      {
        float d = fimgp[col];
        outp[2*col] = d * d;
      }
    }
  }
#if 0
  /* FIXME: Haven't ported this part yet */
  float maximum = 1.0;		/* FIXME */
  float black = 0.0;		/* FIXME */
  maximum *= BIT16;
  black *= BIT16;
  for (c=0; c<4; c++)
    cblack[c] *= BIT16;
  if (filters && colors == 3)	/* pull G1 and G3 closer together */
  {
    float *window[4];
    int wlast, blk[2];
    float mul[2];
    float thold = threshold/512;
    for (row=0; row < 2; row++)
    {
      mul[row] = 0.125 * pre_mul[FC(row+1,0) | 1] / pre_mul[FC(row,0) | 1];
      blk[row] = cblack[FC(row,0) | 1];
    }
    for (i=0; i < 4; i++)
      window[i] = fimg + width*i;
    for (wlast=-1, row=1; row < height-1; row++)
    {
      while (wlast < row+1)
      {
        for (wlast++, i=0; i < 4; i++)
          window[(i+3) & 3] = window[i];
        for (col = FC(wlast,1) & 1; col < width; col+=2)
          window[2][col] = BAYER(wlast,col);
      }
      for (col = (FC(row,0) & 1)+1; col < width-1; col+=2)
      {
        float avg = ( window[0][col-1] + window[0][col+1] +
                      window[2][col-1] + window[2][col+1] - blk[~row & 1]*4 )
                    * mul[row & 1] + (window[1][col] + blk[row & 1]) * 0.5;
        avg = avg < 0 ? 0 : sqrt(avg);
        float diff = sqrt(BAYER(row,col)) - avg;
        if      (diff < -thold) diff += thold;
        else if (diff >  thold) diff -= thold;
        else diff = 0;
        BAYER(row,col) = SQR(avg+diff);
      }
    }
  }
#endif
  dt_free_align(fimg);
}

static void wavelet_denoise_xtrans(const float *const in, float *out, const dt_iop_roi_t *const roi,
                                   dt_iop_rawdenoise_data_t *data, const uint8_t (*const xtrans)[6])
{
  float threshold = data->threshold;
  // note that these constants are the same for X-Trans and Bayer, as
  // they are proportional to image detail on each channel, not the
  // sensor pattern
  float noise_all[] = { 0.8002, 0.2735, 0.1202, 0.0585, 0.0291, 0.0152, 0.0080, 0.0044 };
  for(int i = 0; i < DT_IOP_RAWDENOISE_BANDS; i++)
  {
    // scale the value from [0,1] to [0,16],
    // and makes the "0.5" neutral value become 1
    float threshold_exp_4 = data->force[DT_RAWDENOISE_ALL][DT_IOP_RAWDENOISE_BANDS - i - 1];
    threshold_exp_4 *= threshold_exp_4;
    threshold_exp_4 *= threshold_exp_4;
    noise_all[i] = noise_all[i] * threshold_exp_4 * 16.0;
  }

  const int width = roi->width;
  const int height = roi->height;
  const size_t size = (size_t)width * height;
  float *const fimg = malloc((size_t)size * 4 * sizeof(float));

  for(int c = 0; c < 3; c++)
  {
    float noise[DT_IOP_RAWDENOISE_BANDS];
    for(int i = 0; i < DT_IOP_RAWDENOISE_BANDS; i++)
    {
      float threshold_exp_4;
      switch(c)
      {
        case 0:
          threshold_exp_4 = data->force[DT_RAWDENOISE_R][DT_IOP_RAWDENOISE_BANDS - i - 1];
          break;
        case 2:
          threshold_exp_4 = data->force[DT_RAWDENOISE_B][DT_IOP_RAWDENOISE_BANDS - i - 1];
          break;
        default:
          threshold_exp_4 = data->force[DT_RAWDENOISE_G][DT_IOP_RAWDENOISE_BANDS - i - 1];
          break;
      }
      threshold_exp_4 *= threshold_exp_4;
      threshold_exp_4 *= threshold_exp_4;
      noise[i] = noise_all[i] * threshold_exp_4 * 16.0;
    }
    memset(fimg, 0, size * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(fimg, height, in, roi, size, width, xtrans) \
    shared(c) \
    schedule(static)
#endif
    for(int row = (c != 1); row < height - 1; row++)
    {
      int col = (c != 1);
      const float *inp = in + (size_t)row * width + col;
      float *fimgp = fimg + size + (size_t)row * width + col;
      for(; col < width - 1; col++, inp++, fimgp++)
        if(FCxtrans(row, col, roi, xtrans) == c)
        {
          float d = sqrt(MAX(0, *inp));
          *fimgp = d;
          // cheap nearest-neighbor interpolate
          if(c == 1)
            fimgp[1] = fimgp[width] = d;
          else
          {
            fimgp[-width - 1] = fimgp[-width] = fimgp[-width + 1] = fimgp[-1] = fimgp[1] = fimgp[width - 1]
                = fimgp[width] = fimgp[width + 1] = d;
          }
        }
    }

    int lastpass;

    for(int lev = 0; lev < 5; lev++)
    {
      const size_t pass1 = size * ((lev & 1) * 2 + 1);
      const size_t pass2 = 2 * size;
      const size_t pass3 = 4 * size - pass1;

// filter horizontally and transpose
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(fimg, height, pass1, pass2, width) \
      shared(lev) \
      schedule(static)
#endif
      for(int col = 0; col < width; col++)
        hat_transform(fimg + pass2 + (size_t)col * height, fimg + pass1 + col, width, height, 1 << lev);
// filter vertically and transpose back
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(fimg, height, pass2, pass3, width) \
      shared(lev) \
      schedule(static)
#endif
      for(int row = 0; row < height; row++)
        hat_transform(fimg + pass3 + (size_t)row * width, fimg + pass2 + row, height, width, 1 << lev);

      const float thold = threshold * noise[lev];
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(fimg, pass1, pass3, size, thold) \
      shared(lev) \
      schedule(static)
#endif
      for(size_t i = 0; i < size; i++)
      {
        float *fimgp = fimg + i;
        const float diff = fimgp[pass1] - fimgp[pass3];
        fimgp[0] += copysignf(fmaxf(fabsf(diff) - thold, 0.0f), diff);
      }

      lastpass = pass3;
    }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(height, fimg, roi, width, xtrans) \
    shared(c, lastpass, out) \
    schedule(static)
#endif
    for(int row = 0; row < height; row++)
    {
      const float *fimgp = fimg + (size_t)row * width;
      float *outp = out + (size_t)row * width;
      for(int col = 0; col < width; col++, outp++, fimgp++)
        if(FCxtrans(row, col, roi, xtrans) == c)
        {
          float d = fimgp[0] + fimgp[lastpass];
          *outp = d * d;
        }
    }
  }

  free(fimg);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_rawdenoise_data_t *d = (dt_iop_rawdenoise_data_t *)piece->data;

  const int width = roi_in->width;
  const int height = roi_in->height;

  if(!(d->threshold > 0.0f))
  {
    memcpy(ovoid, ivoid, (size_t)sizeof(float)*width*height);
  }
  else
  {
    const uint32_t filters = piece->pipe->dsc.filters;
    const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
    if (filters != 9u)
      wavelet_denoise(ivoid, ovoid, roi_in, d, filters);
    else
      wavelet_denoise_xtrans(ivoid, ovoid, roi_in, d, xtrans);
  }
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  dt_iop_rawdenoise_params_t *d = module->default_params;

  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
  {
    for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++)
    {
      d->x[ch][k] = k / (DT_IOP_RAWDENOISE_BANDS - 1.f);
    }
  }
}

void reload_defaults(dt_iop_module_t *module)
{
  // can't be switched on for non-raw images:
  module->hide_enable_button = !dt_image_is_raw(&module->dev->image_storage);

  if(module->widget)
  {
    gtk_stack_set_visible_child_name(GTK_STACK(module->widget), module->hide_enable_button ? "non_raw" : "raw");
  }

  module->default_enabled = 0;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)params;
  dt_iop_rawdenoise_data_t *d = (dt_iop_rawdenoise_data_t *)piece->data;

  d->threshold = p->threshold;

  for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++)
  {
    dt_draw_curve_set_point(d->curve[ch], 0, p->x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0, p->y[ch][0]);
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
      dt_draw_curve_set_point(d->curve[ch], k, p->x[ch][k], p->y[ch][k]);
    dt_draw_curve_set_point(d->curve[ch], DT_IOP_RAWDENOISE_BANDS + 1, p->x[ch][1] + 1.0,
                            p->y[ch][DT_IOP_RAWDENOISE_BANDS - 1]);
    dt_draw_curve_calc_values(d->curve[ch], 0.0, 1.0, DT_IOP_RAWDENOISE_BANDS, NULL, d->force[ch]);
  }

  if (!(dt_image_is_raw(&pipe->image)))
    piece->enabled = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rawdenoise_data_t *d = (dt_iop_rawdenoise_data_t *)malloc(sizeof(dt_iop_rawdenoise_data_t));
  dt_iop_rawdenoise_params_t *default_params = (dt_iop_rawdenoise_params_t *)self->default_params;

  piece->data = (void *)d;
  for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->x[ch][k], default_params->y[ch][k]);
  }
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rawdenoise_data_t *d = (dt_iop_rawdenoise_data_t *)(piece->data);
  for(int ch = 0; ch < DT_RAWDENOISE_NONE; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_rawdenoise_gui_data_t *g = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)self->params;
  dt_iop_cancel_history_update(self);
  dt_bauhaus_slider_set_soft(g->threshold, p->threshold);
  gtk_widget_queue_draw(self->widget);
}

static void dt_iop_rawdenoise_get_params(dt_iop_rawdenoise_params_t *p, const int ch, const double mouse_x,
                                         const double mouse_y, const float rad)
{
  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
  {
    const float f = expf(-(mouse_x - p->x[ch][k]) * (mouse_x - p->x[ch][k]) / (rad * rad));
    p->y[ch][k] = (1 - f) * p->y[ch][k] + f * mouse_y;
  }
}

static gboolean rawdenoise_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  dt_iop_rawdenoise_params_t p = *(dt_iop_rawdenoise_params_t *)self->params;

  int ch = (int)c->channel;
  dt_draw_curve_set_point(c->transition_curve, 0, p.x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0, p.y[ch][0]);
  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
    dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);
  dt_draw_curve_set_point(c->transition_curve, DT_IOP_RAWDENOISE_BANDS + 1, p.x[ch][1] + 1.0,
                          p.y[ch][DT_IOP_RAWDENOISE_BANDS - 1]);

  const int inset = DT_IOP_RAWDENOISE_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  cairo_set_source_rgb(cr, .2, .2, .2);

  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  dt_draw_grid(cr, 8, 0, 0, width, height);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max curves:
    dt_iop_rawdenoise_get_params(&p, c->channel, c->mouse_x, 1., c->mouse_radius);
    dt_draw_curve_set_point(c->transition_curve, 0, p.x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0, p.y[ch][0]);
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_set_point(c->transition_curve, DT_IOP_RAWDENOISE_BANDS + 1, p.x[ch][1] + 1.0,
                            p.y[ch][DT_IOP_RAWDENOISE_BANDS - 1]);
    dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_RAWDENOISE_RES, c->draw_min_xs, c->draw_min_ys);

    p = *(dt_iop_rawdenoise_params_t *)self->params;
    dt_iop_rawdenoise_get_params(&p, c->channel, c->mouse_x, .0, c->mouse_radius);
    dt_draw_curve_set_point(c->transition_curve, 0, p.x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0, p.y[ch][0]);
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_set_point(c->transition_curve, DT_IOP_RAWDENOISE_BANDS + 1, p.x[ch][1] + 1.0,
                            p.y[ch][DT_IOP_RAWDENOISE_BANDS - 1]);
    dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_RAWDENOISE_RES, c->draw_max_xs, c->draw_max_ys);
  }

  cairo_save(cr);

  // draw selected cursor
  cairo_translate(cr, 0, height);

  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));

  for(int i = 0; i < DT_RAWDENOISE_NONE; i++)
  {
    // draw curves, selected last
    ch = ((int)c->channel + i + 1) % DT_RAWDENOISE_NONE;
    float alpha = 0.3;
    if(i == DT_RAWDENOISE_NONE - 1) alpha = 1.0;
    switch(ch)
    {
      case 0:
        cairo_set_source_rgba(cr, .7, .7, .7, alpha);
        break;
      case 1:
        cairo_set_source_rgba(cr, .7, .1, .1, alpha);
        break;
      case 2:
        cairo_set_source_rgba(cr, .1, .7, .1, alpha);
        break;
      case 3:
        cairo_set_source_rgba(cr, .1, .1, .7, alpha);
        break;
    }

    p = *(dt_iop_rawdenoise_params_t *)self->params;
    dt_draw_curve_set_point(c->transition_curve, 0, p.x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0, p.y[ch][0]);
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_set_point(c->transition_curve, DT_IOP_RAWDENOISE_BANDS + 1, p.x[ch][1] + 1.0,
                            p.y[ch][DT_IOP_RAWDENOISE_BANDS - 1]);
    dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_RAWDENOISE_RES, c->draw_xs, c->draw_ys);
    cairo_move_to(cr, 0 * width / (float)(DT_IOP_RAWDENOISE_RES - 1), -height * c->draw_ys[0]);
    for(int k = 1; k < DT_IOP_RAWDENOISE_RES; k++)
      cairo_line_to(cr, k * width / (float)(DT_IOP_RAWDENOISE_RES - 1), -height * c->draw_ys[k]);
    cairo_stroke(cr);
  }

  ch = c->channel;
  // draw dots on knots
  cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
  {
    cairo_arc(cr, width * p.x[ch][k], -height * p.y[ch][k], DT_PIXEL_APPLY_DPI(3.0), 0.0, 2.0 * M_PI);
    if(c->x_move == k)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    cairo_set_source_rgba(cr, .7, .7, .7, .6);
    cairo_move_to(cr, 0, -height * c->draw_min_ys[0]);
    for(int k = 1; k < DT_IOP_RAWDENOISE_RES; k++)
      cairo_line_to(cr, k * width / (float)(DT_IOP_RAWDENOISE_RES - 1), -height * c->draw_min_ys[k]);
    for(int k = DT_IOP_RAWDENOISE_RES - 1; k >= 0; k--)
      cairo_line_to(cr, k * width / (float)(DT_IOP_RAWDENOISE_RES - 1), -height * c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = DT_IOP_RAWDENOISE_RES * c->mouse_x;
    int k = (int)pos;
    const float f = k - pos;
    if(k >= DT_IOP_RAWDENOISE_RES - 1) k = DT_IOP_RAWDENOISE_RES - 2;
    float ht = -height * (f * c->draw_ys[k] + (1 - f) * c->draw_ys[k + 1]);
    cairo_arc(cr, c->mouse_x * width, ht, c->mouse_radius * width, 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  cairo_restore(cr);

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

  // draw labels:
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc, (.08 * height) * PANGO_SCALE);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  cairo_set_source_rgb(cr, .1, .1, .1);

  pango_layout_set_text(layout, _("coarse"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .02 * width - ink.y, .5 * (height + ink.width));
  cairo_save(cr);
  cairo_rotate(cr, -M_PI * .5f);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);

  pango_layout_set_text(layout, _("fine"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .98 * width - ink.height, .5 * (height + ink.width));
  cairo_save(cr);
  cairo_rotate(cr, -M_PI * .5f);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);


  pango_layout_set_text(layout, _("smooth"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .5 * (width - ink.width), .08 * height - ink.height);
  pango_cairo_show_layout(cr, layout);

  pango_layout_set_text(layout, _("noisy"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .5 * (width - ink.width), .97 * height - ink.height);
  pango_cairo_show_layout(cr, layout);

  pango_font_description_free(desc);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean rawdenoise_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)self->params;
  const int inset = DT_IOP_RAWDENOISE_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
  c->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
  if(c->dragging)
  {
    *p = c->drag_params;
    if(c->x_move < 0)
    {
      dt_iop_rawdenoise_get_params(p, c->channel, c->mouse_x, c->mouse_y + c->mouse_pick, c->mouse_radius);
    }
    gtk_widget_queue_draw(widget);
    dt_iop_queue_history_update(self, FALSE);
  }
  else
  {
    c->x_move = -1;
    gtk_widget_queue_draw(widget);
  }
  gint x, y;
#if GTK_CHECK_VERSION(3, 20, 0)
  gdk_window_get_device_position(
      event->window, gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(event->window))), &x,
      &y, 0);
#else
  gdk_window_get_device_position(
      event->window,
      gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gdk_window_get_display(event->window))),
      &x, &y, NULL);
#endif
  return TRUE;
}

static gboolean rawdenoise_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  const int ch = c->channel;
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset current curve
    dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)self->params;
    dt_iop_rawdenoise_params_t *d = (dt_iop_rawdenoise_params_t *)self->default_params;
    for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
    {
      p->x[ch][k] = d->x[ch][k];
      p->y[ch][k] = d->y[ch][k];
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(self->widget);
  }
  else if(event->button == 1)
  {
    c->drag_params = *(dt_iop_rawdenoise_params_t *)self->params;
    const int inset = DT_IOP_RAWDENOISE_INSET;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
    c->mouse_pick
        = dt_draw_curve_calc_value(c->transition_curve, CLAMP(event->x - inset, 0, width) / (float)width);
    c->mouse_pick -= 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean rawdenoise_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
    c->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean rawdenoise_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  if(!c->dragging) c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean rawdenoise_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  gdouble delta_y;
  if(dt_gui_get_scroll_deltas(event, NULL, &delta_y))
  {
    c->mouse_radius = CLAMP(c->mouse_radius * (1.0 + 0.1 * delta_y), 0.2 / DT_IOP_RAWDENOISE_BANDS, 1.0);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

static void rawdenoise_tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  c->channel = (dt_iop_rawdenoise_channel_t)page_num;
  gtk_widget_queue_draw(self->widget);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_rawdenoise_gui_data_t *c = IOP_GUI_ALLOC(rawdenoise);
  dt_iop_rawdenoise_params_t *p = (dt_iop_rawdenoise_params_t *)self->default_params;

  c->channel = dt_conf_get_int("plugins/darkroom/rawdenoise/gui_channel");
  c->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

  dt_ui_notebook_page(c->channel_tabs, _("all"), NULL);
  dt_ui_notebook_page(c->channel_tabs, _("R"), NULL);
  dt_ui_notebook_page(c->channel_tabs, _("G"), NULL);
  dt_ui_notebook_page(c->channel_tabs, _("B"), NULL);

  gtk_widget_show(gtk_notebook_get_nth_page(c->channel_tabs, c->channel));
  gtk_notebook_set_current_page(c->channel_tabs, c->channel);
  g_signal_connect(G_OBJECT(c->channel_tabs), "switch_page", G_CALLBACK(rawdenoise_tab_switch), self);

  const int ch = (int)c->channel;
  c->transition_curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  (void)dt_draw_curve_add_point(c->transition_curve, p->x[ch][DT_IOP_RAWDENOISE_BANDS - 2] - 1.0,
                                p->y[ch][DT_IOP_RAWDENOISE_BANDS - 2]);
  for(int k = 0; k < DT_IOP_RAWDENOISE_BANDS; k++)
    (void)dt_draw_curve_add_point(c->transition_curve, p->x[ch][k], p->y[ch][k]);
  (void)dt_draw_curve_add_point(c->transition_curve, p->x[ch][1] + 1.0, p->y[ch][1]);

  c->mouse_x = c->mouse_y = c->mouse_pick = -1.0;
  c->dragging = 0;
  c->x_move = -1;
  self->timeout_handle = 0;
  c->mouse_radius = 1.0 / (DT_IOP_RAWDENOISE_BANDS * 2);

  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(9.0 / 16.0));

  gtk_box_pack_start(GTK_BOX(box_raw), GTK_WIDGET(c->channel_tabs), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box_raw), GTK_WIDGET(c->area), FALSE, FALSE, 0);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                 | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                 | GDK_LEAVE_NOTIFY_MASK | darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(rawdenoise_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(rawdenoise_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "button-release-event", G_CALLBACK(rawdenoise_button_release), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(rawdenoise_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(rawdenoise_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(rawdenoise_scrolled), self);

  c->threshold = dt_bauhaus_slider_from_params(self, "threshold");
  dt_bauhaus_slider_set_soft_max(c->threshold, 0.1);
  dt_bauhaus_slider_set_digits(c->threshold, 3);

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);

  GtkWidget *label_non_raw = dt_ui_label_new(_("raw denoising\nonly works for raw images."));

  gtk_stack_add_named(GTK_STACK(self->widget), label_non_raw, "non_raw");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "raw");
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_rawdenoise_gui_data_t *c = (dt_iop_rawdenoise_gui_data_t *)self->gui_data;
  dt_conf_set_int("plugins/darkroom/rawdenoise/gui_channel", c->channel);
  dt_draw_curve_destroy(c->transition_curve);
  dt_iop_cancel_history_update(self);

  IOP_GUI_FREE;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
