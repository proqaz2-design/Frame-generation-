package com.framegen.app.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * BootReceiver — auto-starts FrameGen service on device boot.
 * 
 * Behavior:
 * - If "auto start on boot" is enabled in settings, starts the service
 * - Service enters monitoring mode (no active generation until a game is detected)
 * - Works the same as if user manually started it
 */
class BootReceiver : BroadcastReceiver() {

    companion object {
        private const val TAG = "FrameGenBoot"
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED &&
            intent.action != Intent.ACTION_LOCKED_BOOT_COMPLETED &&
            intent.action != "android.intent.action.QUICKBOOT_POWERON") {
            return
        }

        Log.i(TAG, "Boot completed — checking if FrameGen should auto-start")

        val prefs = context.getSharedPreferences(
            FrameGenService.PREF_NAME, Context.MODE_PRIVATE
        )

        val autoStart = prefs.getBoolean(FrameGenService.PREF_AUTO_START_BOOT, false)
        val wasEnabled = prefs.getBoolean(FrameGenService.PREF_ENABLED, false)

        if (autoStart && wasEnabled) {
            Log.i(TAG, "Auto-starting FrameGen service")
            FrameGenService.start(context)
        } else {
            Log.i(TAG, "Auto-start disabled or was not enabled. autoStart=$autoStart, wasEnabled=$wasEnabled")
        }
    }
}
