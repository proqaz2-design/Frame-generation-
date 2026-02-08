/**
 * Vulkan Compute — GPU compute pipeline for image processing.
 *
 * Provides the Vulkan compute infrastructure needed by:
 * - Motion estimation (optical flow)
 * - Frame interpolation (warping)
 * - Post-processing (sharpening, color correction)
 */

#pragma once

#include "../framegen_types.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace framegen {

class VulkanCompute {
public:
    VulkanCompute() = default;
    ~VulkanCompute();

    bool init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t computeQueueFamilyIndex);
    void shutdown();

    // Load a SPIR-V compute shader
    bool loadShader(const std::string& name, const uint32_t* spirvCode, size_t codeSize);
    bool loadShaderFromFile(const std::string& name, const std::string& path);

    // Create a compute pipeline for a specific shader
    bool createPipeline(const std::string& shaderName,
                        const std::vector<VkDescriptorSetLayoutBinding>& bindings);

    // Dispatch compute work
    struct DispatchInfo {
        std::string pipelineName;
        uint32_t groupCountX;
        uint32_t groupCountY;
        uint32_t groupCountZ;
        std::vector<VkDescriptorSet> descriptorSets;
        void* pushConstants = nullptr;
        uint32_t pushConstantSize = 0;
    };

    VkCommandBuffer beginCompute();
    void dispatch(VkCommandBuffer cmd, const DispatchInfo& info);
    VkSemaphore endComputeAndSubmit(VkCommandBuffer cmd, VkSemaphore waitSemaphore = VK_NULL_HANDLE);

    // Resource creation helpers
    VkDescriptorSet allocateDescriptorSet(const std::string& pipelineName);
    void updateDescriptorImage(VkDescriptorSet set, uint32_t binding,
                               VkImageView imageView, VkSampler sampler,
                               VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    void updateDescriptorStorageImage(VkDescriptorSet set, uint32_t binding,
                                      VkImageView imageView);
    void updateDescriptorBuffer(VkDescriptorSet set, uint32_t binding,
                                VkBuffer buffer, VkDeviceSize size);

    VkDevice getDevice() const { return device_; }
    VkQueue getComputeQueue() const { return computeQueue_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;

    struct PipelineData {
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    };

    std::unordered_map<std::string, PipelineData> pipelines_;
    std::vector<VkSemaphore> semaphorePool_;
    uint32_t semaphoreIndex_ = 0;

    VkSemaphore getNextSemaphore();
};

} // namespace framegen
