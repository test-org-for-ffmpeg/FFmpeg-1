/*
 * Mpeg video formats-related defines and utility functions
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_MPEGUTILS_H
#define AVCODEC_MPEGUTILS_H

#include <stdint.h>
#include <string.h>

#include "libavutil/attributes.h"
#include "libavutil/frame.h"

#include "avcodec.h"

/**
 * Custom (MV-only fork): shared post-filters applied to every exported motion
 * vector, so h264/mpeg4 (mpegutils.c) and hevc (hevc/hevcdec.c) decimate and
 * threshold identically. Driven by the `mv_min_size` / `mv_grid`
 * AVOptions; both default to 0, which keeps the pre-filter behaviour exactly.
 *
 * The size threshold runs first and the spatial grid second, so a cell is
 * claimed by a vector that actually passed the threshold - otherwise a
 * sub-threshold vector could occupy the cell and suppress the real one behind
 * it.
 */

/** Grid bitmap capacity: 8 KB covers 4K at mv_grid=8 or 1080p at mv_grid=4.
 *  Beyond that the grid simply stops thinning (it never drops a vector it
 *  cannot account for), which is the safe direction to fail in. */
#define FF_MV_GRID_MAX_CELLS 65536

typedef struct FFMvFilter {
    /** Drop vectors whose displacement is shorter than this, in whole pixels.
     *  0 disables the threshold. */
    int min_size;
    /** Cell size in luma pixels for spatial thinning; 0 disables it. */
    int grid;
    /** Cells per row, and total cells, derived from the picture size. */
    int grid_cols, grid_cells;
    /** One bit per cell: set once that cell has exported a vector. */
    uint8_t grid_seen[FF_MV_GRID_MAX_CELLS / 8];
} FFMvFilter;

/**
 * @param width,height picture size in luma samples, for the grid geometry.
 */
static av_always_inline void ff_mv_filter_init(FFMvFilter *f,
                                               const AVCodecContext *avctx,
                                               int width, int height)
{
    f->min_size  = avctx->mv_min_size;
    f->grid      = avctx->mv_grid;
    f->grid_cols = 0;
    f->grid_cells = 0;
    if (f->grid > 0 && width > 0 && height > 0) {
        int cols = (width  + f->grid - 1) / f->grid;
        int rows = (height + f->grid - 1) / f->grid;
        if (cols > 0 && rows > 0 && (int64_t)cols * rows <= FF_MV_GRID_MAX_CELLS) {
            f->grid_cols  = cols;
            f->grid_cells = cols * rows;
            /* Only the used prefix, not the whole 8 KB. */
            memset(f->grid_seen, 0, (size_t)(f->grid_cells + 7) / 8);
        } else {
            f->grid = 0; /* too fine for the bitmap - thin nothing */
        }
    }
}

/**
 * @return 1 to keep the vector, 0 to drop it.
 *
 * Compares squared lengths to avoid a sqrt per vector; `mv_min_size` is capped
 * at INT16_MAX by its AVOption so min_size*min_size cannot overflow int.
 */
/**
 * @param cur_x,cur_y the block's position in the CURRENT picture, which the
 *        spatial grid buckets on. Callers must pass the right endpoint:
 *        mpegutils puts the current-picture centre in dst, hevcdec in src.
 */
static av_always_inline int ff_mv_filter_keep(FFMvFilter *f,
                                              int src_x, int src_y,
                                              int dst_x, int dst_y,
                                              int cur_x, int cur_y)
{
    if (f->min_size > 0) {
        const int dx = dst_x - src_x;
        const int dy = dst_y - src_y;
        if (dx * dx + dy * dy < f->min_size * f->min_size)
            return 0;
    }
    /* One vector per cell: the first to reach an unclaimed cell wins, so the
     * kept field is spread over the picture instead of following the decoder's
     * macroblock walk. */
    if (f->grid > 0 && f->grid_cells > 0) {
        const int cx = cur_x / f->grid;
        const int cy = cur_y / f->grid;
        if (cx >= 0 && cy >= 0 && cx < f->grid_cols) {
            const int idx = cy * f->grid_cols + cx;
            if (idx >= 0 && idx < f->grid_cells) {
                const int byte = idx >> 3, bit = 1 << (idx & 7);
                if (f->grid_seen[byte] & bit)
                    return 0;
                f->grid_seen[byte] |= bit;
            }
        }
    }
    return 1;
}

/* picture type */
#define PICT_TOP_FIELD     1
#define PICT_BOTTOM_FIELD  2
#define PICT_FRAME         3

#define MAX_MB_BYTES    (30 * 16 * 16 * 3 / 8 + 120)

/* MB types */
#define MB_TYPE_INTRA4x4   (1 <<  0)
#define MB_TYPE_INTRA16x16 (1 <<  1) // FIXME H.264-specific
#define MB_TYPE_INTRA_PCM  (1 <<  2) // FIXME H.264-specific
#define MB_TYPE_16x16      (1 <<  3)
#define MB_TYPE_16x8       (1 <<  4)
#define MB_TYPE_8x16       (1 <<  5)
#define MB_TYPE_8x8        (1 <<  6)
#define MB_TYPE_INTERLACED (1 <<  7)
#define MB_TYPE_DIRECT2    (1 <<  8) // FIXME
#define MB_TYPE_CBP        (1 << 10)
#define MB_TYPE_QUANT      (1 << 11)
#define MB_TYPE_FORWARD_MV (1 << 12)
#define MB_TYPE_BACKWARD_MV (1 << 13)
#define MB_TYPE_BIDIR_MV   (MB_TYPE_FORWARD_MV | MB_TYPE_BACKWARD_MV)
// MB_TYPE_P[01]L[01], MB_TYPE_L[01] and MB_TYPE_L0L1 are H.264 only.
#define MB_TYPE_P0L0       (1 << 12)
#define MB_TYPE_P1L0       (1 << 13)
#define MB_TYPE_P0L1       (1 << 14)
#define MB_TYPE_P1L1       (1 << 15)
#define MB_TYPE_L0         (MB_TYPE_P0L0 | MB_TYPE_P1L0)
#define MB_TYPE_L1         (MB_TYPE_P0L1 | MB_TYPE_P1L1)
#define MB_TYPE_L0L1       (MB_TYPE_L0   | MB_TYPE_L1)
#define MB_TYPE_GMC        (1 << 16)
#define MB_TYPE_SKIP       (1 << 17)
#define MB_TYPE_ACPRED     (1 << 18)

#define MB_TYPE_INTRA    MB_TYPE_INTRA4x4 // default mb_type if there is just one type

// The following MB-type can be used by each codec as it sees fit.
#define MB_TYPE_CODEC_SPECIFIC  (1 << 9)

#define IS_INTRA4x4(a)   ((a) & MB_TYPE_INTRA4x4)
#define IS_INTRA16x16(a) ((a) & MB_TYPE_INTRA16x16)
#define IS_PCM(a)        ((a) & MB_TYPE_INTRA_PCM)
#define IS_INTRA(a)      ((a) & 7)
#define IS_INTER(a)      ((a) & (MB_TYPE_16x16 | MB_TYPE_16x8 | \
                                 MB_TYPE_8x16  | MB_TYPE_8x8))
#define IS_SKIP(a)       ((a) & MB_TYPE_SKIP)
#define IS_INTRA_PCM(a)  ((a) & MB_TYPE_INTRA_PCM)
#define IS_INTERLACED(a) ((a) & MB_TYPE_INTERLACED)
#define IS_DIRECT(a)     ((a) & MB_TYPE_DIRECT2)
#define IS_GMC(a)        ((a) & MB_TYPE_GMC)
#define IS_16X16(a)      ((a) & MB_TYPE_16x16)
#define IS_16X8(a)       ((a) & MB_TYPE_16x8)
#define IS_8X16(a)       ((a) & MB_TYPE_8x16)
#define IS_8X8(a)        ((a) & MB_TYPE_8x8)
#define IS_ACPRED(a)     ((a) & MB_TYPE_ACPRED)
#define IS_QUANT(a)      ((a) & MB_TYPE_QUANT)

#define HAS_CBP(a)       ((a) & MB_TYPE_CBP)
#define HAS_FORWARD_MV(a)  ((a) & MB_TYPE_FORWARD_MV)
#define HAS_BACKWARD_MV(a) ((a) & MB_TYPE_BACKWARD_MV)
// dir == 0 means forward, dir == 1 is backward
#define HAS_MV(a, dir)     ((a) & (MB_TYPE_FORWARD_MV << (dir)))

#define MB_TYPE_MV_2_MV_DIR(a) (((a) >> 12) & (MV_DIR_FORWARD | MV_DIR_BACKWARD))

/**
 * Draw a horizontal band if supported.
 *
 * @param h is the normal height, this will be reduced automatically if needed
 */
void ff_draw_horiz_band(AVCodecContext *avctx, const AVFrame *cur, const AVFrame *last,
                        int y, int h, int picture_structure, int first_field,
                        int low_delay);

/**
 * Print debugging info for the given picture.
 */
void ff_print_debug_info2(AVCodecContext *avctx, AVFrame *pict,
                          const uint32_t *mbtype_table,
                          const int8_t *qscale_table, int16_t (*const motion_val[2])[2],
                          int mb_width, int mb_height, int mb_stride, int quarter_sample);

/**
 * Optimized variant for motion_vectors_only mode — skips debug output
 * and emits compact motion vectors (src + dst only, no motion_x/y).
 */
void ff_print_debug_info2_optimized(AVCodecContext *avctx, AVFrame *pict,
                          const uint32_t *mbtype_table,
                          int16_t (*const motion_val[2])[2],
                          int mb_width, int mb_height, int mb_stride, int quarter_sample);

#endif /* AVCODEC_MPEGUTILS_H */
