/*
 * AVS2 encoding using the xavs2 library
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

#include <xavs2.h>
#include <ctype.h>

#include "avcodec.h"
#include "internal.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/avutil.h"

#define DELAY_FRAMES 8

typedef struct XAVS2EContext {
    AVClass        *class;

    void*         handle;

    int i_lcurow_threads;
    int i_frame_threads;
    int i_initial_qp;
    int preset_level;
    int intra_period;
    int sourcewigth;
    int sourceheight;

    void *encoder;
    char *xavs2_opts;

    int b_hierarchical_reference;
    int num_b_frames;

    xavs2_outpacket_t packet;
    xavs2_param_t *param;

    const xavs2_api_t *api;

} XAVS2EContext;

// TODO: use rationals instead.
static const float AVS2_FRAME_RATE[8] = {
    24000.0f / 1001.0f, 24.0f, 25.0f, 30000.0f / 1001.0f, 30.0f, 50.0f, 60000.0f / 1001.0f, 60.0f
};

// TODO: replace function by ff_mpeg12_find_best_frame_rate(avctx->framerate,&code,NULL,NULL,0);
static int xavs2e_find_framerate_code(AVCodecContext *avctx)
{
    int fps_num = avctx->time_base.den;
    int fps_den = avctx->time_base.num;
    float fps = (float)(fps_num) / (float)(fps_den);
    int i;

    av_log(avctx, AV_LOG_DEBUG, "frame rate: %d/%d, %.3f\n", fps_num, fps_den, fps);
    for (i = 0; i < 7; i++) {
         if (fps <= AVS2_FRAME_RATE[i]) {
             break;
         }
    }
    av_log(avctx, AV_LOG_DEBUG, "frame rate code: %d\n", i + 1);
    return i + 1;
}

static av_cold int xavs2e_init(AVCodecContext *avctx)
{
    XAVS2EContext *cae= avctx->priv_data;

    char str_bd[16], str_iqp[16], str_w[16], str_h[16], str_preset[16], str_hr[16], str_bf[16];
    char str_iv[16], str_TBR[16], str_fr[16];

    if (avctx->pix_fmt == AV_PIX_FMT_YUV420P) {
        /* get API handler */
        cae->api = xavs2_api_get(8);
        cae->param = cae->api->opt_alloc();

        if (!cae->param) {
            av_log(avctx, AV_LOG_ERROR, "param alloc failed.\n");
            return AVERROR(EINVAL);
        }

        sprintf(str_bd, "%d", 8);
        sprintf(str_iqp, "%d", 32);

        cae->api->opt_set2(cae->param, "bitdepth", str_bd);
        cae->api->opt_set2(cae->param, "initial_qp", str_iqp);
    } else if (avctx->pix_fmt == AV_PIX_FMT_YUV420P10) {
        /* get API handler */
        cae->api = xavs2_api_get(10);

        cae->param = cae->api->opt_alloc();

        sprintf(str_bd, "%d", 10);
        sprintf(str_iqp, "%d", 45);

        cae->api->opt_set2(cae->param, "bitdepth", str_bd);
        cae->api->opt_set2(cae->param, "initial_qp", str_iqp);
    }

    sprintf(str_w, "%d", avctx->width);
    sprintf(str_h, "%d", avctx->height);

    cae->api->opt_set2(cae->param,"width",str_w);
    cae->api->opt_set2(cae->param,"height",str_h);
    cae->api->opt_set2(cae->param,"rec","0");
    cae->api->opt_set2(cae->param,"log","0");

    /* preset level */
    sprintf(str_preset, "%d", cae->preset_level);

    cae->api->opt_set2(cae->param,"preset", str_preset);
    /* bframes */
    av_log(avctx, AV_LOG_DEBUG,
          "HierarchicalReference %d, Number B Frames %d.\n", cae->b_hierarchical_reference, cae->num_b_frames);
    
    sprintf(str_hr, "%d", cae->b_hierarchical_reference);
    // sprintf(str_bf, "%d", cae->num_b_frames);
    sprintf(str_bf, "%d", avctx->max_b_frames);

    cae->api->opt_set2(cae->param, "hierarchical_ref",  str_hr);
    cae->api->opt_set2(cae->param, "bframes",  str_bf);

    if (cae->xavs2_opts) {
        AVDictionary *dict    = NULL;
        AVDictionaryEntry *en = NULL;

        if (!av_dict_parse_string(&dict, cae->xavs2_opts, "=", ":", 0)) {
            while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX))) {
                int i_value = isdigit(en->value[0]) ? atoi(en->value) : 0;

                sprintf(str_iv, "%d", i_value);

                int parse_ret = cae->api->opt_set2(cae->param, en->key, str_iv);

                if (parse_ret < 0) {
                    av_log(avctx, AV_LOG_WARNING,
                          "Invalid value for %s: %s.\n", en->key, en->value);
                }
            }
            av_dict_free(&dict);
        }
    }

    /* Rate control */
    if (avctx->bit_rate > 0) {
        cae->api->opt_set2(cae->param, "RateControl",  "1");  // VBR

        sprintf(str_TBR, "%d", avctx->bit_rate);

        cae->api->opt_set2(cae->param, "TargetBitRate", str_TBR);
    }
    cae->api->opt_set2(cae->param, "intraperiod", "50");

    sprintf(str_fr, "%d", xavs2e_find_framerate_code(avctx));

    cae->api->opt_set2(cae->param, "FrameRate", str_fr);

    cae->encoder = cae->api->encoder_create(cae->param);

    if (cae->encoder == NULL) {
        av_log(avctx,AV_LOG_ERROR, "Can not create encoder. Null pointer returned.\n");

        return AVERROR(EINVAL);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 */
static void dump_encoded_data(AVCodecContext *avctx, void *coder, xavs2_outpacket_t *packet)
{
     XAVS2EContext *cae= avctx->priv_data;
     cae->api->encoder_packet_unref(coder, packet);
}

static int xavs2e_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                      const AVFrame *frame, int *got_packet)
{
    XAVS2EContext *cae = avctx->priv_data;

    xavs2_picture_t pic;

    int j, k;

    /* create the XAVS2 video encoder */
    /* read frame data and send to the XAVS2 video encoder */
    if (cae->api->encoder_get_buffer(cae->encoder, &pic) < 0) {
        fprintf(stderr, "failed to get frame buffer [%3d].\n", pic.i_pts);
        return -1; 
    }
    if (frame) {  

        switch (frame->format) {
            case AV_PIX_FMT_YUV420P:
                if (pic.img.in_sample_size != pic.img.enc_sample_size) {
                    const int shift_in = atoi(cae->api->opt_get(cae->param, "SampleShift"));
                    for (k = 0; k < 3; k++) {
                        int i_stride = pic.img.i_stride[k];
                        for (j = 0; j < pic.img.i_lines[k]; j++) {
                            uint16_t *p_plane = (uint16_t *)&pic.img.img_planes[k][j * i_stride];
                            int i;
                            uint8_t *p_buffer = frame->data[k] + frame->linesize[k] * j;
                            memset(p_plane, 0, i_stride);
                            for (i = 0; i < pic.img.i_width[k]; i++) {
                                p_plane[i] = p_buffer[i] << shift_in;
                            }
                        }
                    }
                } else {
                    for (k = 0; k < 3; k++) {
                        for (j = 0; j < pic.img.i_lines[k]; j++) {
                            memcpy(pic.img.img_planes[k] + pic.img.i_stride[k] * j, frame->data[k]+frame->linesize[k] * j, pic.img.i_width[k] * pic.img.in_sample_size);
                        }
                    }
// TODO: if our library look at the frame data after the encode call, hold the references to the input buffer. if not, remove this copy.
                }
            break;
            case AV_PIX_FMT_YUV420P10:
                if (pic.img.in_sample_size == 2) {
                    for (k = 0; k < 3; k++) {
                        for (j = 0; j < pic.img.i_lines[k]; j++) {
                            memcpy(pic.img.img_planes[k] + pic.img.i_stride[k] * j, frame->data[k]+frame->linesize[k] * j, pic.img.i_width[k] * pic.img.in_sample_size);
                        }
                    }
                } else {
                    av_log(avctx, AV_LOG_ERROR,
                          "Unsupportted input pixel format: 10-bit is not supported.\n");
                }
            break;
            default:
                av_log(avctx, AV_LOG_ERROR,
                      "Unsupportted pixel format\n");
            break;
        }

        pic.i_state = 0;
        pic.i_pts  = frame->pts;
        pic.i_type = XAVS2_TYPE_AUTO;

// TODO: check error
        cae->api->encoder_encode(cae->encoder, &pic, &cae->packet);
        dump_encoded_data(avctx, cae->encoder, &cae->packet);
    } else {
        cae->api->encoder_encode(cae->encoder, NULL, &cae->packet);
        dump_encoded_data(avctx, cae->encoder, &cae->packet);
    }

    if((cae->packet.len != 0) && (cae->packet.state != XAVS2_STATE_FLUSH_END)){
        av_new_packet(pkt, cae->packet.len); 

        if (!pkt) {
            av_log(avctx, AV_LOG_ERROR, "packet alloc failed.\n");
            return AVERROR(EINVAL);
        }

        pkt->pts = cae->packet.pts;
        pkt->dts = cae->packet.dts;

// TODO: avoide this copy by holding a reference to the output data?
        memcpy(pkt->data, cae->packet.stream, cae->packet.len);
        pkt->data=cae->packet.len;

        pkt->size = cae->packet.len;
        *got_packet = 1;
    } else {
        *got_packet = 0;
    }

    return 0;
}

static av_cold int xavs2e_close(AVCodecContext *avctx)
{
    XAVS2EContext *cae = avctx->priv_data;
    /* destroy the encoder */
    if(cae->api) {
        cae->api->encoder_destroy(cae->encoder);

        if (cae->param) {
            cae->api->opt_destroy(cae->param);
        }
    }
   
    return 0;
}

#define OFFSET(x) offsetof(XAVS2EContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
   { "i_lcurow_threads",           "number of parallel threads for rows"     ,       OFFSET(i_lcurow_threads),  AV_OPT_TYPE_INT,    {.i64 =  5 }, 1,   8,  VE },
// TODO: replace by AVCodecContext.thread_count.
    { "i_frame_threads" ,           "number of parallel threads for frames"   ,       OFFSET(i_frame_threads) ,  AV_OPT_TYPE_INT,    {.i64 =  1 }, 1,   4,  VE },
    { "i_initial_qp"    ,           "Quantization parameter",       OFFSET(i_initial_qp)    ,  AV_OPT_TYPE_INT,    {.i64 = 34 }, 1,  63,  VE },
    { "preset_level"    ,           "Speed level"           ,       OFFSET(preset_level)    ,  AV_OPT_TYPE_INT,    {.i64 =  0 }, 0,   9,  VE },
    { "intra_period"    ,           "Intra period"          ,       OFFSET(intra_period)    ,  AV_OPT_TYPE_INT,    {.i64 =  4 }, 3, 100,  VE },
    { "hierarchical_ref",           "hierarchical reference",       OFFSET(b_hierarchical_reference)    ,  AV_OPT_TYPE_INT,    {.i64 =  1 }, 0, 1,  VE },
    { "num_bframes"     ,           "number of B frames"    ,       OFFSET(num_b_frames)    ,  AV_OPT_TYPE_INT,    {.i64 =  7 }, 0,  15,  VE },
    { "xavs2-params",    "set the xavs2 configuration using a :-separated list of key=value parameters", OFFSET(xavs2_opts), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },
    { NULL },
};

static const AVClass xavs2e_class = {
    .class_name = "XAVS2EContext",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault xavs2e_defaults[] = {
    { "b",                "0" },
    { NULL },
};

AVCodec ff_libxavs2_encoder = {
    .name           = "libxavs2",
    .long_name      = NULL_IF_CONFIG_SMALL("xavs2 Chinese AVS2 (Audio Video Standard)"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_AVS2,
    .priv_data_size = sizeof(XAVS2EContext),
    .init           = xavs2e_init,
    .encode2        = xavs2e_encode_frame,
    .close          = xavs2e_close,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
    .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10, AV_PIX_FMT_NONE },
    .priv_class     = &xavs2e_class,
    .defaults       = xavs2e_defaults,
    .wrapper_name   = "libxavs2",
} ;
