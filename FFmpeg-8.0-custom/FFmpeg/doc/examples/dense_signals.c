/*
 * dense_signals.c — enable dense_mb_signals on the H.264 decoder and
 * read the four per-frame side-data channels the custom fork emits:
 *   AV_FRAME_DATA_MOTION_VECTORS  (full AVMotionVector[])
 *   AV_FRAME_DATA_H264_NZC_SUMS   (uint16_t[mb_w * mb_h])
 *   AV_FRAME_DATA_H264_MB_TYPE    (uint32_t[mb_w * mb_h])
 *   AV_FRAME_DATA_H264_NZC_ALL    (uint16_t[mb_w * mb_h])
 *
 * Build (from the FFmpeg source tree):
 *   ./configure --prefix=/tmp/mstone_dense ...   # already done for the fork
 *   make examples
 *
 * Run:
 *   LD_LIBRARY_PATH=/tmp/mstone_dense/lib \
 *   doc/examples/dense_signals input.mp4 [max_frames]
 *
 * Standalone build (outside the tree, against an installed fork):
 *   PKG_CONFIG_PATH=/tmp/mstone_dense/lib/pkgconfig \
 *   gcc -O2 -Wall dense_signals.c \
 *       $(pkg-config --cflags --libs libavformat libavcodec libavutil) \
 *       -o dense_signals
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
#include <libavutil/opt.h>

/* From libavcodec/mpegutils.h — not a public header, so mirrored here. */
#define MB_TYPE_INTRA4x4   (1u << 0)
#define MB_TYPE_INTRA16x16 (1u << 1)
#define MB_TYPE_INTRA_PCM  (1u << 2)
#define MB_TYPE_SKIP       (1u << 17)
#define MB_TYPE_INTRA_ANY  (MB_TYPE_INTRA4x4 | MB_TYPE_INTRA16x16 | MB_TYPE_INTRA_PCM)

static void print_frame(const AVFrame *fr, const AVCodecContext *ctx, int idx)
{
    const int mb_w = (ctx->width  + 15) >> 4;
    const int mb_h = (ctx->height + 15) >> 4;
    const int mb_n = mb_w * mb_h;

    printf("frame %d  type=%c  grid=%dx%d (%d MBs)\n",
           idx, av_get_picture_type_char(fr->pict_type), mb_w, mb_h, mb_n);

    AVFrameSideData *sd;

    sd = av_frame_get_side_data(fr, AV_FRAME_DATA_MOTION_VECTORS);
    if (sd) {
        size_t n = sd->size / sizeof(AVMotionVector);
        const AVMotionVector *mv = (const AVMotionVector *)sd->data;
        long long sx = 0, sy = 0;
        for (size_t i = 0; i < n; i++) { sx += mv[i].motion_x; sy += mv[i].motion_y; }
        printf("  MOTION_VECTORS : %zu records  avg=(%.2f, %.2f)\n",
               n, n ? (double)sx / n : 0.0, n ? (double)sy / n : 0.0);
    }

    sd = av_frame_get_side_data(fr, AV_FRAME_DATA_H264_NZC_SUMS);
    if (sd && sd->size == (size_t)mb_n * sizeof(uint16_t)) {
        const uint16_t *v = (const uint16_t *)sd->data;
        long long s = 0;
        for (int i = 0; i < mb_n; i++) s += v[i];
        printf("  NZC_SUMS       : sum=%lld  (AC only, 48-slot layout: 8x8 area-weighted)\n", s);
    }

    sd = av_frame_get_side_data(fr, AV_FRAME_DATA_H264_MB_TYPE);
    if (sd && sd->size == (size_t)mb_n * sizeof(uint32_t)) {
        const uint32_t *t = (const uint32_t *)sd->data;
        int intra = 0, skip = 0, other = 0;
        for (int i = 0; i < mb_n; i++) {
            if      (t[i] & MB_TYPE_SKIP)      skip++;
            else if (t[i] & MB_TYPE_INTRA_ANY) intra++;
            else                               other++;
        }
        printf("  MB_TYPE        : intra=%d skip=%d other=%d\n", intra, skip, other);
    }

    sd = av_frame_get_side_data(fr, AV_FRAME_DATA_H264_NZC_ALL);
    if (sd && sd->size == (size_t)mb_n * sizeof(uint16_t)) {
        const uint16_t *v = (const uint16_t *)sd->data;
        long long s = 0;
        int coded_mbs = 0;
        for (int i = 0; i < mb_n; i++) { s += v[i]; coded_mbs += (v[i] != 0); }
        printf("  NZC_ALL        : sum=%lld  coded_mbs=%d/%d  (per-MB coded-coeff count, all blocks)\n",
               s, coded_mbs, mb_n);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s input.mp4 [max_frames]\n", argv[0]);
        return 1;
    }
    const char *url = argv[1];
    int max_frames = (argc >= 3) ? atoi(argv[2]) : 0;

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, url, NULL, NULL) < 0) {
        fprintf(stderr, "open failed: %s\n", url); return 2;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "find_stream_info failed\n"); return 2;
    }

    int vi = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vi = i; break; }
    }
    if (vi < 0) { fprintf(stderr, "no video stream\n"); return 2; }
    if (fmt->streams[vi]->codecpar->codec_id != AV_CODEC_ID_H264) {
        fprintf(stderr, "dense_mb_signals is H.264-only\n"); return 2;
    }

    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, fmt->streams[vi]->codecpar);

    /* README warning: per-MB channels require single-thread decode. */
    ctx->thread_type  = FF_THREAD_SLICE;
    ctx->thread_count = 1;

    /* Enable the fork's dense mode + ask FFmpeg to emit MV side data. */
    av_opt_set_int(ctx, "dense_mb_signals", 1, AV_OPT_SEARCH_CHILDREN);
    ctx->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;

    if (avcodec_open2(ctx, dec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2 failed\n"); return 2;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *fr  = av_frame_alloc();
    int frame_idx = 0;

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vi && avcodec_send_packet(ctx, pkt) == 0) {
            while (avcodec_receive_frame(ctx, fr) == 0) {
                print_frame(fr, ctx, frame_idx++);
                av_frame_unref(fr);
                if (max_frames && frame_idx >= max_frames) goto done;
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, fr) == 0) {
        print_frame(fr, ctx, frame_idx++);
        av_frame_unref(fr);
        if (max_frames && frame_idx >= max_frames) break;
    }

done:
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return 0;
}
