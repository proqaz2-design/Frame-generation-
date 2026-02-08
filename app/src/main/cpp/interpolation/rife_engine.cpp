/**
 * RIFE Engine implementation — NCNN inference + GPU fallback
 */

#include "rife_engine.h"
#include "motion_estimator.h"
#include <algorithm>

namespace framegen {

RifeEngine::~RifeEngine() {
    shutdown();
}

bool RifeEngine::init(const std::string& modelDir, VulkanCompute* compute, const Config& config) {
    compute_ = compute;
    config_ = config;

    // Create linear sampler for texture sampling
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(compute_->getDevice(), &samplerInfo, nullptr, &linearSampler_) != VK_SUCCESS) {
        LOGE("RifeEngine: Failed to create sampler");
        return false;
    }

#if NCNN_ENABLED
    if (initNCNN(modelDir)) {
        modelLoaded_ = true;
        LOGI("RifeEngine: NCNN RIFE model loaded successfully");
        return true;
    }
    LOGW("RifeEngine: NCNN init failed, falling back to GPU compute");
#endif

    // Fallback to compute shader-based interpolation
    if (initFallback()) {
        LOGI("RifeEngine: GPU compute fallback initialized");
        return true;
    }

    LOGE("RifeEngine: All initialization methods failed");
    return false;
}

void RifeEngine::shutdown() {
    if (compute_ && linearSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(compute_->getDevice(), linearSampler_, nullptr);
        linearSampler_ = VK_NULL_HANDLE;
    }

    for (auto& buf : scaledBuffers_) {
        if (buf.view != VK_NULL_HANDLE)
            vkDestroyImageView(compute_->getDevice(), buf.view, nullptr);
        if (buf.image != VK_NULL_HANDLE)
            vkDestroyImage(compute_->getDevice(), buf.image, nullptr);
        if (buf.memory != VK_NULL_HANDLE)
            vkFreeMemory(compute_->getDevice(), buf.memory, nullptr);
    }
    scaledBuffers_.clear();

#if NCNN_ENABLED
    rifeNet_.clear();
#endif

    modelLoaded_ = false;
}

#if NCNN_ENABLED
bool RifeEngine::initNCNN(const std::string& modelDir) {
    // Initialize NCNN with Vulkan compute
    ncnn::create_gpu_instance();

    int gpuCount = ncnn::get_gpu_count();
    if (gpuCount <= 0) {
        LOGW("RifeEngine: No GPU available for NCNN");
        return false;
    }

    ncnnVkDevice_ = ncnn::get_gpu_device(0);
    if (!ncnnVkDevice_) {
        LOGW("RifeEngine: Failed to get NCNN Vulkan device");
        return false;
    }

    rifeNet_.opt.use_vulkan_compute = true;
    rifeNet_.opt.use_fp16_packed = true;
    rifeNet_.opt.use_fp16_storage = true;
    rifeNet_.opt.use_fp16_arithmetic = true;
    rifeNet_.opt.use_packing_layout = true;
    rifeNet_.set_vulkan_device(ncnnVkDevice_);

    // Load RIFE model — expecting rife-v4.6-lite for mobile
    std::string paramPath = modelDir + "/rife-v4.6-lite.param";
    std::string binPath = modelDir + "/rife-v4.6-lite.bin";

    if (rifeNet_.load_param(paramPath.c_str()) != 0) {
        LOGW("RifeEngine: Failed to load param: %s", paramPath.c_str());
        return false;
    }

    if (rifeNet_.load_model(binPath.c_str()) != 0) {
        LOGW("RifeEngine: Failed to load model: %s", binPath.c_str());
        return false;
    }

    LOGI("RifeEngine: RIFE v4.6 lite model loaded (FP16, Vulkan)");
    return true;
}

bool RifeEngine::runNCNNInference(const FrameData& frame1, const FrameData& frame2,
                                   float timestep, FrameData& output) {
    auto startTime = Clock::now();

    uint32_t w = static_cast<uint32_t>(frame1.width * config_.model_scale);
    uint32_t h = static_cast<uint32_t>(frame1.height * config_.model_scale);

    // Pad to multiple of 32 (RIFE requirement)
    uint32_t paddedW = (w + 31) / 32 * 32;
    uint32_t paddedH = (h + 31) / 32 * 32;

    // Create NCNN Mat from Vulkan images
    // In production, we'd use ncnn::VkMat for zero-copy GPU<->NCNN transfer
    ncnn::Mat in0(paddedW, paddedH, 3);
    ncnn::Mat in1(paddedW, paddedH, 3);
    ncnn::Mat timestepMat(1);
    timestepMat[0] = timestep;

    // Run inference
    ncnn::Extractor ex = rifeNet_.create_extractor();
    ex.set_vulkan_compute(true);

    ex.input("input0", in0);
    ex.input("input1", in1);
    ex.input("timestep", timestepMat);

    ncnn::Mat outMat;
    ex.extract("output", outMat);

    // Convert output back to VkImage
    // The actual pixel copy would go through compute shaders
    // to avoid expensive GPU<->CPU transfers

    auto endTime = Clock::now();
    lastInferenceMs_ = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    LOGD("RifeEngine: NCNN inference: %.2f ms (budget: %.2f ms)",
         lastInferenceMs_, ns_to_ms(config_.max_frame_time_ns));

    return lastInferenceMs_ < ns_to_ms(config_.max_frame_time_ns);
}
#endif

// ============================================================
// Fallback: GPU compute shader interpolation
// ============================================================
bool RifeEngine::initFallback() {
    if (!compute_) return false;

    // We use three compute shaders:
    // 1. optical_flow.comp   — motion estimation between two frames
    // 2. frame_warp.comp     — warp frame using motion vectors
    // 3. frame_blend.comp    — blend warped frames for final output

    // These would be loaded from SPIR-V at runtime
    // For now, we set up the pipeline structure
    
    // Optical flow pipeline bindings
    std::vector<VkDescriptorSetLayoutBinding> flowBindings = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},  // flow output
    };

    // Frame warp pipeline bindings
    std::vector<VkDescriptorSetLayoutBinding> warpBindings = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},  // flow
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},  // output
    };

    // Blend pipeline bindings
    std::vector<VkDescriptorSetLayoutBinding> blendBindings = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    LOGI("RifeEngine: Fallback pipeline structure ready");
    LOGI("RifeEngine: Load SPIR-V shaders from assets to complete initialization");
    return true;
}

bool RifeEngine::runFallbackInterpolation(const FrameData& frame1, const FrameData& frame2,
                                            float timestep, FrameData& output) {
    auto startTime = Clock::now();

    // Step 1: Compute optical flow (motion vectors) between frame1 and frame2
    // This uses a multi-scale Lucas-Kanade approach on GPU
    VkCommandBuffer cmd = compute_->beginCompute();

    // Dispatch optical flow
    VulkanCompute::DispatchInfo flowInfo;
    flowInfo.pipelineName = "optical_flow";
    flowInfo.groupCountX = (frame1.width + 15) / 16;
    flowInfo.groupCountY = (frame1.height + 15) / 16;
    flowInfo.groupCountZ = 1;

    struct FlowPushConstants {
        float timestep;
        uint32_t width;
        uint32_t height;
        uint32_t blockSize;
    } flowPC = {timestep, frame1.width, frame1.height, 16};

    flowInfo.pushConstants = &flowPC;
    flowInfo.pushConstantSize = sizeof(flowPC);

    compute_->dispatch(cmd, flowInfo);

    // Memory barrier between flow and warp
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);

    // Step 2: Warp both frames towards timestep
    VulkanCompute::DispatchInfo warpInfo;
    warpInfo.pipelineName = "frame_warp";
    warpInfo.groupCountX = (frame1.width + 15) / 16;
    warpInfo.groupCountY = (frame1.height + 15) / 16;
    warpInfo.groupCountZ = 1;
    compute_->dispatch(cmd, warpInfo);

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);

    // Step 3: Blend warped frames
    VulkanCompute::DispatchInfo blendInfo;
    blendInfo.pipelineName = "frame_blend";
    blendInfo.groupCountX = (frame1.width + 15) / 16;
    blendInfo.groupCountY = (frame1.height + 15) / 16;
    blendInfo.groupCountZ = 1;

    struct BlendPushConstants {
        float blendFactor;
        uint32_t width;
        uint32_t height;
        float pad;
    } blendPC = {timestep, frame1.width, frame1.height, 0.0f};

    blendInfo.pushConstants = &blendPC;
    blendInfo.pushConstantSize = sizeof(blendPC);
    compute_->dispatch(cmd, blendInfo);

    // Submit all and signal semaphore
    VkSemaphore doneSem = compute_->endComputeAndSubmit(cmd, frame2.render_complete);

    output.render_complete = doneSem;
    output.is_interpolated = true;
    output.timestamp_ns = (frame1.timestamp_ns + frame2.timestamp_ns) / 2;

    auto endTime = Clock::now();
    lastInferenceMs_ = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    return lastInferenceMs_ < ns_to_ms(config_.max_frame_time_ns);
}

bool RifeEngine::interpolate(const FrameData& frame1, const FrameData& frame2,
                              float timestep, FrameData& output) {
#if NCNN_ENABLED
    if (modelLoaded_) {
        return runNCNNInference(frame1, frame2, timestep, output);
    }
#endif
    return runFallbackInterpolation(frame1, frame2, timestep, output);
}

bool RifeEngine::interpolateMulti(const FrameData& frame1, const FrameData& frame2,
                                   uint32_t count, std::vector<FrameData>& outputs) {
    outputs.resize(count);

    for (uint32_t i = 0; i < count; i++) {
        float t = static_cast<float>(i + 1) / static_cast<float>(count + 1);
        if (!interpolate(frame1, frame2, t, outputs[i])) {
            LOGW("RifeEngine: Interpolation %u/%u exceeded time budget", i + 1, count);
            // Truncate — return what we have
            outputs.resize(i);
            return i > 0;
        }
    }

    return true;
}

void RifeEngine::setQuality(float quality) {
    config_.quality = std::max(0.0f, std::min(1.0f, quality));

    // Adjust model scale based on quality
    if (config_.quality < 0.3f) {
        config_.model_scale = 0.25f;  // Quarter res — fastest
    } else if (config_.quality < 0.6f) {
        config_.model_scale = 0.5f;   // Half res — balanced
    } else {
        config_.model_scale = 0.75f;  // 3/4 res — high quality
    }

    LOGI("RifeEngine: Quality=%.2f, ModelScale=%.2f", config_.quality, config_.model_scale);
}

void RifeEngine::setModelScale(float scale) {
    config_.model_scale = std::max(0.25f, std::min(1.0f, scale));
}

} // namespace framegen
