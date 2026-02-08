/**
 * Shader Compiler — loads SPIR-V shaders from Android assets
 */

#pragma once

#include "../framegen_types.h"
#include <vector>
#include <string>

struct AAssetManager;

namespace framegen {

class ShaderCompiler {
public:
    /**
     * Load SPIR-V binary from Android assets.
     * @param assetManager AAssetManager from Java side
     * @param assetPath Path within assets/ (e.g. "shaders/optical_flow.spv")
     * @return SPIR-V bytecode, empty on failure
     */
    static std::vector<uint32_t> loadFromAsset(AAssetManager* assetManager,
                                                const std::string& assetPath);

    /**
     * Load SPIR-V from filesystem path.
     */
    static std::vector<uint32_t> loadFromFile(const std::string& path);

    /**
     * Create VkShaderModule from SPIR-V code.
     */
    static VkShaderModule createModule(VkDevice device,
                                       const std::vector<uint32_t>& spirv);
};

} // namespace framegen
