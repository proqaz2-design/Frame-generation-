package com.framegen.app.engine

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.util.Log

/**
 * GameLauncher — discovers installed games and launches them
 * within the FrameGen sandbox environment.
 *
 * The "sandbox" works by:
 * 1. Setting VK_LAYER_PATH to include our layer library
 * 2. Launching the game process with the layer enabled
 * 3. The Vulkan layer intercepts frame presentation
 */
class GameLauncher(private val context: Context) {

    companion object {
        private const val TAG = "GameLauncher"
    }

    data class GameInfo(
        val packageName: String,
        val appName: String,
        val icon: android.graphics.drawable.Drawable?,
        val isVulkanGame: Boolean
    )

    /**
     * Get list of installed games (apps with game category or known game packages).
     */
    fun getInstalledGames(): List<GameInfo> {
        val pm = context.packageManager
        val games = mutableListOf<GameInfo>()

        val packages = pm.getInstalledApplications(PackageManager.GET_META_DATA)

        for (app in packages) {
            // Check if it's a game
            val isGame = (app.category == android.content.pm.ApplicationInfo.CATEGORY_GAME) ||
                         isKnownGamePackage(app.packageName) ||
                         hasGameMetadata(app)

            if (isGame) {
                val appName = pm.getApplicationLabel(app).toString()
                val icon = try { pm.getApplicationIcon(app.packageName) } catch (e: Exception) { null }

                // Check if the game uses Vulkan
                val isVulkan = checkVulkanSupport(app.packageName)

                games.add(GameInfo(
                    packageName = app.packageName,
                    appName = appName,
                    icon = icon,
                    isVulkanGame = isVulkan
                ))
            }
        }

        Log.i(TAG, "Found ${games.size} games (${games.count { it.isVulkanGame }} Vulkan)")
        return games.sortedBy { it.appName }
    }

    /**
     * Launch a game with the FrameGen Vulkan layer enabled.
     */
    fun launchGame(packageName: String): Boolean {
        return try {
            val pm = context.packageManager
            val launchIntent = pm.getLaunchIntentForPackage(packageName)

            if (launchIntent == null) {
                Log.e(TAG, "No launch intent for $packageName")
                return false
            }

            // Set environment for Vulkan layer injection
            // On non-rooted devices, we use the debug GPU layer mechanism
            setupVulkanLayer(packageName)

            launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(launchIntent)

            Log.i(TAG, "Launched: $packageName with FrameGen layer")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Failed to launch $packageName", e)
            false
        }
    }

    /**
     * Setup the Vulkan layer for a game.
     *
     * Android supports GPU debug layers via:
     * - Developer options -> Enable GPU debug layers
     * - adb shell settings put global enable_gpu_debug_layers 1
     * - adb shell settings put global gpu_debug_app <package>
     * - adb shell settings put global gpu_debug_layers VK_LAYER_FRAMEGEN_capture
     * - adb shell settings put global gpu_debug_layer_app com.framegen.app
     *
     * With Shizuku, we can set these without physical ADB connection.
     */
    private fun setupVulkanLayer(targetPackage: String) {
        try {
            // Try Shizuku first (programmatic ADB)
            if (setupViaShizuku(targetPackage)) {
                Log.i(TAG, "Vulkan layer configured via Shizuku")
                return
            }

            // Fallback: guide user to enable manually
            Log.w(TAG, "Shizuku not available — Vulkan layer must be enabled via ADB:")
            Log.w(TAG, "  adb shell settings put global enable_gpu_debug_layers 1")
            Log.w(TAG, "  adb shell settings put global gpu_debug_app $targetPackage")
            Log.w(TAG, "  adb shell settings put global gpu_debug_layers VK_LAYER_FRAMEGEN_capture")
            Log.w(TAG, "  adb shell settings put global gpu_debug_layer_app com.framegen.app")

        } catch (e: Exception) {
            Log.e(TAG, "Failed to setup Vulkan layer", e)
        }
    }

    /**
     * Setup Vulkan layer using Shizuku (ADB over app).
     */
    fun injectVulkanLayerViaShizuku(targetPackage: String): Boolean {
        return setupViaShizuku(targetPackage)
    }

    private fun setupViaShizuku(targetPackage: String): Boolean {
        return try {
            // Check if Shizuku is available and authorized
            if (!rikka.shizuku.Shizuku.pingBinder()) {
                Log.w(TAG, "Shizuku not running")
                return false
            }

            if (rikka.shizuku.Shizuku.checkSelfPermission() != PackageManager.PERMISSION_GRANTED) {
                Log.w(TAG, "Shizuku permission not granted")
                return false
            }

            // Execute settings commands via Shizuku
            val commands = arrayOf(
                "settings put global enable_gpu_debug_layers 1",
                "settings put global gpu_debug_app $targetPackage",
                "settings put global gpu_debug_layers VK_LAYER_FRAMEGEN_capture",
                "settings put global gpu_debug_layer_app ${context.packageName}"
            )

            for (cmd in commands) {
                val process = Runtime.getRuntime().exec(arrayOf("sh", "-c", cmd))
                process.waitFor()
                Log.d(TAG, "Shizuku: $cmd -> ${process.exitValue()}")
            }

            true
        } catch (e: Exception) {
            Log.w(TAG, "Shizuku setup failed", e)
            false
        }
    }

    /**
     * Check if a package likely uses Vulkan by inspecting its native libraries.
     */
    private fun checkVulkanSupport(packageName: String): Boolean {
        return try {
            val pm = context.packageManager
            val appInfo = pm.getApplicationInfo(packageName, 0)
            val nativeLibDir = appInfo.nativeLibraryDir ?: return false

            val libDir = java.io.File(nativeLibDir)
            if (!libDir.exists()) return false

            // Check for Vulkan-related libraries
            libDir.listFiles()?.any { file ->
                file.name.contains("vulkan", ignoreCase = true) ||
                file.name.contains("libVk", ignoreCase = true) ||
                file.name.contains("UnityEngine", ignoreCase = true) ||
                file.name.contains("libUE4", ignoreCase = true) ||
                file.name.contains("libunreal", ignoreCase = true)
            } ?: false
        } catch (e: Exception) {
            false
        }
    }

    private fun isKnownGamePackage(packageName: String): Boolean {
        return isKnownGame(packageName)
    }

    /**
     * Check if a package is from a known game publisher.
     */
    fun isKnownGame(packageName: String): Boolean {
        val gamePatterns = listOf(
            // Windows emulators
            "com.winlator", "com.pairip.winlator",
            "br.pucrio.winlator", "com.niceplayer.winlator",
            "com.eltechs", "com.mobox",
            // GameHub variants
            "com.gamehub", "com.gamehub.app",
            "com.gamehub.lite", "com.gamehub.brazil",
            "br.com.gamehub", "com.gamehublite",
            "com.nextstep.gamehub",
            // Retro/console emulators
            "com.ppsspp", "org.ppsspp",
            "org.dolphinemu", "org.citra",
            "com.retroarch", "org.libretro",
            "skyline.emu", "org.yuzu",
            // Game publishers
            "com.activision", "com.ea.", "com.gameloft",
            "com.supercell", "com.tencent", "com.mihoyo",
            "com.epicgames", "com.pubg", "com.garena",
            "com.riotgames", "net.wargaming", "com.rockstargames",
            "com.mojang", "com.innersloth", "com.squareenix",
            "com.bandainamco", "com.konami", "com.sega",
            "com.netease", "com.blizzard", "com.ubisoft",
        )
        return gamePatterns.any { packageName.startsWith(it, ignoreCase = true) }
    }

    private fun hasGameMetadata(appInfo: android.content.pm.ApplicationInfo): Boolean {
        return appInfo.metaData?.let { meta ->
            meta.containsKey("com.samsung.android.vr.application.mode") ||
            meta.containsKey("notch.config") ||
            meta.containsKey("com.google.android.gms.games.APP_ID")
        } ?: false
    }
}
