/*
 * AVS2 decoding using the davs2 library
 *
 * Copyright (C) 2018 Yiqun Xu, <yiqun.xu@vipl.ict.ac.cn>
 *                    Falei Luo, <falei.luo@gmail.com>
 *                    Huiwen Ren, <hwrenx@gmail.com>
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

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/avutil.h"
#include "avcodec.h"
#include "libavutil/imgutils.h"
#include "internal.h"

#include <davs2.h>

typedef struct DAVS2Context {
    void *decoder;

    AVFrame *frame;
    davs2_param_t    param;      // decoding parameters
    davs2_packet_t   packet;     // input bitstream

    int decoded_frames;

    davs2_picture_t  out_frame;  // output data, frame data
    davs2_seq_info_t headerset;  // output data, sequence header

}DAVS2Context;

static av_cold davs2_init(AVCodecContext *avctx)
{
    DAVS2Context *cad = avctx->priv_data;

    /* init the decoder */
    cad->param.threads      = avctx->thread_count;
    cad->param.i_info_level = 0;
    cad->decoder = davs2_decoder_open(&cad->param);
    avctx->flags |= AV_CODEC_FLAG_TRUNCATED;

    av_log(avctx, AV_LOG_VERBOSE, "decoder created. %p\n", cad->decoder);
    return 0;
}

static int davs_dump_frames(AVCodecContext *avctx, davs2_picture_t *pic, davs2_seq_info_t *headerset, AVFrame *frame)
{
    DAVS2Context *cad = avctx->priv_data;
    avctx->flags |= AV_CODEC_FLAG_TRUNCATED;
    int bytes_per_sample = pic->bytes_per_sample;
    int i;

    if (!headerset)
        return 0;

    if (!pic || pic->ret_type == DAVS2_GOT_HEADER) {
        avctx->width        = headerset->horizontal_size;
        avctx->height       = headerset->vertical_size;
        avctx->pix_fmt      = headerset->output_bitdepth == 10 ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P;

        AVRational r = av_d2q(headerset->frame_rate,4096);
        avctx->framerate.num = r.num;
        avctx->framerate.den = r.den;
        return 0;
    }

    for (i = 0; i < 3; ++i) {
        int size_plane = pic->width[i] * pic->lines[i] * bytes_per_sample;
        frame->buf[i]      = av_buffer_alloc(size_plane);
        frame->data[i]     = frame->buf[i]->data;
        frame->linesize[i] = pic->width[i] * bytes_per_sample;
        if (!frame->buf[i] || !frame->data[i] || !frame->linesize[i]){
            av_log(avctx, AV_LOG_ERROR, "dump error: alloc failed.\n");
            return AVERROR(EINVAL);
        }
        memcpy(frame->data[i], pic->planes[i], size_plane);
    }

    frame->width     = cad->headerset.horizontal_size;
    frame->height    = cad->headerset.vertical_size;
    frame->pts       = cad->out_frame.pts;
    frame->pict_type = pic->type;
    frame->format    = avctx->pix_fmt;

    cad->decoded_frames++;
    return 1;
}

static av_cold davs2_end(AVCodecContext *avctx)
{
    DAVS2Context *cad = avctx->priv_data;

    /* close the decoder */
    if (cad->decoder) {
        davs2_decoder_close(cad->decoder);
        av_log(avctx, AV_LOG_VERBOSE, "decoder destroyed. %p; frames %d\n", cad->decoder, cad->decoded_frames);
        cad->decoder = NULL;
    }

    return 0;
}

static int davs2_decode_frame(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
{
    DAVS2Context *cad = avctx->priv_data;
    int buf_size       = avpkt->size;
    uint8_t *buf_ptr = avpkt->data;
    AVFrame *frame = data;
    int ret = 0;

    *got_frame = 0;
    cad->frame = frame;
    avctx->flags |= AV_CODEC_FLAG_TRUNCATED;

    if (!buf_size) {
        cad->packet.data = buf_ptr;
        cad->packet.len  = buf_size;
        cad->packet.pts  = avpkt->pts;
        cad->packet.dts  = avpkt->dts;

        while (1) {
            ret = davs2_decoder_flush(cad->decoder, &cad->headerset, &cad->out_frame);

            if (ret < 0)
                return 0;

            if (cad->out_frame.ret_type != DAVS2_DEFAULT) {
                *got_frame = davs_dump_frames(avctx, &cad->out_frame, &cad->headerset, frame);
                davs2_decoder_frame_unref(cad->decoder, &cad->out_frame);
            }
            if (!*got_frame)
                break;
        }
        return 0;
    } else {
        while (buf_size > 0) {
            int len = buf_size;   // for API-3, pass all data in

            cad->packet.marker = 0;
            cad->packet.data = buf_ptr;
            cad->packet.len  = len;
            cad->packet.pts  = avpkt->pts;
            cad->packet.dts  = avpkt->dts;

            len = davs2_decoder_decode(cad->decoder, &cad->packet, &cad->headerset, &cad->out_frame);

            if (cad->out_frame.ret_type != DAVS2_DEFAULT) {
                *got_frame = davs_dump_frames(avctx, &cad->out_frame, &cad->headerset, frame);
                davs2_decoder_frame_unref(cad->decoder, &cad->out_frame);
            }

            if (len < 0) {
                av_log(avctx, AV_LOG_ERROR, "A decoder error counted\n");
                if (cad->decoder) {
                    davs2_decoder_close(cad->decoder);
                    av_log(avctx, AV_LOG_VERBOSE, "decoder destroyed. %p; frames %d\n", cad->decoder, cad->decoded_frames);
                    cad->decoder = NULL;
                }
                return AVERROR(EINVAL);
            }

            buf_ptr     += len;
            buf_size    -= len;

            if (!*got_frame)
                break;
        }
    }

    buf_size = (buf_ptr - avpkt->data);

    return buf_size;
}

AVCodec libdavs2_decoder = {
    .name           = "libdavs2",
    .long_name      = NULL_IF_CONFIG_SMALL("Decoder for AVS2/IEEE 1857.4"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_AVS2,
    .priv_data_size = sizeof(DAVS2Context),
    .init           = davs2_init,
    .close          = davs2_end,
    .decode         = davs2_decode_frame,
    .capabilities   =  AV_CODEC_CAP_DELAY,//AV_CODEC_CAP_DR1 |
    .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10,
                                                     AV_PIX_FMT_NONE },
    .wrapper_name   = "libdavs2",
};
