## Layout

- [FFmpeg-8.0/](FFmpeg-8.0/) — vanilla upstream `release/8.0` checkout. The baseline for all benchmarks.
- [FFmpeg-8.0-custom/](FFmpeg-8.0-custom/) — same checkout with the motion-vector patch from [../ffmpeg_installer/custom_ffmpeg.diff](../ffmpeg_installer/custom_ffmpeg.diff) applied.

Each tree is a self-contained install prefix: `bin/`, `include/`, `lib/`, `share/`, plus the source under `FFmpeg/`. 

## What the custom build changes

The patch targets one problem: extracting H.264/HEVC motion vectors is dominated by work the consumer doesn't care about (residual decoding, motion compensation, deblocking, film grain, SEI handling). Two additions:

1. **`motion_vectors_only` decode mode.** A new `int motion_vectors_only` field on `AVCodecContext`. When set, the H.264 and HEVC decoders skip pixel reconstruction and most post-processing paths — they parse just enough of each slice/MB/CU to recover MVs, then bail. Touches [libavcodec/avcodec.h](FFmpeg-8.0-custom/FFmpeg/libavcodec/avcodec.h), [decode.c](FFmpeg-8.0-custom/FFmpeg/libavcodec/decode.c), the H.264 stack (`h264_*.c`, `h264dsp.[ch]`), and the HEVC stack (`hevc/*.c`).
2. **Compact motion vector export.** A new side-data type `AV_FRAME_DATA_MOTION_VECTORS_COMPACT` carrying `AVMotionVectorCompact` records (8 bytes each, vs. ~40 for the stock `AVMotionVector`). Emitted from `mpegutils.c` and `hevc/hevcdec.c` when the caller opts in. Declared in [libavutil/motion_vector.h](FFmpeg-8.0-custom/FFmpeg/libavutil/motion_vector.h) and [libavutil/frame.h](FFmpeg-8.0-custom/FFmpeg/libavutil/frame.h).

