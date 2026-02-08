package com.framegen.app.service

import android.graphics.drawable.Icon
import android.os.Build
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import android.util.Log

/**
 * Quick Settings Tile — toggle FrameGen from the notification shade.
 * Pull down → tap the FrameGen tile → toggles on/off.
 *
 * Works like WiFi or Bluetooth toggle — system-level feel.
 */
class FrameGenTileService : TileService() {

    companion object {
        private const val TAG = "FrameGenTile"
    }

    override fun onStartListening() {
        super.onStartListening()
        updateTileState()
    }

    override fun onClick() {
        super.onClick()

        Log.i(TAG, "Tile clicked — toggling service")

        if (FrameGenService.isRunning) {
            FrameGenService.stop(this)
        } else {
            FrameGenService.start(this)
        }

        // Small delay to let service state update
        qsTile?.let { tile ->
            tile.state = if (FrameGenService.isRunning) Tile.STATE_INACTIVE else Tile.STATE_ACTIVE
            tile.updateTile()
        }

        // Schedule state refresh
        android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
            updateTileState()
        }, 500)
    }

    override fun onTileAdded() {
        super.onTileAdded()
        Log.i(TAG, "Tile added to Quick Settings")
        updateTileState()
    }

    override fun onTileRemoved() {
        super.onTileRemoved()
        Log.i(TAG, "Tile removed from Quick Settings")
    }

    private fun updateTileState() {
        val tile = qsTile ?: return

        if (FrameGenService.isRunning) {
            tile.state = Tile.STATE_ACTIVE
            tile.label = "FrameGen"

            if (FrameGenService.isActivelyGenerating) {
                tile.subtitle = "Active: ${FrameGenService.currentGamePackage?.split(".")?.lastOrNull() ?: "game"}"
            } else {
                tile.subtitle = "Monitoring"
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                tile.subtitle = tile.subtitle
            }
        } else {
            tile.state = Tile.STATE_INACTIVE
            tile.label = "FrameGen"
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                tile.subtitle = "Off"
            }
        }

        tile.updateTile()
    }
}
