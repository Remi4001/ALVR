#include "EncodePipelineVulkan.h"
#include "ALVR-common/packet_types.h"
#include "VkContext.hpp"
#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "ffmpeg_helper.h"
#include <chrono>
#include <libavutil/pixfmt.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

using alvr::Vendor;

namespace {

const char* encoder(ALVR_CODEC codec) {
    switch (codec) {
    case ALVR_CODEC_H264:
        return "h264_vulkan";
    case ALVR_CODEC_HEVC:
        return "hevc_vulkan";
    case ALVR_CODEC_AV1:
        return "av1_vulkan";
    }
    throw std::runtime_error("invalid codec " + std::to_string(codec));
}

void set_hwframe_ctx(AVCodecContext* ctx, AVBufferRef* hw_device_ctx) {
    AVBufferRef* hw_frames_ref;
    AVHWFramesContext* frames_ctx = NULL;
    int err = 0;

    if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx))) {
        throw std::runtime_error("Failed to create Vulkan frame context.");
    }
    frames_ctx = (AVHWFramesContext*)(hw_frames_ref->data);
    frames_ctx->format = AV_PIX_FMT_VULKAN;
    frames_ctx->sw_format = (Settings::Instance().m_codec == ALVR_CODEC_HEVC
                             || Settings::Instance().m_codec == ALVR_CODEC_AV1)
            && Settings::Instance().m_use10bitEncoder
        ? AV_PIX_FMT_P010
        : AV_PIX_FMT_NV12;
    frames_ctx->width = ctx->width;
    frames_ctx->height = ctx->height;
    frames_ctx->initial_pool_size = 3;
    if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
        av_buffer_unref(&hw_frames_ref);
        throw alvr::AvException("Failed to initialize Vulkan frame context:", err);
    }
    ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!ctx->hw_frames_ctx)
        err = AVERROR(ENOMEM);

    av_buffer_unref(&hw_frames_ref);
}

}

alvr::EncodePipelineVulkan::EncodePipelineVulkan(
    alvr::HWContext& vk_ctx,
    std::string devicePath,
    alvr::Vendor vendor,
    VkFrame& input_frame, 
    uint32_t width, 
    uint32_t height
) {
    /* Vulkan Encoding pipeline
     * The encoding pipeline has 3 frame types:
     * - input vulkan frames, only used to initialize the mapped frames
     * - mapped frames, one per input frame, same format, and point to the same memory on the device
     * - encoder frame, with a format compatible with the encoder, created by the filter
     * Each frame type has a corresponding hardware frame context, the vulkan one is provided
     *
     * The pipeline is simply made of a scale_vaapi object, that does the conversion between formats
     * and the encoder that takes the converted frame and produces packets.
     */
    this->hw_ctx = vk_ctx.avCtx;

    int err = 0;
    // TODO: useful?
    // int err = av_hwdevice_ctx_init(hw_ctx);
    // if (err < 0) {
    //     throw alvr::AvException("Failed to create Vulkan device:", err);
    // }

    const auto& settings = Settings::Instance();

    auto codec_id = ALVR_CODEC(settings.m_codec);
    const char* encoder_name = encoder(codec_id);
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name);
    if (codec == nullptr) {
        throw std::runtime_error(std::string("Failed to find encoder ") + encoder_name);
    }

    encoder_ctx = avcodec_alloc_context3(codec);
    if (not encoder_ctx) {
        throw std::runtime_error("failed to allocate Vulkan encoder");
    }

    encoder_ctx->gop_size = INT_MAX;

    switch (codec_id) {
    case ALVR_CODEC_H264:
        switch (settings.m_h264Profile) {
        case ALVR_H264_PROFILE_BASELINE:
            encoder_ctx->profile = AV_PROFILE_H264_BASELINE;
            break;
        case ALVR_H264_PROFILE_MAIN:
            encoder_ctx->profile = AV_PROFILE_H264_MAIN;
            break;
        default:
        case ALVR_H264_PROFILE_HIGH:
            encoder_ctx->profile = AV_PROFILE_H264_HIGH;
            break;
        }

        switch (settings.m_entropyCoding) {
        case ALVR_CABAC:
            av_opt_set(encoder_ctx->priv_data, "coder", "cabac", 0);
            break;
        case ALVR_CAVLC:
            av_opt_set(encoder_ctx->priv_data, "coder", "vlc", 0);
            break;
        }

        break;
    case ALVR_CODEC_HEVC:
        encoder_ctx->profile = Settings::Instance().m_use10bitEncoder ? AV_PROFILE_HEVC_MAIN_10
                                                                      : AV_PROFILE_HEVC_MAIN;
        encoder_ctx->gop_size = INT16_MAX;
        break;
    case ALVR_CODEC_AV1:
        encoder_ctx->profile = AV_PROFILE_AV1_MAIN;
        break;
    }

    switch (settings.m_rateControlMode) {
    case ALVR_VBR:
        av_opt_set(encoder_ctx->priv_data, "rc_mode", "vbr", 0);
        break;
    case ALVR_CBR:
    default:
        av_opt_set(encoder_ctx->priv_data, "rc_mode", "cbr", 0);
        break;
    }

    // av_opt_set_int(encoder_ctx->priv_data, "filler_data", settings.m_fillerData, 0);

    encoder_ctx->width = width;
    encoder_ctx->height = height;
    encoder_ctx->time_base = { 1, (int)1e9 };
    encoder_ctx->sample_aspect_ratio = AVRational { 1, 1 };
    encoder_ctx->pix_fmt = AV_PIX_FMT_VULKAN;
    encoder_ctx->max_b_frames = 0;
    // encoder_ctx->color_range = AVCOL_RANGE_JPEG;

    auto params = FfiDynamicEncoderParams {};
    params.updated = true;
    params.bitrate_bps = 30'000'000;
    params.framerate = settings.m_refreshRate;
    SetParams(params);

    vlVaQualityBits quality = {};
    quality.vbaq_mode
        = Settings::Instance()
              .m_enableVbaq; // No noticable performance difference and should improve subjective
                             // quality by allocating more bits to smooth areas
    // switch (settings.m_encoderQualityPreset) {
    // case ALVR_QUALITY:
    //     if (vendor == Vendor::Amd) {
    //         quality.preset_mode = PRESET_MODE_QUALITY;
    //         encoder_ctx->compression_level
    //             = quality.quality; // (QUALITY preset, no pre-encoding, vbaq)
    //     } else if (vendor == Vendor::Intel) {
    //         encoder_ctx->compression_level = 1;
    //     }
    //     break;
    // case ALVR_BALANCED:
    //     if (vendor == Vendor::Amd) {
    //         quality.preset_mode = PRESET_MODE_BALANCE;
    //         encoder_ctx->compression_level
    //             = quality.quality; // (BALANCE preset, no pre-encoding, vbaq)
    //     } else if (vendor == Vendor::Intel) {
    //         encoder_ctx->compression_level = 4;
    //     }
    //     break;
    // case ALVR_SPEED:
    // default:
    //     if (vendor == Vendor::Amd) {
    //         quality.preset_mode = PRESET_MODE_SPEED;
    //         encoder_ctx->compression_level
    //             = quality.quality; // (speed preset, no pre-encoding, vbaq)
    //     } else if (vendor == Vendor::Intel) {
    //         encoder_ctx->compression_level = 7;
    //     }
    //     break;
    // }

    // av_opt_set_int(encoder_ctx->priv_data, "async_depth", 1, 0);

    set_hwframe_ctx(encoder_ctx, hw_ctx);

    err = avcodec_open2(encoder_ctx, codec, NULL);
    if (err < 0) {
        throw alvr::AvException("Cannot open video encoder codec:", err);
    }

    AVBufferRef* hw_frames_ref;
    if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_ctx))) {
        throw std::runtime_error("Failed to create Vulkan frame context.");
    }
    auto frames_ctx = (AVHWFramesContext*)(hw_frames_ref->data);
    frames_ctx->format = AV_PIX_FMT_VULKAN;
    frames_ctx->sw_format = input_frame.avFormat();
    frames_ctx->width = input_frame.imageInfo().extent.width;
    frames_ctx->height = input_frame.imageInfo().extent.height;
    frames_ctx->initial_pool_size = 1;
    if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
        av_buffer_unref(&hw_frames_ref);
        throw alvr::AvException("Failed to initialize Vulkan frame context:", err);
    }

    encoder_frame = av_frame_alloc();

    filter_graph = avfilter_graph_alloc();

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();

    // TODO: Respect colorspace
    std::stringstream buffer_filter_args;
    buffer_filter_args << "video_size=" << encoder_frame->width << "x" << encoder_frame->height;
    buffer_filter_args << ":time_base=" << encoder_ctx->time_base.num << "/"
                       << encoder_ctx->time_base.den;
    filter_in = avfilter_graph_alloc_filter(filter_graph, avfilter_get_by_name("buffer"), "in");
    if (!filter_in) {
        throw std::runtime_error("filter_in allocation failed");
    }
    AVBufferSrcParameters* par = av_buffersrc_parameters_alloc();
    par->format = encoder_frame->format;
    par->hw_frames_ctx = av_buffer_ref(encoder_frame->hw_frames_ctx);
    av_buffersrc_parameters_set(filter_in, par);
    av_free(par);
    if ((err = avfilter_init_str(filter_in, buffer_filter_args.str().c_str()))) {
        throw alvr::AvException("filter_in creation failed:", err);
    }

    if ((err = avfilter_graph_create_filter(
             &filter_out, avfilter_get_by_name("buffersink"), "out", NULL, NULL, filter_graph
         ))) {
        throw alvr::AvException("filter_out creation failed:", err);
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = filter_in;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = filter_out;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    // std::string filters = "scale_vaapi=out_range=full:format=";
    // if ((Settings::Instance().m_codec == ALVR_CODEC_HEVC
    //      || Settings::Instance().m_codec == ALVR_CODEC_AV1)
    //     && Settings::Instance().m_use10bitEncoder) {
    //     filters += "p010";
    // } else {
    //     filters += "nv12";
    // }
    // if ((err = avfilter_graph_parse_ptr(filter_graph, filters.c_str(), &inputs, &outputs, NULL))
    //     < 0) {
    //     throw alvr::AvException("avfilter_graph_parse_ptr failed:", err);
    // }

    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);

    for (unsigned i = 0; i < filter_graph->nb_filters; ++i) {
        filter_graph->filters[i]->hw_device_ctx = av_buffer_ref(hw_ctx);
    }

    if ((err = avfilter_graph_config(filter_graph, NULL))) {
        throw alvr::AvException("avfilter_graph_config failed:", err);
    }
}

alvr::EncodePipelineVulkan::~EncodePipelineVulkan() {
    // Commented because freeing it here causes a gpu reset, it should be cleaned up away
    // avcodec_free_context(&encoder_ctx);
    // avfilter_graph_free(&filter_graph);
    // av_frame_free(&mapped_frame);
    // av_frame_free(&encoder_frame);
    // av_buffer_unref(&hw_ctx);
    // av_buffer_unref(&drm_ctx);
}

void alvr::EncodePipelineVulkan::PushFrame(uint64_t targetTimestampNs, bool idr) {
    timestamp.cpu = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()
    )
                        .count();
    int err = av_buffersrc_add_frame_flags(
        filter_in, encoder_frame, AV_BUFFERSRC_FLAG_PUSH | AV_BUFFERSRC_FLAG_KEEP_REF
    );
    if (err != 0) {
        throw alvr::AvException("av_buffersrc_add_frame failed", err);
    }
    err = av_buffersink_get_frame(filter_out, encoder_frame);
    if (err != 0) {
        throw alvr::AvException("av_buffersink_get_frame failed", err);
    }

    encoder_frame->pict_type = idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    encoder_frame->pts = targetTimestampNs;

    if ((err = avcodec_send_frame(encoder_ctx, encoder_frame)) < 0) {
        throw alvr::AvException("avcodec_send_frame failed: ", err);
    }
    av_frame_unref(encoder_frame);
}

void alvr::EncodePipelineVulkan::SetParams(FfiDynamicEncoderParams params) {
    if (!params.updated) {
        return;
    }
    encoder_ctx->bit_rate = params.bitrate_bps;
    encoder_ctx->framerate = AVRational { int(params.framerate * 1000), 1000 };
    encoder_ctx->rc_buffer_size = encoder_ctx->bit_rate / params.framerate;
    encoder_ctx->rc_max_rate = encoder_ctx->bit_rate;
    encoder_ctx->rc_initial_buffer_occupancy = encoder_ctx->rc_buffer_size;

    if (Settings::Instance().m_amdBitrateCorruptionFix) {
        RequestIDR();
    }
}
