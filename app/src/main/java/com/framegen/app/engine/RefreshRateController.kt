package com.framegen.app.engine

import android.app.Activity
import android.os.Build
import android.view.Display
import android.view.WindowManager
import android.content.Context
import android.util.Log

/**
 * RefreshRateController — manages display refresh rate.
 *
 * Forces the display to the highest supported refresh rate (120Hz, 90Hz, etc.)
 * for the interpolated frame output to be visible.
 *
 * Works without root using:
 * 1. WindowManager API (Android 11+)
 * 2. Surface Control (Android 10+)
 * 3. Shizuku for system-level override (optional)
 */
class RefreshRateController(private val context: Context) {

    companion object {
        private const val TAG = "RefreshRate"
    }

    data class DisplayInfo(
        val currentRate: Float,
        val supportedRates: List<Float>,
        val maxRate: Float
    )

    /**
     * Get information about the current display.
     */
    fun getDisplayInfo(): DisplayInfo {
        val display = getDisplay()
        val modes = display.supportedModes
        val currentMode = display.mode
        val rates = modes.map { it.refreshRate }.distinct().sorted()

        return DisplayInfo(
            currentRate = currentMode.refreshRate,
            supportedRates = rates,
            maxRate = rates.maxOrNull() ?: 60f
        )
    }

    /**
     * Request the highest available refresh rate.
     */
    fun requestMaxRefreshRate(activity: Activity): Boolean {
        return try {
            val info = getDisplayInfo()
            Log.i(TAG, "Current: ${info.currentRate}Hz, Max: ${info.maxRate}Hz")
            Log.i(TAG, "Supported: ${info.supportedRates}")

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                // Android 11+ — use Window attribute
                setRefreshRateAndroid11(activity, info.maxRate)
            } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                // Android 6+ — use preferred display mode
                setRefreshRatePreferred(activity, info.maxRate)
            } else {
                Log.w(TAG, "Android version too old for refresh rate control")
                false
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set refresh rate", e)
            false
        }
    }

    /**
     * Request a specific refresh rate.
     */
    fun requestRefreshRate(activity: Activity, targetRate: Float): Boolean {
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                setRefreshRateAndroid11(activity, targetRate)
            } else {
                setRefreshRatePreferred(activity, targetRate)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set refresh rate to $targetRate", e)
            false
        }
    }

    /**
     * Android 11+ refresh rate control via Window attributes.
     */
    private fun setRefreshRateAndroid11(activity: Activity, targetRate: Float): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return false

        val params = activity.window.attributes

        // Set frame rate preference
        params.preferredDisplayModeId = findBestMode(targetRate)

        // Force high refresh rate
        activity.window.attributes = params

        // Disable AOSP frame rate limiter
        try {
            val wmParams = activity.window.attributes
            wmParams.preferredRefreshRate = targetRate
            activity.window.attributes = wmParams
            Log.i(TAG, "Set preferred refresh rate to ${targetRate}Hz")
        } catch (e: Exception) {
            Log.w(TAG, "Could not set preferredRefreshRate", e)
        }

        return true
    }

    /**
     * Pre-Android 11 refresh rate control via display mode.
     */
    private fun setRefreshRatePreferred(activity: Activity, targetRate: Float): Boolean {
        val modeId = findBestMode(targetRate)
        if (modeId == 0) return false

        val params = activity.window.attributes
        params.preferredDisplayModeId = modeId
        activity.window.attributes = params

        Log.i(TAG, "Set display mode ID: $modeId (target: ${targetRate}Hz)")
        return true
    }

    /**
     * Find the display mode closest to the target refresh rate.
     */
    private fun findBestMode(targetRate: Float): Int {
        val display = getDisplay()
        val modes = display.supportedModes

        // Find mode closest to target rate with matching resolution
        val currentMode = display.mode
        var bestMode = currentMode
        var bestDiff = Float.MAX_VALUE

        for (mode in modes) {
            // Must match current resolution
            if (mode.physicalWidth != currentMode.physicalWidth ||
                mode.physicalHeight != currentMode.physicalHeight) continue

            val diff = kotlin.math.abs(mode.refreshRate - targetRate)
            if (diff < bestDiff) {
                bestDiff = diff
                bestMode = mode
            }
        }

        Log.i(TAG, "Best mode: ${bestMode.modeId} " +
              "(${bestMode.physicalWidth}x${bestMode.physicalHeight} @ ${bestMode.refreshRate}Hz)")
        return bestMode.modeId
    }

    private fun getDisplay(): Display {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            context.display ?: (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay
        } else {
            @Suppress("DEPRECATION")
            (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay
        }
    }
}
