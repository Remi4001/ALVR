#pragma once

#include "EncodePipeline.h"
#include "ffmpeg_helper.h"

extern "C" struct AVBufferRef;
extern "C" struct AVCodecContext;
extern "C" struct AVFilterContext;
extern "C" struct AVFilterGraph;
extern "C" struct AVFrame;

class Renderer;

namespace alvr {

// #define PRESET_MODE_SPEED (0)
// #define PRESET_MODE_BALANCE (1)
// #define PRESET_MODE_QUALITY (2)

// enum EncoderQualityPreset { QUALITY = 0, BALANCED = 1, SPEED = 2 };

class EncodePipelineVulkan : public EncodePipeline {
public:
    ~EncodePipelineVulkan();
    EncodePipelineVulkan(
        alvr::HWContext& vk_ctx,
        alvr::Vendor vendor,
        alvr::VkFrame& input_frame,
        uint32_t width,
        uint32_t height
    );

    // TODO: Don't pass the timestamp here we're literally just passing it through (or does it help
    // with dumped videos?)
    void PushFrame(uint64_t targetTimestampNs, bool idr) override;
    void SetParams(FfiDynamicEncoderParams params) override;

private:
    AVBufferRef* hw_ctx = nullptr;
    std::unique_ptr<alvr::VkFrameCtx> vk_frame_ctx;
    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> vk_frame;
};
}
