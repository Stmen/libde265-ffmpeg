/*
 * H.265 decoder
 *
 * Copyright (c) 2013, Dirk Farin <dirk.farin@gmail.com>
 * Copyright (c) 2013-2015, Joachim Bauch <bauch@struktur.de>
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

/**
 * @file
 * H.265 decoder based on libde265
 */

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>

#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/intreadwrite.h>
#ifdef __cplusplus
}
#endif

#include <libde265/de265.h>

#if !defined(LIBDE265_NUMERIC_VERSION) || LIBDE265_NUMERIC_VERSION < 0x02000000
#error "You need libde265 2.0 or newer to compile this plugin."
#endif

#include "libde265dec.h"

#define MAX_FRAME_QUEUE     16
#define MAX_SPEC_QUEUE      16

#define LIBDE265_FFMPEG_MAX(a, b)  ((a) > (b) ? (a) : (b))
#define LIBDE265_FFMPEG_MIN(a, b)  ((a) < (b) ? (a) : (b))

typedef struct DE265DecoderContext {
    de265_decoder_context* decoder;

    int check_extra;
    int packetized;
    int length_size;
    int deblocking;
    int decode_ratio;
    int frame_queue_len;
    AVFrame *frame_queue[MAX_FRAME_QUEUE];
    int spec_queue_len;
    struct de265_image_spec *spec_queue[MAX_SPEC_QUEUE];
} DE265Context;


static inline int align_value(int value, int alignment) {
    return ((value + (alignment - 1)) & ~(alignment - 1));
}


static inline enum AVPixelFormat get_pixel_format(AVCodecContext *avctx, enum de265_chroma chroma, int bits_per_pixel) {
    enum AVPixelFormat result = AV_PIX_FMT_NONE;
    switch (chroma) {
    case de265_chroma_mono:
        result = AV_PIX_FMT_GRAY8;
        break;
    case de265_chroma_420:
        switch (bits_per_pixel) {
        case 8:
            result = AV_PIX_FMT_YUV420P;
            break;
        case 9:
            result = AV_PIX_FMT_YUV420P9LE;
            break;
        case 10:
            result = AV_PIX_FMT_YUV420P10LE;
            break;
        case 12:
            result = AV_PIX_FMT_YUV420P12LE;
            break;
        case 14:
            result = AV_PIX_FMT_YUV420P14LE;
            break;
        case 16:
            result = AV_PIX_FMT_YUV420P16LE;
            break;
        default:
            if (bits_per_pixel < 8 || bits_per_pixel > 16) {
                av_log(avctx, AV_LOG_WARNING, "Unsupported chroma %d with %d bits per pixel", chroma, bits_per_pixel);
            } else {
                result = AV_PIX_FMT_YUV420P16LE;
            }
            break;
        }
        break;
    case de265_chroma_422:
        switch (bits_per_pixel) {
        case 8:
            result = AV_PIX_FMT_YUV422P;
            break;
        case 9:
            result = AV_PIX_FMT_YUV422P9LE;
            break;
        case 10:
            result = AV_PIX_FMT_YUV422P10LE;
            break;
        case 12:
            result = AV_PIX_FMT_YUV422P12LE;
            break;
        case 14:
            result = AV_PIX_FMT_YUV422P14LE;
            break;
        case 16:
            result = AV_PIX_FMT_YUV422P16LE;
            break;
        default:
            if (bits_per_pixel < 8 || bits_per_pixel > 16) {
                av_log(avctx, AV_LOG_WARNING, "Unsupported chroma %d with %d bits per pixel", chroma, bits_per_pixel);
            } else {
                result = AV_PIX_FMT_YUV422P16LE;
            }
            break;
        }
        break;
    case de265_chroma_444:
        switch (bits_per_pixel) {
        case 8:
            result = AV_PIX_FMT_YUV444P;
            break;
        case 9:
            result = AV_PIX_FMT_YUV444P9LE;
            break;
        case 10:
            result = AV_PIX_FMT_YUV444P10LE;
            break;
        case 12:
            result = AV_PIX_FMT_YUV444P12LE;
            break;
        case 14:
            result = AV_PIX_FMT_YUV444P14LE;
            break;
        case 16:
            result = AV_PIX_FMT_YUV444P16LE;
            break;
        default:
            if (bits_per_pixel < 8 || bits_per_pixel > 16) {
                av_log(avctx, AV_LOG_WARNING, "Unsupported chroma %d with %d bits per pixel", chroma, bits_per_pixel);
            } else {
                result = AV_PIX_FMT_YUV444P16LE;
            }
            break;
        }
        break;
    default:
        av_log(avctx, AV_LOG_WARNING, "Unsupported chroma %d with %d bits per pixel", chroma, bits_per_pixel);
    }
    return result;
}

static inline int get_output_bits_per_pixel(enum AVPixelFormat format) {
    switch (format) {
    case AV_PIX_FMT_GRAY8:
        return 8;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV444P:
        return 8;
    case AV_PIX_FMT_YUV420P9LE:
    case AV_PIX_FMT_YUV422P9LE:
    case AV_PIX_FMT_YUV444P9LE:
        return 9;
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_YUV422P10LE:
    case AV_PIX_FMT_YUV444P10LE:
        return 10;
    case AV_PIX_FMT_YUV420P12LE:
    case AV_PIX_FMT_YUV422P12LE:
    case AV_PIX_FMT_YUV444P12LE:
        return 12;
    case AV_PIX_FMT_YUV420P14LE:
    case AV_PIX_FMT_YUV422P14LE:
    case AV_PIX_FMT_YUV444P14LE:
        return 14;
    case AV_PIX_FMT_YUV420P16LE:
    case AV_PIX_FMT_YUV422P16LE:
    case AV_PIX_FMT_YUV444P16LE:
        return 16;
    default:
        return -1;
    }
}

static void free_spec(DE265Context *ctx, struct de265_image_spec* spec) {
    if (ctx->spec_queue_len < MAX_SPEC_QUEUE) {
        ctx->spec_queue[ctx->spec_queue_len++] = spec;
    } else {
        free(spec);
    }
}

static int ff_libde265dec_get_buffer(struct de265_image_intern* img,
                                     const struct de265_image_spec* spec,
                                     void* userdata)
{
    AVCodecContext *avctx = (AVCodecContext *) userdata;
    DE265Context *dectx = (DE265Context *) avctx->priv_data;

    enum de265_chroma chroma = spec->chroma;
    if (chroma != de265_chroma_mono) {
        if (spec->luma_bits_per_pixel != spec->chroma_bits_per_pixel) {
            goto fallback;
        }
    }

    int bits_per_pixel = spec->luma_bits_per_pixel;
    enum AVPixelFormat format = get_pixel_format(avctx, chroma, bits_per_pixel);
    if (format == AV_PIX_FMT_NONE) {
        goto fallback;
    }

    if (get_output_bits_per_pixel(format) != bits_per_pixel) {
        goto fallback;
    }

    AVFrame *frame = NULL;
    if (avctx->get_buffer2) {
        frame = av_frame_alloc();
        if (frame == NULL) {
            goto fallback;
        }

        frame->width = spec->visible_width;
        frame->height = spec->visible_height;
        frame->format = format;
        avctx->coded_width = align_value(spec->width, spec->alignment);
        avctx->coded_height = spec->height;
        avctx->pix_fmt = format;
        if (avctx->get_buffer2(avctx, frame, 0) < 0) {
            av_frame_free(&frame);
            goto fallback;
        }
    } else {
        if (dectx->frame_queue_len > 0) {
            frame = dectx->frame_queue[0];
            dectx->frame_queue_len--;
            if (dectx->frame_queue_len > 0) {
                memmove(dectx->frame_queue, &dectx->frame_queue[1], dectx->frame_queue_len * sizeof(AVFrame *));
            }
            if (frame->width != spec->width || frame->height != spec->height || frame->format != format) {
                av_frame_free(&frame);
            } else {
                av_frame_make_writable(frame);
            }
        }

        if (frame == NULL) {
            frame = av_frame_alloc();
            if (frame == NULL) {
                goto fallback;
            }

            frame->width = spec->width;
            frame->height = spec->height;
            frame->format = format;
            if (av_frame_get_buffer(frame, spec->alignment) != 0) {
                av_frame_free(&frame);
                goto fallback;
            }
        }
    }

    if (frame->width != spec->visible_width || frame->height != spec->visible_height) {
        // we might need to crop later
        struct de265_image_spec *spec_copy;
        if (dectx->spec_queue_len > 0) {
            spec_copy = dectx->spec_queue[--dectx->spec_queue_len];
        } else {
            spec_copy = (struct de265_image_spec *) malloc(sizeof(struct de265_image_spec));
            if (spec_copy == NULL) {
                av_frame_free(&frame);
                goto fallback;
            }
        }

        memcpy(spec_copy, spec, sizeof(struct de265_image_spec));
        frame->opaque = spec_copy;
    }

    int numplanes = (chroma == de265_chroma_mono ? 1 : 3);
    for (int i=0; i<numplanes; i++) {
        uint8_t *data = frame->data[i];
        if ((uintptr_t)data % spec->alignment) {
            av_frame_free(&frame);
            goto fallback;
        }

        int stride = frame->linesize[i];
        de265_set_image_plane_intern(img, i, data, stride, frame);
    }
    return 1;

fallback:
    return de265_get_default_image_allocation_functions()->get_buffer(img, spec, userdata);
}


static void ff_libde265dec_release_buffer(struct de265_image_intern* img, void* userdata)
{
    AVCodecContext *avctx = (AVCodecContext *) userdata;
    DE265Context *dectx = (DE265Context *) avctx->priv_data;
    AVFrame *frame = (AVFrame *) de265_get_image_plane_user_data_intern(img, 0);
    if (frame == NULL) {
        de265_get_default_image_allocation_functions()->release_buffer(img, userdata);
        return;
    }

    if (frame->opaque) {
        struct de265_image_spec *spec = (struct de265_image_spec *) frame->opaque;
        frame->opaque = NULL;
        free_spec(dectx, spec);
    }

    if (avctx->get_buffer2) {
        av_frame_free(&frame);
        return;
    }

    if (dectx->frame_queue_len == MAX_FRAME_QUEUE) {
        av_frame_free(&frame);
        return;
    }

    dectx->frame_queue[dectx->frame_queue_len++] = frame;
}


static void ff_libde265dec_enable_inexact_decoding(DE265Context *ctx) {
    if (ctx->deblocking) {
      de265_allow_inexact_decoding(ctx->decoder,
                                   de265_inexact_decoding_no_SAO        |
                                   de265_inexact_decoding_no_deblocking);
    } else {
      de265_allow_inexact_decoding(ctx->decoder, de265_inexact_decoding_mask_none);
    }
}


static int ff_libde265dec_decode(AVCodecContext *avctx,
                                 void *data, int *got_frame, AVPacket *avpkt)
{
    DE265Context *ctx = (DE265Context *) avctx->priv_data;
    AVFrame *picture = (AVFrame *) data;
    const struct de265_image *img;
    de265_error err;
    int ret;
    int64_t pts;
    int more = 0;

    const uint8_t* src[4];
    int stride[4];

    if (ctx->check_extra) {
        int extradata_size = avctx->extradata_size;
        ctx->check_extra = 0;
        if (extradata_size > 0) {
            unsigned char *extradata = (unsigned char *) avctx->extradata;
            if (extradata_size > 3 && extradata != NULL && (extradata[0] || extradata[1] || extradata[2] > 1)) {
                ctx->packetized = 1;
                if (extradata_size > 22) {
                    if (extradata[0] != 0) {
                        av_log(avctx, AV_LOG_WARNING, "Unsupported extra data version %d, decoding may fail\n", extradata[0]);
                    }
                    ctx->length_size = (extradata[21] & 3) + 1;
                    int num_param_sets = extradata[22];
                    int pos = 23;
                    for (int i=0; i<num_param_sets; i++) {
                        if (pos + 3 > extradata_size) {
                            av_log(avctx, AV_LOG_ERROR, "Buffer underrun in extra header (%d >= %d)\n", pos + 3, extradata_size);
                            return AVERROR_INVALIDDATA;
                        }
                        // ignore flags + NAL type (1 byte)
                        int nal_count  = extradata[pos+1] << 8 | extradata[pos+2];
                        pos += 3;
                        for (int j=0; j<nal_count; j++) {
                            if (pos + 2 > extradata_size) {
                                av_log(avctx, AV_LOG_ERROR, "Buffer underrun in extra nal header (%d >= %d)\n", pos + 2, extradata_size);
                                return AVERROR_INVALIDDATA;
                            }
                            int nal_size = extradata[pos] << 8 | extradata[pos+1];
                            if (pos + 2 + nal_size > extradata_size) {
                                av_log(avctx, AV_LOG_ERROR, "Buffer underrun in extra nal (%d >= %d)\n", pos + 2 + nal_size, extradata_size);
                                return AVERROR_INVALIDDATA;
                            }
                            err = de265_push_NAL(ctx->decoder, extradata + pos + 2, nal_size, 0, NULL);
                            if (err) {
                                av_log(avctx, AV_LOG_ERROR, "Failed to push data: %s (%d)\n", de265_get_error_text(err), err);
                                return AVERROR_INVALIDDATA;
                            }
                            pos += 2 + nal_size;
                        }
                    }
                }
                av_log(avctx, AV_LOG_DEBUG, "Assuming packetized data (%d bytes length)\n", ctx->length_size);
            } else {
                ctx->packetized = 0;
                av_log(avctx, AV_LOG_DEBUG, "Assuming non-packetized data\n");
                err = de265_push_data(ctx->decoder, extradata, extradata_size, 0, NULL);
                if (err) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to push extra data: %s (%d)\n", de265_get_error_text(err), err);
                    return AVERROR_INVALIDDATA;
                }
            }

            de265_push_end_of_NAL(ctx->decoder);
        }
    }

    if (avpkt->size > 0) {
        if (avpkt->pts != AV_NOPTS_VALUE) {
            pts = avpkt->pts;
        } else {
            pts = avctx->reordered_opaque;
        }

        if (ctx->packetized) {
            uint8_t* avpkt_data = avpkt->data;
            uint8_t* avpkt_end = avpkt->data + avpkt->size;
            while (avpkt_data + ctx->length_size <= avpkt_end) {
                int nal_size = 0;
                int i;
                for (i=0; i<ctx->length_size; i++) {
                    nal_size = (nal_size << 8) | avpkt_data[i];
                }
                err = de265_push_NAL(ctx->decoder, avpkt_data + ctx->length_size, nal_size, pts, NULL);
                if (err != DE265_OK) {
                    const char *error = de265_get_error_text(err);
                    av_log(avctx, AV_LOG_ERROR, "Failed to push data: %s\n", error);
                    return AVERROR_INVALIDDATA;
                }
                avpkt_data += ctx->length_size + nal_size;
            }
        } else {
            err = de265_push_data(ctx->decoder, avpkt->data, avpkt->size, pts, NULL);
            if (err != DE265_OK) {
                const char *error = de265_get_error_text(err);
                av_log(avctx, AV_LOG_ERROR, "Failed to push data: %s\n", error);
                return AVERROR_INVALIDDATA;
            }
        }
    } else {
        de265_push_end_of_stream(ctx->decoder);
    }


    // TODO: libde265 should support more fine-grained settings
    int deblocking = (avctx->skip_loop_filter < AVDISCARD_NONREF);
    if (deblocking != ctx->deblocking) {
        ctx->deblocking = deblocking;
        ff_libde265dec_enable_inexact_decoding(ctx);
    }

    int decode_ratio = (avctx->skip_frame < AVDISCARD_NONREF) ? 100 : 25;
    if (decode_ratio != ctx->decode_ratio) {
        ctx->decode_ratio = decode_ratio;
        de265_set_framerate_ratio(ctx->decoder, decode_ratio);
    }


    // block until we get an image, or until we need more input data

    int action = de265_get_action(ctx->decoder, 1);


    if ((img = de265_get_next_picture(ctx->decoder)) != NULL) {
        int width;
        int height;
        int bits_per_pixel = LIBDE265_FFMPEG_MAX(
                                 LIBDE265_FFMPEG_MAX(
                                     de265_get_bits_per_pixel(img, 0),
                                     de265_get_bits_per_pixel(img, 1)
                                 ),
                                 de265_get_bits_per_pixel(img, 2));
        enum de265_chroma chroma = de265_get_chroma_format(img);
        enum AVPixelFormat format = get_pixel_format(avctx, chroma, bits_per_pixel);
        if (format == AV_PIX_FMT_NONE) {
            return AVERROR_INVALIDDATA;
        }

        int numplanes = (chroma == de265_chroma_mono ? 1 : 3);
        avctx->pix_fmt = format;
        width  = de265_get_image_width(img,0);
        height = de265_get_image_height(img,0);
        if (width != avctx->width || height != avctx->height) {
            if (avctx->width != 0)
                av_log(avctx, AV_LOG_INFO, "dimension change! %dx%d -> %dx%d\n",
                       avctx->width, avctx->height, width, height);

            if (av_image_check_size(width, height, 0, avctx)) {
                return AVERROR_INVALIDDATA;
            }

            avcodec_set_dimensions(avctx, width, height);
        }


        AVFrame *frame = (AVFrame *) de265_get_image_plane_user_data(img, 0);
        if (frame != NULL) {
            av_frame_ref(picture, frame);
            if (frame->opaque) {
                // Cropping needed.
                struct de265_image_spec *spec = (struct de265_image_spec *) frame->opaque;
                frame->opaque = NULL;
                picture->width = spec->visible_width;
                picture->height = spec->visible_height;
                for (int i=0; i<numplanes; i++) {
                    int shift = (i == 0) ? 0 : 1;
                    int offset = (spec->crop_left >> shift) + (spec->crop_top >> shift) * picture->linesize[i];
                    picture->data[i] += offset;
                }
                free_spec(ctx, spec);
            }
        } else {
            picture->width = avctx->width;
            picture->height = avctx->height;
            picture->format = avctx->pix_fmt;
            if (avctx->get_buffer2 != NULL) {
                ret = avctx->get_buffer2(avctx, picture, 0);
            } else {
                ret = av_frame_get_buffer(picture, 32);
            }
            if (ret < 0) {
                return ret;
            }

            for (int i=0;i<=3;i++) {
                if (i<numplanes) {
                    src[i] = de265_get_image_plane(img, i, &stride[i]);
                } else {
                    src[i] = NULL;
                    stride[i] = 0;
                }
            }

            int equal_strides = 1;
            for (int i=1; i<numplanes; i++) {
                if (stride[i-1] != stride[i]) {
                    equal_strides = 0;
                    break;
                }
            }
            if (equal_strides) {
                // All input planes match the output planes, copy directly.
                av_image_copy(picture->data, picture->linesize, src, stride,
                              avctx->pix_fmt, width, height);
            } else {
                int max_bits_per_pixel = get_output_bits_per_pixel(format);
                for (int i=0; i<numplanes; i++) {
                    int plane_width = de265_get_image_width(img, i);
                    int plane_height = de265_get_image_height(img, i);
                    int plane_bits_per_pixel = de265_get_bits_per_pixel(img, i);
                    int size = LIBDE265_FFMPEG_MIN(stride[i], picture->linesize[i]);
                    uint8_t* src_ptr = (uint8_t*) src[i];
                    uint8_t* dst_ptr = (uint8_t*) picture->data[i];
                    if (plane_bits_per_pixel > max_bits_per_pixel) {
                        // More bits per pixel in this plane than supported by the output format
                        int shift = (plane_bits_per_pixel - max_bits_per_pixel);
                        for( int line = 0; line < plane_height; line++ ) {
                            uint16_t *s = (uint16_t *) src_ptr;
                            uint16_t *d = (uint16_t *) dst_ptr;
                            for (int pos=0; pos<size/2; pos++) {
                                *d = *s >> shift;
                                d++;
                                s++;
                            }
                            src_ptr += stride[i];
                            dst_ptr += picture->linesize[i];
                        }
                    } else if (plane_bits_per_pixel < max_bits_per_pixel && plane_bits_per_pixel > 8) {
                        // Less bits per pixel in this plane than the rest of the picture
                        // but more than 8bpp.
                        int shift = (max_bits_per_pixel - plane_bits_per_pixel);
                        for( int line = 0; line < plane_height; line++ ) {
                            uint16_t *s = (uint16_t *) src_ptr;
                            uint16_t *d = (uint16_t *) dst_ptr;
                            for (int pos=0; pos<size/2; pos++) {
                                *d = *s << shift;
                                d++;
                                s++;
                            }
                            src_ptr += stride[i];
                            dst_ptr += picture->linesize[i];
                        }
                    } else if (plane_bits_per_pixel < max_bits_per_pixel && plane_bits_per_pixel == 8) {
                        // 8 bits per pixel in this plane, which is less than the rest of the picture.
                        int shift = (max_bits_per_pixel - plane_bits_per_pixel);
                        for( int line = 0; line < plane_height; line++ ) {
                            uint8_t *s = (uint8_t *) src_ptr;
                            uint16_t *d = (uint16_t *) dst_ptr;
                            for (int pos=0; pos<size; pos++) {
                                *d = *s << shift;
                                d++;
                                s++;
                            }
                            src_ptr += stride[i];
                            dst_ptr += picture->linesize[i];
                        }
                    } else {
                        // Bits per pixel of plane match output format.
                        av_image_copy_plane(picture->data[i], picture->linesize[i],
                                            src[i], stride[i], size, plane_height);

                    }
                }
            }
        }

        *got_frame = 1;

        picture->reordered_opaque = de265_get_image_PTS(img);
        picture->pkt_pts = de265_get_image_PTS(img);

        de265_release_picture(img);
    }
    return avpkt->size;
}


static av_cold int ff_libde265dec_free(AVCodecContext *avctx)
{
    DE265Context *ctx = (DE265Context *) avctx->priv_data;
    de265_free_decoder(ctx->decoder);

    while (ctx->frame_queue_len) {
        AVFrame *frame = ctx->frame_queue[--ctx->frame_queue_len];
        av_frame_free(&frame);
    }
    while (ctx->spec_queue_len) {
        struct de265_image_spec *spec = ctx->spec_queue[--ctx->spec_queue_len];
        free(spec);
    }

    return 0;
}


static av_cold void ff_libde265dec_flush(AVCodecContext *avctx)
{
    DE265Context *ctx = (DE265Context *) avctx->priv_data;
    de265_reset(ctx->decoder);
}


static av_cold void ff_libde265dec_static_init(struct AVCodec *codec)
{
    // No initialization required
}


static av_cold int ff_libde265dec_ctx_init(AVCodecContext *avctx)
{
    DE265Context *ctx = (DE265Context *) avctx->priv_data;
    ctx->decoder = de265_new_decoder();

    struct de265_image_allocation allocation;
    allocation.get_buffer = ff_libde265dec_get_buffer;
    allocation.release_buffer = ff_libde265dec_release_buffer;
    allocation.allocation_userdata = avctx;
    de265_set_image_allocation_functions(ctx->decoder, &allocation);

    ctx->check_extra = 1;
    ctx->packetized = 1;
    ctx->length_size = 4;
    ctx->deblocking = 1;
    ctx->decode_ratio = 100;
    ctx->frame_queue_len = 0;
    ctx->spec_queue_len = 0;

    int nFramesParallel = av_cpu_count() / 2; // TODO: how should we set this optimally ?
    if (nFramesParallel<=1) { nFramesParallel=2; }
    if (nFramesParallel==4) { nFramesParallel=5; } // 5 frames are typically much faster than 4

    int nThreads = nFramesParallel * 5;

    /*
    if (1 || avctx->active_thread_type & FF_THREAD_SLICE) {
        int threads = avctx->thread_count;
        if (threads <= 0) {
            threads = av_cpu_count();
        }
    */

    de265_set_max_frames_to_decode_in_parallel(ctx->decoder, nFramesParallel);
    de265_start_worker_threads(ctx->decoder, nThreads);

    // Set a max pictures latency in case we are switching input channels without decoder reset.
    // TODO: this should be a decoder option.
    de265_set_max_reorder_buffer_latency(ctx->decoder, 50);

    ff_libde265dec_enable_inexact_decoding(ctx);

    return 0;
}

static void ff_libde265dec_unregister_codecs(enum AVCodecID id)
{
    AVCodec *prev = NULL;
    AVCodec *codec = av_codec_next(NULL);
    while (codec != NULL) {
        AVCodec *next = av_codec_next(codec);
        if (codec->id == id) {
            if (prev != NULL) {
                // remove previously registered codec with the same id
                // NOTE: this won't work for the first registered codec
                //       which is fine for this use case
                prev->next = next;
            } else {
                prev = codec;
            }
        } else {
            prev = codec;
        }
        codec = next;
    }
}


AVCodec ff_libde265_decoder;

void libde265dec_register(void)
{
    static int registered = 0;

    if (registered) {
        return;
    }

    registered = 1;
    ff_libde265dec_unregister_codecs(AV_CODEC_ID_HEVC);
    memset(&ff_libde265_decoder, 0, sizeof(AVCodec));
    ff_libde265_decoder.name           = "libde265";
    ff_libde265_decoder.type           = AVMEDIA_TYPE_VIDEO;
    ff_libde265_decoder.id             = AV_CODEC_ID_HEVC;
    ff_libde265_decoder.priv_data_size = sizeof(DE265Context);
    ff_libde265_decoder.init_static_data = ff_libde265dec_static_init;
    ff_libde265_decoder.init           = ff_libde265dec_ctx_init;
    ff_libde265_decoder.close          = ff_libde265dec_free;
    ff_libde265_decoder.decode         = ff_libde265dec_decode;
    ff_libde265_decoder.flush          = ff_libde265dec_flush;
    ff_libde265_decoder.capabilities   = CODEC_CAP_DELAY | CODEC_CAP_AUTO_THREADS | CODEC_CAP_DR1 |
                                         CODEC_CAP_SLICE_THREADS;
    ff_libde265_decoder.long_name      = "libde265 H.265/HEVC decoder";

    avcodec_register(&ff_libde265_decoder);
}
