package com.framegen.app.engine

import android.content.res.AssetManager
import android.view.Surface

/**
 * FrameGenEngine — Kotlin wrapper for the native C++ engine.
 *
 * Provides the JNI bridge to the Vulkan-based frame interpolation pipeline.
 */
class FrameGenEngine {

    companion object {
        init {
            System.loadLibrary("framegen")
        }

        const val MODE_OFF = 0
        const val MODE_60FPS = 1
        const val MODE_90FPS = 2
        const val MODE_120FPS = 3
    }

    data class Stats(
        val captureMs: Float = 0f,
        val motionMs: Float = 0f,
        val interpolationMs: Float = 0f,
        val presentMs: Float = 0f,
        val totalMs: Float = 0f,
        val effectiveFps: Float = 0f,
        val gpuTemp: Float = 0f,
        val framesGenerated: Long = 0,
        val framesDropped: Long = 0
    )

    var isRunning = false
        private set

    /**
     * Initialize the engine.
     *
     * @param surface The game's rendering surface
     * @param assetManager For loading shaders and models
     * @param mode Interpolation mode (MODE_OFF, MODE_60FPS, etc.)
     * @param quality Quality factor 0.0-1.0
     * @param targetFps Target output FPS (60, 90, or 120)
     */
    fun init(
        surface: Surface,
        assetManager: AssetManager,
        mode: Int = MODE_60FPS,
        quality: Float = 0.5f,
        targetFps: Int = 120
    ): Boolean {
        return nativeInit(surface, assetManager, mode, quality, targetFps)
    }

    fun start() {
        nativeStart()
        isRunning = true
    }

    fun stop() {
        nativeStop()
        isRunning = false
    }

    fun destroy() {
        stop()
        nativeDestroy()
    }

    fun setMode(mode: Int) {
        nativeSetMode(mode)
    }

    fun setQuality(quality: Float) {
        nativeSetQuality(quality.coerceIn(0f, 1f))
    }

    fun getStats(): Stats {
        val raw = nativeGetStats() ?: return Stats()
        return Stats(
            captureMs = raw[0],
            motionMs = raw[1],
            interpolationMs = raw[2],
            presentMs = raw[3],
            totalMs = raw[4],
            effectiveFps = raw[5],
            gpuTemp = raw[6],
            framesGenerated = raw[7].toLong(),
            framesDropped = raw[8].toLong()
        )
    }

    fun getGpuTemperature(): Float = nativeGetGpuTemp()

    fun isThermalThrottled(): Boolean = nativeIsThermalThrottled()

    // ============================================================
    // Native methods
    // ============================================================
    private external fun nativeInit(
        surface: Surface,
        assetManager: AssetManager,
        mode: Int,
        quality: Float,
        targetFps: Int
    ): Boolean

    private external fun nativeStart()
    private external fun nativeStop()
    private external fun nativeDestroy()
    private external fun nativeSetMode(mode: Int)
    private external fun nativeSetQuality(quality: Float)
    private external fun nativeGetStats(): FloatArray?
    private external fun nativeGetGpuTemp(): Float
    private external fun nativeIsThermalThrottled(): Boolean
}
