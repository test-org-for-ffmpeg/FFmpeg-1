## Layout

- [FFmpeg-8.0/](FFmpeg-8.0/) — vanilla upstream `release/8.0` checkout. The baseline for all benchmarks.
- [FFmpeg-8.0-custom/](FFmpeg-8.0-custom/) — same checkout with the motion-vector patch from [../ffmpeg_installer/custom_ffmpeg.diff](../ffmpeg_installer/custom_ffmpeg.diff) applied.

Each tree is a self-contained install prefix: `bin/`, `include/`, `lib/`, `share/`, plus the source under `FFmpeg/`. 

## What the custom build changes

The patch targets one problem: extracting H.264/HEVC motion vectors is dominated by work the consumer doesn't care about (residual decoding, motion compensation, deblocking, film grain, SEI handling). Four additions:

1. **`motion_vectors_only` decode mode.** A new `int motion_vectors_only` field on `AVCodecContext`. When set, the H.264 and HEVC decoders skip pixel reconstruction and most post-processing paths — they parse just enough of each slice/MB/CU to recover MVs, then bail. Touches [libavcodec/avcodec.h](FFmpeg-8.0-custom/FFmpeg/libavcodec/avcodec.h), [decode.c](FFmpeg-8.0-custom/FFmpeg/libavcodec/decode.c), the H.264 stack (`h264_*.c`, `h264dsp.[ch]`), and the HEVC stack (`hevc/*.c`).
2. **Compact motion vector export.** A new side-data type `AV_FRAME_DATA_MOTION_VECTORS_COMPACT` carrying `AVMotionVectorCompact` records (8 bytes each, vs. ~40 for the stock `AVMotionVector`). Emitted from `mpegutils.c` and `hevc/hevcdec.c` when the caller opts in. Declared in [libavutil/motion_vector.h](FFmpeg-8.0-custom/FFmpeg/libavutil/motion_vector.h) and [libavutil/frame.h](FFmpeg-8.0-custom/FFmpeg/libavutil/frame.h).
3. **`dense_mb_signals` decode mode + per-macroblock side data.** A new `int dense_mb_signals` field on `AVCodecContext`. When set, the H.264 decoder runs the same fast path as `motion_vectors_only` (no IDCT / MC / deblock) but emits:
   - the **full** `AVMotionVector[]` on `AV_FRAME_DATA_MOTION_VECTORS` (not the compact form) — same distribution as stock FFmpeg, sub-pel MVs preserved,
   - `AV_FRAME_DATA_H264_NZC_SUMS` — `uint16_t[mb_w * mb_h]`, sum of the 48 4x4 non-zero-coefficient counts per MB,
   - `AV_FRAME_DATA_H264_MB_TYPE` — `uint32_t[mb_w * mb_h]`, raw `MB_TYPE_*` bitmask per MB (interpret with `IS_INTRA` / `IS_SKIP` / `IS_INTER` macros from `mpegutils.h`).

   Both new channels are emitted on P and B frames (empty on I-frames, mirroring the existing MV convention). Written for compressed-domain AI consumers that need the stock MV distribution plus residual and type context per MB. Touches [libavutil/frame.h](FFmpeg-8.0-custom/FFmpeg/libavutil/frame.h), [libavcodec/avcodec.h](FFmpeg-8.0-custom/FFmpeg/libavcodec/avcodec.h), [options_table.h](FFmpeg-8.0-custom/FFmpeg/libavcodec/options_table.h), [h264dec.c](FFmpeg-8.0-custom/FFmpeg/libavcodec/h264dec.c), [h264_picture.c](FFmpeg-8.0-custom/FFmpeg/libavcodec/h264_picture.c), and [mpegutils.c](FFmpeg-8.0-custom/FFmpeg/libavcodec/mpegutils.c).
4. **`AV_FRAME_DATA_H264_NZC_ALL` — per-macroblock non-zero coefficient count.** Also emitted under `dense_mb_signals`, on P and B frames, alongside the channels above (which are unchanged). `uint16_t[mb_w * mb_h]`, same row-major layout as `NZC_SUMS`. Each element is the number of non-zero transform coefficients decoded for its MB, counted while the coefficients are entropy decoded (CABAC and CAVLC, normal and fast path): every coded block counts once — 4x4, 8x8, luma DC and chroma DC; skipped MBs are 0. It differs from `NZC_SUMS`, which sums FFmpeg's 48-entry `non_zero_count` table: there an 8x8 transform block is stored in 4 entries (so counted 4 times) and DC blocks are not included. Touches [libavutil/frame.h](FFmpeg-8.0-custom/FFmpeg/libavutil/frame.h), [libavutil/side_data.c](FFmpeg-8.0-custom/FFmpeg/libavutil/side_data.c), [h264dec.h](FFmpeg-8.0-custom/FFmpeg/libavcodec/h264dec.h), [h264dec.c](FFmpeg-8.0-custom/FFmpeg/libavcodec/h264dec.c), [h264_cabac.c](FFmpeg-8.0-custom/FFmpeg/libavcodec/h264_cabac.c), [h264_cavlc.c](FFmpeg-8.0-custom/FFmpeg/libavcodec/h264_cavlc.c), [h264_slice.c](FFmpeg-8.0-custom/FFmpeg/libavcodec/h264_slice.c), and [h264_picture.c](FFmpeg-8.0-custom/FFmpeg/libavcodec/h264_picture.c).

## Usage

Enable the modes on any `AVCodecContext` before `avcodec_open2`. Three combinations, most compact to most detailed:

```c
/* (1) Compact MVs only — existing motion_vectors_only path (H.264 + HEVC) */
av_opt_set_int(ctx, "motion_vectors_only", 1, AV_OPT_SEARCH_CHILDREN);
ctx->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;

/* (2) Dense MVs + per-MB NZC (NZC_SUMS and NZC_ALL) + per-MB MB_TYPE (H.264 only) */
av_opt_set_int(ctx, "dense_mb_signals", 1, AV_OPT_SEARCH_CHILDREN);
ctx->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;

/* (3) Add frame-level QP from the bitstream */
ctx->export_side_data |= AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS;
```

Notes:

- `dense_mb_signals` is H.264-only. On other codecs it is a silent no-op — accepted at the AVOption layer, ignored by the decoder. Same shape as `motion_vectors_only` on non-H.264/HEVC codecs.
- Under either fast path, `fr->width / fr->height` are 16 (the decoder allocates a minimal 16x16 pixel buffer). Read `ctx->width / ctx->height` for the actual picture size.
- Frame-level metadata — packet size (`pkt->size`), PTS (`fr->pts`), picture type (`fr->pict_type`), frame QP (`AV_FRAME_DATA_VIDEO_ENC_PARAMS` → `par->qp`) — is available through standard FFmpeg APIs; no fork-specific reads needed.
- I / P / B frames are all supported. Side data is emitted on P and B frames (empty on I). `AVMotionVector` values under the fast path are bit-exact against full pixel decode; verified per-vector against the semantic fields on a mixed I/P/B x264 stream (`bframes=3, b-adapt=2`).
- Decode single-threaded (`thread_count = 1`) when reading the per-MB channels. With frame threading, `NZC_SUMS`, `NZC_ALL`, `MB_TYPE` (and some MVs) are not reproducible between runs.


