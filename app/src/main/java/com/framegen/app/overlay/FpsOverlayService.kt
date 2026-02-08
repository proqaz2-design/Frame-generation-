package com.framegen.app.overlay

import android.app.Service
import android.content.Context
import android.content.Intent
import android.graphics.PixelFormat
import android.os.Build
import android.os.IBinder
import android.util.Log
import android.view.Gravity
import android.view.WindowManager
import com.framegen.app.engine.FrameGenEngine

/**
 * FpsOverlayService — manages the floating FPS overlay window.
 *
 * Uses SYSTEM_ALERT_WINDOW permission to draw over other apps.
 * The overlay shows real-time FPS, frame time graph, GPU temp,
 * and drop counter — like RTSS/Steam overlay but for Android.
 *
 * Can be toggled from:
 * - Main UI switch
 * - Quick Settings tile  
 * - FrameGenService when generation starts/stops
 */
class FpsOverlayService : Service() {

    companion object {
        private const val TAG = "FpsOverlay"

        const val ACTION_SHOW = "com.framegen.app.SHOW_OVERLAY"
        const val ACTION_HIDE = "com.framegen.app.HIDE_OVERLAY"
        const val ACTION_TOGGLE = "com.framegen.app.TOGGLE_OVERLAY"
        const val EXTRA_TARGET_FPS = "target_fps"

        @Volatile
        var isShowing = false
            private set

        fun show(context: Context, targetFps: Float = 60f) {
            val intent = Intent(context, FpsOverlayService::class.java).apply {
                action = ACTION_SHOW
                putExtra(EXTRA_TARGET_FPS, targetFps)
            }
            context.startService(intent)
        }

        fun hide(context: Context) {
            val intent = Intent(context, FpsOverlayService::class.java).apply {
                action = ACTION_HIDE
            }
            context.startService(intent)
        }

        fun toggle(context: Context, targetFps: Float = 60f) {
            if (isShowing) hide(context) else show(context, targetFps)
        }
    }

    private var windowManager: WindowManager? = null
    private var overlayView: FpsOverlayView? = null
    private var engine: FrameGenEngine? = null

    override fun onCreate() {
        super.onCreate()
        windowManager = getSystemService(WINDOW_SERVICE) as WindowManager
        engine = FrameGenEngine()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_SHOW -> {
                val targetFps = intent.getFloatExtra(EXTRA_TARGET_FPS, 60f)
                showOverlay(targetFps)
            }
            ACTION_HIDE -> hideOverlay()
            ACTION_TOGGLE -> {
                val targetFps = intent.getFloatExtra(EXTRA_TARGET_FPS, 60f)
                if (isShowing) hideOverlay() else showOverlay(targetFps)
            }
        }
        return START_STICKY
    }

    private fun showOverlay(targetFps: Float) {
        if (isShowing) {
            overlayView?.setTargetFps(targetFps)
            return
        }

        try {
            val view = FpsOverlayView(this)
            view.attachEngine(engine!!)
            view.setTargetFps(targetFps)

            val params = WindowManager.LayoutParams(
                320,
                280,
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
                    WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                else
                    @Suppress("DEPRECATION")
                    WindowManager.LayoutParams.TYPE_PHONE,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                    WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT
            ).apply {
                gravity = Gravity.TOP or Gravity.START
                x = 20
                y = 80
            }

            windowManager?.addView(view, params)
            overlayView = view
            view.start()
            isShowing = true

            Log.i(TAG, "FPS overlay shown (target: ${targetFps}fps)")

        } catch (e: Exception) {
            Log.e(TAG, "Failed to show overlay", e)
            isShowing = false
        }
    }

    private fun hideOverlay() {
        try {
            overlayView?.let { view ->
                view.stop()
                windowManager?.removeView(view)
            }
            overlayView = null
            isShowing = false
            Log.i(TAG, "FPS overlay hidden")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to hide overlay", e)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        hideOverlay()
        super.onDestroy()
    }
}
