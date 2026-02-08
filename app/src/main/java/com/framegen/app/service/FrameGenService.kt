package com.framegen.app.service

import android.app.*
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.os.*
import android.util.Log
import android.view.Surface
import androidx.core.app.NotificationCompat
import com.framegen.app.MainActivity
import com.framegen.app.R
import com.framegen.app.engine.FrameGenEngine
import com.framegen.app.engine.GameLauncher
import com.framegen.app.engine.RefreshRateController
import kotlinx.coroutines.*

/**
 * FrameGenService — persistent foreground service that runs frame generation
 * in the background. Auto-activates when a known game is detected.
 *
 * This is the "system-level" core: once started, it stays alive and
 * monitors for games, auto-injects Vulkan layer, and manages the engine.
 */
class FrameGenService : Service() {

    companion object {
        const val TAG = "FrameGenService"
        const val CHANNEL_ID = "framegen_service"
        const val NOTIFICATION_ID = 1001
        const val STATS_NOTIFICATION_ID = 1002

        const val ACTION_START = "com.framegen.app.START"
        const val ACTION_STOP = "com.framegen.app.STOP"
        const val ACTION_TOGGLE = "com.framegen.app.TOGGLE"
        const val ACTION_UPDATE_MODE = "com.framegen.app.UPDATE_MODE"
        const val ACTION_GAME_DETECTED = "com.framegen.app.GAME_DETECTED"
        const val ACTION_GAME_CLOSED = "com.framegen.app.GAME_CLOSED"

        const val EXTRA_MODE = "mode"
        const val EXTRA_QUALITY = "quality"
        const val EXTRA_PACKAGE = "package_name"

        const val PREF_NAME = "framegen_prefs"
        const val PREF_ENABLED = "enabled"
        const val PREF_MODE = "mode"
        const val PREF_QUALITY = "quality"
        const val PREF_AUTO_DETECT = "auto_detect"
        const val PREF_AUTO_START_BOOT = "auto_start_boot"

        @Volatile
        var isRunning = false
            private set

        @Volatile
        var isActivelyGenerating = false
            private set

        @Volatile
        var currentGamePackage: String? = null
            private set

        fun start(context: Context) {
            val intent = Intent(context, FrameGenService::class.java).apply {
                action = ACTION_START
            }
            context.startForegroundService(intent)
        }

        fun stop(context: Context) {
            val intent = Intent(context, FrameGenService::class.java).apply {
                action = ACTION_STOP
            }
            context.startService(intent)
        }

        fun toggle(context: Context) {
            val intent = Intent(context, FrameGenService::class.java).apply {
                action = ACTION_TOGGLE
            }
            context.startForegroundService(intent)
        }

        fun updateMode(context: Context, mode: Int, quality: Float) {
            val intent = Intent(context, FrameGenService::class.java).apply {
                action = ACTION_UPDATE_MODE
                putExtra(EXTRA_MODE, mode)
                putExtra(EXTRA_QUALITY, quality)
            }
            context.startService(intent)
        }

        fun notifyGameDetected(context: Context, packageName: String) {
            val intent = Intent(context, FrameGenService::class.java).apply {
                action = ACTION_GAME_DETECTED
                putExtra(EXTRA_PACKAGE, packageName)
            }
            context.startForegroundService(intent)
        }

        fun notifyGameClosed(context: Context) {
            val intent = Intent(context, FrameGenService::class.java).apply {
                action = ACTION_GAME_CLOSED
            }
            context.startService(intent)
        }
    }

    private lateinit var engine: FrameGenEngine
    private lateinit var gameLauncher: GameLauncher
    private lateinit var prefs: SharedPreferences
    private val serviceScope = CoroutineScope(Dispatchers.Default + SupervisorJob())
    private var statsJob: Job? = null
    private var monitorJob: Job? = null

    private var currentMode = 1      // 30→60
    private var currentQuality = 0.5f
    private var enabled = true

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "Service created")

        engine = FrameGenEngine()
        gameLauncher = GameLauncher(this)
        prefs = getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)

        // Restore saved settings
        enabled = prefs.getBoolean(PREF_ENABLED, true)
        currentMode = prefs.getInt(PREF_MODE, 1)
        currentQuality = prefs.getFloat(PREF_QUALITY, 0.5f)

        createNotificationChannels()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> handleStart()
            ACTION_STOP -> handleStop()
            ACTION_TOGGLE -> handleToggle()
            ACTION_UPDATE_MODE -> {
                currentMode = intent.getIntExtra(EXTRA_MODE, currentMode)
                currentQuality = intent.getFloatExtra(EXTRA_QUALITY, currentQuality)
                savePrefs()
                engine.setMode(currentMode)
                engine.setQuality(currentQuality)
                updateNotification()
            }
            ACTION_GAME_DETECTED -> {
                val pkg = intent.getStringExtra(EXTRA_PACKAGE) ?: return START_STICKY
                handleGameDetected(pkg)
            }
            ACTION_GAME_CLOSED -> handleGameClosed()
        }
        return START_STICKY
    }

    private fun handleStart() {
        if (isRunning) return

        isRunning = true
        enabled = true
        savePrefs()

        startForeground(NOTIFICATION_ID, buildNotification())
        startProcessMonitor()

        Log.i(TAG, "Service started — monitoring for games")

        // Broadcast state
        sendBroadcast(Intent("com.framegen.app.STATE_CHANGED"))
    }

    private fun handleStop() {
        Log.i(TAG, "Service stopping")

        if (isActivelyGenerating) {
            engine.stop()
            isActivelyGenerating = false
        }

        statsJob?.cancel()
        monitorJob?.cancel()
        isRunning = false
        currentGamePackage = null

        savePrefs()
        sendBroadcast(Intent("com.framegen.app.STATE_CHANGED"))

        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun handleToggle() {
        if (isRunning) {
            handleStop()
        } else {
            handleStart()
        }
    }

    private fun handleGameDetected(packageName: String) {
        if (!enabled || !isRunning) return
        if (currentGamePackage == packageName && isActivelyGenerating) return

        Log.i(TAG, "Game detected: $packageName — activating frame generation")
        currentGamePackage = packageName

        // Inject Vulkan layer
        gameLauncher.launchGame(packageName)

        // Start engine for this game
        activateForGame(packageName)
    }

    private fun activateForGame(packageName: String) {
        serviceScope.launch {
            try {
                val targetFps = when (currentMode) {
                    1 -> 60; 2 -> 90; 3 -> 120; else -> 60
                }

                // Setup Vulkan layer for the game
                gameLauncher.injectVulkanLayerViaShizuku(packageName)

                isActivelyGenerating = true
                currentGamePackage = packageName
                updateNotification()
                startStatsMonitor()

                Log.i(TAG, "Frame generation active for $packageName @ ${targetFps}fps")
                sendBroadcast(Intent("com.framegen.app.STATE_CHANGED"))

            } catch (e: Exception) {
                Log.e(TAG, "Failed to activate for $packageName", e)
                isActivelyGenerating = false
            }
        }
    }

    private fun handleGameClosed() {
        if (!isActivelyGenerating) return

        Log.i(TAG, "Game closed: $currentGamePackage — deactivating")

        engine.stop()
        statsJob?.cancel()
        isActivelyGenerating = false
        currentGamePackage = null

        updateNotification()
        sendBroadcast(Intent("com.framegen.app.STATE_CHANGED"))
    }

    /**
     * Monitors running processes to detect game launches/exits.
     * Works alongside the Accessibility Service.
     */
    private fun startProcessMonitor() {
        monitorJob?.cancel()
        monitorJob = serviceScope.launch {
            val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager

            while (isActive && isRunning) {
                try {
                    // Check currently running tasks
                    val tasks = am.getRunningTasks(1)
                    if (tasks.isNotEmpty()) {
                        val topPackage = tasks[0].topActivity?.packageName ?: ""

                        if (topPackage != packageName && isGamePackage(topPackage)) {
                            if (currentGamePackage != topPackage) {
                                handleGameDetected(topPackage)
                            }
                        } else if (isActivelyGenerating && topPackage != currentGamePackage) {
                            // Game is no longer on top
                            handleGameClosed()
                        }
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "Process monitor error", e)
                }

                delay(2000) // Check every 2 seconds
            }
        }
    }

    /**
     * Check if a package is a known game.
     */
    private fun isGamePackage(packageName: String): Boolean {
        return try {
            val appInfo = packageManager.getApplicationInfo(packageName, 0)
            appInfo.category == android.content.pm.ApplicationInfo.CATEGORY_GAME ||
                gameLauncher.isKnownGame(packageName)
        } catch (e: Exception) {
            false
        }
    }

    /**
     * Real-time stats monitoring + notification updates.
     */
    private fun startStatsMonitor() {
        statsJob?.cancel()
        statsJob = serviceScope.launch {
            while (isActive && isActivelyGenerating) {
                try {
                    val stats = engine.getStats()
                    val temp = engine.getGpuTemperature()
                    val throttled = engine.isThermalThrottled()

                    updateStatsNotification(stats, temp, throttled)
                } catch (e: Exception) {
                    Log.w(TAG, "Stats monitor error", e)
                }
                delay(1000)
            }
        }
    }

    // ─── Notifications ───────────────────────────────

    private fun createNotificationChannels() {
        val serviceChannel = NotificationChannel(
            CHANNEL_ID,
            "FrameGen Service",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Frame generation background service"
            setShowBadge(false)
        }

        val statsChannel = NotificationChannel(
            "framegen_stats",
            "FrameGen Stats",
            NotificationManager.IMPORTANCE_MIN
        ).apply {
            description = "Real-time performance stats"
            setShowBadge(false)
        }

        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(serviceChannel)
        nm.createNotificationChannel(statsChannel)
    }

    private fun buildNotification(): Notification {
        val openIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val stopIntent = PendingIntent.getService(
            this, 1,
            Intent(this, FrameGenService::class.java).apply { action = ACTION_STOP },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val modeText = when (currentMode) {
            0 -> "OFF"; 1 -> "30→60"; 2 -> "30→90"; 3 -> "30→120"; else -> "?"
        }

        val statusText = if (isActivelyGenerating) {
            "Active: $currentGamePackage | $modeText FPS"
        } else {
            "Monitoring for games... | $modeText FPS"
        }

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("⚡ FrameGen")
            .setContentText(statusText)
            .setSmallIcon(android.R.drawable.ic_menu_manage)
            .setContentIntent(openIntent)
            .addAction(android.R.drawable.ic_media_pause, "Stop", stopIntent)
            .setOngoing(true)
            .setSilent(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .build()
    }

    private fun updateNotification() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.notify(NOTIFICATION_ID, buildNotification())
    }

    private fun updateStatsNotification(stats: FrameGenEngine.Stats, temp: Float, throttled: Boolean) {
        val nm = getSystemService(NotificationManager::class.java)

        val text = "FPS: %.0f | %.1fms | GPU: %.0f°C %s".format(
            stats.effectiveFps,
            stats.totalMs,
            temp,
            if (throttled) "⚠" else "✓"
        )

        val notification = NotificationCompat.Builder(this, "framegen_stats")
            .setContentTitle("⚡ FrameGen Stats")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_menu_info_details)
            .setSilent(true)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_MIN)
            .build()

        nm.notify(STATS_NOTIFICATION_ID, notification)
    }

    // ─── Prefs ───────────────────────────────────────

    private fun savePrefs() {
        prefs.edit()
            .putBoolean(PREF_ENABLED, enabled)
            .putInt(PREF_MODE, currentMode)
            .putFloat(PREF_QUALITY, currentQuality)
            .apply()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        Log.i(TAG, "Service destroyed")
        serviceScope.cancel()
        statsJob?.cancel()
        monitorJob?.cancel()
        isRunning = false
        isActivelyGenerating = false
        super.onDestroy()
    }
}
