package com.framegen.app.service

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.AccessibilityServiceInfo
import android.content.pm.ApplicationInfo
import android.util.Log
import android.view.accessibility.AccessibilityEvent

/**
 * GameDetectorService — Accessibility Service that detects game launches.
 *
 * Why Accessibility Service:
 * - Only reliable way to detect app switches WITHOUT root on modern Android
 * - getRunningTasks() is restricted since Android 5+
 * - UsageStats requires manual permission and is delayed
 * - Accessibility events are real-time and reliable
 *
 * The user needs to enable this in:
 *   Settings → Accessibility → FrameGen Game Detector → Enable
 *
 * Once enabled, this detects window changes and notifies FrameGenService
 * when a game is opened or closed.
 */
class GameDetectorService : AccessibilityService() {

    companion object {
        private const val TAG = "GameDetector"

        @Volatile
        var isServiceActive = false
            private set

        // Common game package prefixes/patterns
        private val GAME_PACKAGE_HINTS = listOf(
            "com.supercell",
            "com.miHoYo", "com.HoYoverse",
            "com.tencent.ig", "com.pubg",
            "com.activision", "com.garena",
            "com.epicgames", "com.innersloth",
            "com.kiloo", "com.imangi",
            "com.king.", "com.rovio",
            "com.mojang", "com.ea.",
            "com.gameloft", "com.netmarble",
            "com.nexon", "com.kabam",
            "com.square_enix", "com.bandainamco",
            "com.sega.", "com.capcom",
            "com.ubisoft", "com.riotgames",
            "jp.konami", "com.netease",
            "com.lilithgames", "com.yostar",
            "com.plarium", "com.scopely"
        )
    }

    private var currentTopPackage: String = ""
    private var currentGameActive = false

    override fun onServiceConnected() {
        super.onServiceConnected()
        Log.i(TAG, "Game Detector accessibility service connected")

        val info = AccessibilityServiceInfo().apply {
            eventTypes = AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED
            feedbackType = AccessibilityServiceInfo.FEEDBACK_GENERIC
            flags = AccessibilityServiceInfo.FLAG_INCLUDE_NOT_IMPORTANT_VIEWS
            notificationTimeout = 300 // ms debounce
        }
        serviceInfo = info
        isServiceActive = true
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {
        if (event?.eventType != AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED) return

        val packageName = event.packageName?.toString() ?: return

        // Ignore our own app, system UI, keyboards
        if (packageName == this.packageName ||
            packageName == "com.android.systemui" ||
            packageName == "com.android.launcher" ||
            packageName.startsWith("com.android.inputmethod") ||
            packageName.startsWith("com.google.android.inputmethod") ||
            packageName == "android") {
            return
        }

        // Same app, no change
        if (packageName == currentTopPackage) return
        currentTopPackage = packageName

        Log.d(TAG, "Window changed to: $packageName")

        val isGame = isGamePackage(packageName)

        if (isGame && !currentGameActive) {
            // Game launched!
            currentGameActive = true
            Log.i(TAG, "🎮 Game detected: $packageName")
            FrameGenService.notifyGameDetected(this, packageName)

        } else if (!isGame && currentGameActive) {
            // Left the game
            currentGameActive = false
            Log.i(TAG, "📱 Game closed (switched to $packageName)")
            FrameGenService.notifyGameClosed(this)
        }
    }

    override fun onInterrupt() {
        Log.w(TAG, "Game Detector service interrupted")
        isServiceActive = false
    }

    override fun onDestroy() {
        isServiceActive = false
        super.onDestroy()
    }

    /**
     * Determine if a package is a game using multiple heuristics:
     * 1. Android category == GAME
     * 2. Known game publisher package prefix
     * 3. Has game-related metadata (e.g., Unity, Unreal)
     * 4. User-added game list
     */
    private fun isGamePackage(packageName: String): Boolean {
        try {
            val appInfo = packageManager.getApplicationInfo(packageName, 0)

            // Category check (most reliable for Android 8+)
            if (appInfo.category == ApplicationInfo.CATEGORY_GAME) {
                return true
            }

            // Known game packages
            if (GAME_PACKAGE_HINTS.any { packageName.startsWith(it, ignoreCase = true) }) {
                return true
            }

            // Check for game engine markers in native libs
            val nativeLibDir = appInfo.nativeLibraryDir
            if (nativeLibDir != null) {
                val libDir = java.io.File(nativeLibDir)
                if (libDir.exists()) {
                    val libs = libDir.list() ?: emptyArray()
                    // Unity, Unreal, Cocos2d, etc.
                    if (libs.any {
                            it.contains("libunity", ignoreCase = true) ||
                            it.contains("libUE4", ignoreCase = true) ||
                            it.contains("libUnreal", ignoreCase = true) ||
                            it.contains("libcocos", ignoreCase = true) ||
                            it.contains("libgodot", ignoreCase = true)
                        }) {
                        return true
                    }
                }
            }

            // Check user's custom game list
            val prefs = getSharedPreferences("framegen_games", MODE_PRIVATE)
            if (prefs.getBoolean(packageName, false)) {
                return true
            }

        } catch (e: Exception) {
            Log.w(TAG, "Error checking package $packageName", e)
        }

        return false
    }
}
