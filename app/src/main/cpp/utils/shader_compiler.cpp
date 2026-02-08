/**
 * Shader Compiler implementation
 */

#include "shader_compiler.h"
#include <android/asset_manager.h>
#include <fstream>

namespace framegen {

std::vector<uint32_t> ShaderCompiler::loadFromAsset(AAssetManager* assetManager,
                                                     const std::string& assetPath) {
    if (!assetManager) {
        LOGE("ShaderCompiler: Null asset manager");
        return {};
    }

    AAsset* asset = AAssetManager_open(assetManager, assetPath.c_str(), AASSET_MODE_BUFFER);
    if (!asset) {
        LOGE("ShaderCompiler: Could not open asset: %s", assetPath.c_str());
        return {};
    }

    size_t size = AAsset_getLength(asset);
    if (size == 0 || size % 4 != 0) {
        LOGE("ShaderCompiler: Invalid SPIR-V size: %zu", size);
        AAsset_close(asset);
        return {};
    }

    std::vector<uint32_t> spirv(size / sizeof(uint32_t));
    AAsset_read(asset, spirv.data(), size);
    AAsset_close(asset);

    // Validate SPIR-V magic number
    if (spirv[0] != 0x07230203) {
        LOGE("ShaderCompiler: Invalid SPIR-V magic number in %s", assetPath.c_str());
        return {};
    }

    LOGI("ShaderCompiler: Loaded %s (%zu bytes)", assetPath.c_str(), size);
    return spirv;
}

std::vector<uint32_t> ShaderCompiler::loadFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        LOGE("ShaderCompiler: Could not open: %s", path.c_str());
        return {};
    }

    size_t size = static_cast<size_t>(file.tellg());
    if (size == 0 || size % 4 != 0) {
        LOGE("ShaderCompiler: Invalid size: %zu", size);
        return {};
    }

    std::vector<uint32_t> spirv(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(spirv.data()), size);

    if (spirv[0] != 0x07230203) {
        LOGE("ShaderCompiler: Invalid SPIR-V magic");
        return {};
    }

    return spirv;
}

VkShaderModule ShaderCompiler::createModule(VkDevice device,
                                             const std::vector<uint32_t>& spirv) {
    if (spirv.empty()) return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirv.size() * sizeof(uint32_t);
    createInfo.pCode = spirv.data();

    VkShaderModule module;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        LOGE("ShaderCompiler: Failed to create shader module");
        return VK_NULL_HANDLE;
    }

    return module;
}

} // namespace framegen
