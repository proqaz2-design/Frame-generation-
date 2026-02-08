/**
 * FrameGen JNI Bridge — connects the C++ engine to the Android/Kotlin side.
 *
 * This is the main entry point for the native library.
 * All Java↔C++ communication goes through here.
 */

#include "framegen_types.h"
#include "vulkan/vulkan_layer.h"
#include "vulkan/vulkan_capture.h"
#include "vulkan/vulkan_compute.h"
#include "interpolation/rife_engine.h"
#include "interpolation/motion_estimator.h"
#include "interpolation/optical_flow.h"
#include "pipeline/frame_queue.h"
#include "pipeline/frame_presenter.h"
#include "pipeline/timing_controller.h"
#include "utils/perf_monitor.h"
#include "utils/shader_compiler.h"

#include <jni.h>
#include <android/native_window_jni.h>
#include <android/asset_manager_jni.h>
#include <memory>

using namespace framegen;

// ============================================================
// Global engine state
// ============================================================
struct EngineState {
    Config config;
    std::unique_ptr<VulkanCapture> capture;
    std::unique_ptr<VulkanCompute> compute;
    std::unique_ptr<RifeEngine> rife;
    std::unique_ptr<MotionEstimator> motionEstimator;
    std::unique_ptr<OpticalFlow> opticalFlow;
    std::unique_ptr<FramePresenter> presenter;
    std::unique_ptr<TimingController> timing;
    std::unique_ptr<PerfMonitor> perfMonitor;

    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;

    ANativeWindow* window = nullptr;
    AAssetManager* assetManager = nullptr;

    bool initialized = false;
    JavaVM* jvm = nullptr;
    jobject callbackObj = nullptr;
    jmethodID onStatsUpdateMethod = nullptr;
};

static EngineState g_engine;

// ============================================================
// Vulkan initialization helpers
// ============================================================
static bool initVulkanInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "FrameGen";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "FrameGen Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;

    return vkCreateInstance(&createInfo, nullptr, &g_engine.vkInstance) == VK_SUCCESS;
}

static bool selectPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(g_engine.vkInstance, &deviceCount, nullptr);
    if (deviceCount == 0) return false;

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(g_engine.vkInstance, &deviceCount, devices.data());

    // Prefer discrete GPU, fall back to integrated
    for (auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        LOGI("GPU: %s (Vulkan %u.%u.%u)",
             props.deviceName,
             VK_VERSION_MAJOR(props.apiVersion),
             VK_VERSION_MINOR(props.apiVersion),
             VK_VERSION_PATCH(props.apiVersion));

        g_engine.vkPhysicalDevice = device;
        return true;
    }

    return false;
}

static bool createLogicalDevice() {
    // Find queue families
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_engine.vkPhysicalDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_engine.vkPhysicalDevice, &queueFamilyCount, families.data());

    uint32_t graphicsFamily = UINT32_MAX;
    uint32_t computeFamily = UINT32_MAX;

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = i;
        }
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            computeFamily = i; // Prefer dedicated compute queue
        }
    }

    if (graphicsFamily == UINT32_MAX) return false;
    if (computeFamily == UINT32_MAX) computeFamily = graphicsFamily;

    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;

    VkDeviceQueueCreateInfo graphicsQueueInfo{};
    graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    graphicsQueueInfo.queueFamilyIndex = graphicsFamily;
    graphicsQueueInfo.queueCount = 1;
    graphicsQueueInfo.pQueuePriorities = &queuePriority;
    queueInfos.push_back(graphicsQueueInfo);

    if (computeFamily != graphicsFamily) {
        VkDeviceQueueCreateInfo computeQueueInfo{};
        computeQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        computeQueueInfo.queueFamilyIndex = computeFamily;
        computeQueueInfo.queueCount = 1;
        computeQueueInfo.pQueuePriorities = &queuePriority;
        queueInfos.push_back(computeQueueInfo);
    }

    const char* deviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;

    if (vkCreateDevice(g_engine.vkPhysicalDevice, &deviceInfo, nullptr, &g_engine.vkDevice) != VK_SUCCESS) {
        return false;
    }

    vkGetDeviceQueue(g_engine.vkDevice, graphicsFamily, 0, &g_engine.graphicsQueue);
    vkGetDeviceQueue(g_engine.vkDevice, computeFamily, 0, &g_engine.computeQueue);

    return true;
}

// ============================================================
// JNI Functions
// ============================================================
extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_engine.jvm = vm;
    LOGI("FrameGen native library loaded");
    return JNI_VERSION_1_6;
}

/**
 * Initialize the engine with a native window and configuration
 */
JNIEXPORT jboolean JNICALL
Java_com_framegen_app_engine_FrameGenEngine_nativeInit(
    JNIEnv* env, jobject thiz,
    jobject surface, jobject assetManager,
    jint mode, jfloat quality, jint targetFps)
{
    LOGI("=== FrameGen Engine Initializing ===");

    g_engine.assetManager = AAssetManager_fromJava(env, assetManager);
    g_engine.window = ANativeWindow_fromSurface(env, surface);

    if (!g_engine.window) {
        LOGE("Failed to get native window");
        return JNI_FALSE;
    }

    uint32_t width = static_cast<uint32_t>(ANativeWindow_getWidth(g_engine.window));
    uint32_t height = static_cast<uint32_t>(ANativeWindow_getHeight(g_engine.window));
    LOGI("Window: %ux%u", width, height);

    // Configure
    g_engine.config.mode = static_cast<Config::Mode>(mode);
    g_engine.config.quality = quality;
    g_engine.config.target_refresh_rate = targetFps;

    // Adjust time budget based on target FPS
    // For 120fps: budget = 8.33ms
    // For 60fps:  budget = 16.6ms
    g_engine.config.max_frame_time_ns = 1'000'000'000ULL / targetFps;

    // Step 1: Initialize Vulkan
    if (!initVulkanInstance()) {
        LOGE("Failed to create Vulkan instance");
        return JNI_FALSE;
    }

    if (!selectPhysicalDevice()) {
        LOGE("No suitable GPU found");
        return JNI_FALSE;
    }

    if (!createLogicalDevice()) {
        LOGE("Failed to create logical device");
        return JNI_FALSE;
    }

    // Step 2: Initialize compute pipeline
    g_engine.compute = std::make_unique<VulkanCompute>();
    if (!g_engine.compute->init(g_engine.vkDevice, g_engine.vkPhysicalDevice, 0)) {
        LOGE("Failed to init VulkanCompute");
        return JNI_FALSE;
    }

    // Step 3: Load compute shaders from assets
    auto loadShader = [&](const char* name, const char* asset) {
        auto spirv = ShaderCompiler::loadFromAsset(g_engine.assetManager, asset);
        if (!spirv.empty()) {
            g_engine.compute->loadShader(name, spirv.data(), spirv.size() * sizeof(uint32_t));
        }
    };

    loadShader("optical_flow", "shaders/optical_flow.spv");
    loadShader("frame_warp", "shaders/frame_warp.spv");
    loadShader("frame_blend", "shaders/frame_blend.spv");
    loadShader("downsample", "shaders/downsample.spv");
    loadShader("block_match", "shaders/block_match.spv");
    loadShader("flow_refine", "shaders/flow_refine.spv");
    loadShader("flow_consistency", "shaders/flow_consistency.spv");
    loadShader("rgb_to_gray", "shaders/rgb_to_gray.spv");

    // Step 4: Initialize frame capture
    g_engine.capture = std::make_unique<VulkanCapture>();
    if (!g_engine.capture->init(g_engine.vkDevice, g_engine.vkPhysicalDevice,
                                 0, width, height, VK_FORMAT_R8G8B8A8_UNORM)) {
        LOGE("Failed to init VulkanCapture");
        return JNI_FALSE;
    }

    // Step 5: Initialize RIFE interpolation engine
    g_engine.rife = std::make_unique<RifeEngine>();
    // Model files should be in app's files dir
    std::string modelDir = "/data/data/com.framegen.app/files/models";
    g_engine.rife->init(modelDir, g_engine.compute.get(), g_engine.config);

    // Step 6: Initialize motion estimator
    g_engine.motionEstimator = std::make_unique<MotionEstimator>();
    g_engine.motionEstimator->init(g_engine.compute.get(), width, height);

    // Step 7: Initialize optical flow
    g_engine.opticalFlow = std::make_unique<OpticalFlow>();
    g_engine.opticalFlow->init(g_engine.compute.get(), width, height);

    // Step 8: Initialize timing controller
    g_engine.timing = std::make_unique<TimingController>();
    g_engine.timing->init(g_engine.config);

    // Step 9: Initialize performance monitor
    g_engine.perfMonitor = std::make_unique<PerfMonitor>();
    g_engine.perfMonitor->init();

    // Step 10: Initialize frame presenter
    g_engine.presenter = std::make_unique<FramePresenter>();
    FramePresenter::InitParams presenterParams;
    presenterParams.capture = g_engine.capture.get();
    presenterParams.interpolator = g_engine.rife.get();
    presenterParams.device = g_engine.vkDevice;
    presenterParams.presentQueue = g_engine.graphicsQueue;
    presenterParams.width = width;
    presenterParams.height = height;
    presenterParams.config = g_engine.config;

    if (!g_engine.presenter->init(presenterParams)) {
        LOGE("Failed to init FramePresenter");
        return JNI_FALSE;
    }

    // Hook the Vulkan layer for frame capture
    VulkanLayer::instance().setFrameCaptureCallback(
        [](VkDevice device, VkQueue queue, VkImage srcImage,
           VkFormat format, uint32_t w, uint32_t h, uint64_t frameIndex) {
            if (g_engine.capture && g_engine.presenter) {
                auto frame = g_engine.capture->captureFrame(
                    queue, srcImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, frameIndex);
                g_engine.presenter->onFrameCaptured(frame);
            }
        }
    );

    g_engine.initialized = true;
    LOGI("=== FrameGen Engine Ready ===");
    return JNI_TRUE;
}

/**
 * Start frame generation
 */
JNIEXPORT void JNICALL
Java_com_framegen_app_engine_FrameGenEngine_nativeStart(JNIEnv* env, jobject thiz) {
    if (!g_engine.initialized) return;

    VulkanLayer::instance().setEnabled(true);
    g_engine.presenter->start();

    LOGI("FrameGen: Started");
}

/**
 * Stop frame generation
 */
JNIEXPORT void JNICALL
Java_com_framegen_app_engine_FrameGenEngine_nativeStop(JNIEnv* env, jobject thiz) {
    if (!g_engine.initialized) return;

    VulkanLayer::instance().setEnabled(false);
    g_engine.presenter->stop();

    LOGI("FrameGen: Stopped");
}

/**
 * Shutdown and cleanup
 */
JNIEXPORT void JNICALL
Java_com_framegen_app_engine_FrameGenEngine_nativeDestroy(JNIEnv* env, jobject thiz) {
    LOGI("FrameGen: Shutting down...");

    if (g_engine.presenter) g_engine.presenter->shutdown();
    if (g_engine.opticalFlow) g_engine.opticalFlow->shutdown();
    if (g_engine.motionEstimator) g_engine.motionEstimator->shutdown();
    if (g_engine.rife) g_engine.rife->shutdown();
    if (g_engine.capture) g_engine.capture->shutdown();
    if (g_engine.compute) g_engine.compute->shutdown();

    g_engine.presenter.reset();
    g_engine.opticalFlow.reset();
    g_engine.motionEstimator.reset();
    g_engine.rife.reset();
    g_engine.capture.reset();
    g_engine.compute.reset();
    g_engine.timing.reset();
    g_engine.perfMonitor.reset();

    if (g_engine.vkDevice != VK_NULL_HANDLE) {
        vkDestroyDevice(g_engine.vkDevice, nullptr);
    }
    if (g_engine.vkInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(g_engine.vkInstance, nullptr);
    }
    if (g_engine.window) {
        ANativeWindow_release(g_engine.window);
    }

    g_engine.initialized = false;
    LOGI("FrameGen: Shutdown complete");
}

/**
 * Set interpolation mode at runtime
 */
JNIEXPORT void JNICALL
Java_com_framegen_app_engine_FrameGenEngine_nativeSetMode(JNIEnv* env, jobject thiz, jint mode) {
    g_engine.config.mode = static_cast<Config::Mode>(mode);
    if (g_engine.presenter) {
        g_engine.presenter->setMode(g_engine.config.mode);
    }
}

/**
 * Set quality (0.0 - 1.0)
 */
JNIEXPORT void JNICALL
Java_com_framegen_app_engine_FrameGenEngine_nativeSetQuality(JNIEnv* env, jobject thiz, jfloat quality) {
    g_engine.config.quality = quality;
    if (g_engine.presenter) {
        g_engine.presenter->setQuality(quality);
    }
    if (g_engine.rife) {
        g_engine.rife->setQuality(quality);
    }
}

/**
 * Get performance stats as float array:
 * [0] = capture_ms, [1] = motion_ms, [2] = interp_ms, [3] = present_ms,
 * [4] = total_ms, [5] = effective_fps, [6] = gpu_temp, [7] = frames_generated,
 * [8] = frames_dropped
 */
JNIEXPORT jfloatArray JNICALL
Java_com_framegen_app_engine_FrameGenEngine_nativeGetStats(JNIEnv* env, jobject thiz) {
    jfloatArray result = env->NewFloatArray(9);
    if (!g_engine.presenter) return result;

    const PerfStats& stats = g_engine.presenter->getStats();
    float data[9] = {
        stats.capture_ms.load(),
        stats.motion_est_ms.load(),
        stats.interpolation_ms.load(),
        stats.present_ms.load(),
        stats.total_ms.load(),
        stats.effective_fps.load(),
        stats.gpu_temp_celsius.load(),
        static_cast<float>(stats.frames_generated.load()),
        static_cast<float>(stats.frames_dropped.load()),
    };

    env->SetFloatArrayRegion(result, 0, 9, data);
    return result;
}

/**
 * Get GPU temperature
 */
JNIEXPORT jfloat JNICALL
Java_com_framegen_app_engine_FrameGenEngine_nativeGetGpuTemp(JNIEnv* env, jobject thiz) {
    if (g_engine.timing) {
        return g_engine.timing->getGpuTemperature();
    }
    return 0.0f;
}

/**
 * Check if thermal throttling is active
 */
JNIEXPORT jboolean JNICALL
Java_com_framegen_app_engine_FrameGenEngine_nativeIsThermalThrottled(JNIEnv* env, jobject thiz) {
    if (g_engine.timing) {
        return g_engine.timing->isThermalThrottled() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

} // extern "C"
