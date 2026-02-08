cmake_minimum_required(VERSION 3.22.1)
project("framegen" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)

# ============================================================
# NCNN (Tencent) — lightweight neural network inference
# Pre-built from: https://github.com/Tencent/ncnn/releases
# Place ncnn-android-vulkan under app/src/main/cpp/third_party/
# ============================================================
set(NCNN_DIR ${CMAKE_SOURCE_DIR}/third_party/ncnn-android-vulkan/${ANDROID_ABI})

if(EXISTS "${NCNN_DIR}/lib/libncnn.a")
    add_library(ncnn STATIC IMPORTED)
    set_target_properties(ncnn PROPERTIES
        IMPORTED_LOCATION ${NCNN_DIR}/lib/libncnn.a
        INTERFACE_INCLUDE_DIRECTORIES ${NCNN_DIR}/include/ncnn
    )
    set(NCNN_FOUND TRUE)
    message(STATUS "NCNN found at ${NCNN_DIR}")
else()
    set(NCNN_FOUND FALSE)
    message(WARNING "NCNN not found — building stub mode. Download ncnn-android-vulkan and place in third_party/")
endif()

# ============================================================
# Vulkan from NDK
# ============================================================
find_library(VULKAN_LIB vulkan)
find_library(LOG_LIB log)
find_library(ANDROID_LIB android)
find_library(JNIGRAPHICS_LIB jnigraphics)

# ============================================================
# Source files
# ============================================================
set(CORE_SOURCES
    # Entry point
    framegen_jni.cpp

    # Vulkan capture & compute (for JNI engine, NOT the layer)
    vulkan/vulkan_capture.cpp
    vulkan/vulkan_compute.cpp

    # Frame interpolation
    interpolation/rife_engine.cpp
    interpolation/motion_estimator.cpp
    interpolation/optical_flow.cpp

    # Frame management
    pipeline/frame_queue.cpp
    pipeline/frame_presenter.cpp
    pipeline/timing_controller.cpp

    # Utilities
    utils/gpu_buffer.cpp
    utils/shader_compiler.cpp
    utils/perf_monitor.cpp
)

add_library(framegen SHARED ${CORE_SOURCES})

target_include_directories(framegen PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/vulkan
    ${CMAKE_SOURCE_DIR}/interpolation
    ${CMAKE_SOURCE_DIR}/pipeline
    ${CMAKE_SOURCE_DIR}/utils
)

target_link_libraries(framegen
    ${VULKAN_LIB}
    ${LOG_LIB}
    ${ANDROID_LIB}
    ${JNIGRAPHICS_LIB}
)

if(NCNN_FOUND)
    target_link_libraries(framegen ncnn)
    target_compile_definitions(framegen PRIVATE NCNN_ENABLED=1)
else()
    target_compile_definitions(framegen PRIVATE NCNN_ENABLED=0)
endif()

# ============================================================
# Vulkan Layer — SELF-CONTAINED frame generation
# This .so is loaded into game processes via gpu_debug_layers.
# It must be fully standalone (no deps on libframegen.so).
# ============================================================
set(LAYER_SOURCES
    vulkan/vulkan_layer.cpp
)

add_library(VkLayer_framegen SHARED ${LAYER_SOURCES})

target_include_directories(VkLayer_framegen PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/vulkan
)

target_link_libraries(VkLayer_framegen
    ${VULKAN_LIB}
    ${LOG_LIB}
)

target_compile_definitions(VkLayer_framegen PRIVATE
    LAYER_NAME="VK_LAYER_FRAMEGEN_capture"
)

# Ensure the layer exports are visible
set_target_properties(VkLayer_framegen PROPERTIES
    CXX_VISIBILITY_PRESET default
    C_VISIBILITY_PRESET default
)

# ============================================================
# Compute shaders (SPIR-V) — compiled at build time
# ============================================================
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SPIRV_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SPIRV_DIR})

# If glslangValidator is available, compile shaders
find_program(GLSLANG glslangValidator)
if(GLSLANG)
    file(GLOB COMP_SHADERS "${SHADER_DIR}/*.comp")
    foreach(SHADER ${COMP_SHADERS})
        get_filename_component(SHADER_NAME ${SHADER} NAME_WE)
        set(SPIRV_OUTPUT "${SPIRV_DIR}/${SHADER_NAME}.spv")
        add_custom_command(
            OUTPUT ${SPIRV_OUTPUT}
            COMMAND ${GLSLANG} -V ${SHADER} -o ${SPIRV_OUTPUT}
            DEPENDS ${SHADER}
            COMMENT "Compiling ${SHADER_NAME}.comp -> SPIR-V"
        )
        list(APPEND SPIRV_BINARIES ${SPIRV_OUTPUT})
    endforeach()
    add_custom_target(compile_shaders ALL DEPENDS ${SPIRV_BINARIES})
    add_dependencies(framegen compile_shaders)
endif()
