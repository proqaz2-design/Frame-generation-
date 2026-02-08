package com.framegen.app

import android.content.*
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.framegen.app.engine.FrameGenEngine
import com.framegen.app.engine.GameLauncher
import com.framegen.app.engine.RefreshRateController
import com.framegen.app.service.FrameGenService
import com.framegen.app.service.GameDetectorService
import com.framegen.app.overlay.FpsOverlayService
import kotlinx.coroutines.*

class MainActivity : AppCompatActivity() {

    private lateinit var engine: FrameGenEngine
    private lateinit var gameLauncher: GameLauncher
    private lateinit var refreshController: RefreshRateController

    private lateinit var surfaceView: SurfaceView
    private lateinit var btnStart: Button
    private lateinit var btnStop: Button
    private lateinit var btnSelectGame: Button
    private lateinit var spinnerMode: Spinner
    private lateinit var seekQuality: SeekBar
    private lateinit var txtStats: TextView
    private lateinit var txtDisplayInfo: TextView
    private lateinit var txtGpuTemp: TextView
    private lateinit var switchThermal: Switch
    private lateinit var gameListView: ListView
    private lateinit var switchAutoStart: Switch
    private lateinit var switchSystemService: Switch
    private lateinit var btnAccessibility: Button
    private lateinit var txtServiceStatus: TextView
    private lateinit var switchFpsOverlay: Switch

    private var statsJob: Job? = null
    private var selectedGame: GameLauncher.GameInfo? = null

    // Listen for service state changes
    private val stateReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            updateServiceStatusUI()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        engine = FrameGenEngine()
        gameLauncher = GameLauncher(this)
        refreshController = RefreshRateController(this)

        initViews()
        setupListeners()
        showDisplayInfo()

        // Register for service state updates
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(stateReceiver, IntentFilter("com.framegen.app.STATE_CHANGED"),
                RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(stateReceiver, IntentFilter("com.framegen.app.STATE_CHANGED"))
        }

        // Auto-start service if it was enabled
        val prefs = getSharedPreferences(FrameGenService.PREF_NAME, MODE_PRIVATE)
        if (prefs.getBoolean(FrameGenService.PREF_ENABLED, false)) {
            FrameGenService.start(this)
        }
    }

    private fun initViews() {
        surfaceView = findViewById(R.id.surfaceView)
        btnStart = findViewById(R.id.btnStart)
        btnStop = findViewById(R.id.btnStop)
        btnSelectGame = findViewById(R.id.btnSelectGame)
        spinnerMode = findViewById(R.id.spinnerMode)
        seekQuality = findViewById(R.id.seekQuality)
        txtStats = findViewById(R.id.txtStats)
        txtDisplayInfo = findViewById(R.id.txtDisplayInfo)
        txtGpuTemp = findViewById(R.id.txtGpuTemp)
        switchThermal = findViewById(R.id.switchThermal)
        gameListView = findViewById(R.id.gameListView)

        // Mode spinner
        val modes = arrayOf("OFF", "30→60 FPS", "30→90 FPS", "30→120 FPS")
        spinnerMode.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, modes)
        spinnerMode.setSelection(1) // Default: 60fps

        // Quality slider
        seekQuality.max = 100
        seekQuality.progress = 50

        btnStop.isEnabled = false

        // System service controls
        switchAutoStart = findViewById(R.id.switchAutoStart)
        switchSystemService = findViewById(R.id.switchSystemService)
        btnAccessibility = findViewById(R.id.btnAccessibility)
        txtServiceStatus = findViewById(R.id.txtServiceStatus)
        switchFpsOverlay = findViewById(R.id.switchFpsOverlay)

        // Restore prefs
        val prefs = getSharedPreferences(FrameGenService.PREF_NAME, MODE_PRIVATE)
        switchAutoStart.isChecked = prefs.getBoolean(FrameGenService.PREF_AUTO_START_BOOT, false)
        switchSystemService.isChecked = FrameGenService.isRunning
        switchFpsOverlay.isChecked = FpsOverlayService.isShowing

        updateServiceStatusUI()
    }

    private fun setupListeners() {
        // Surface lifecycle
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                // Surface is ready for Vulkan rendering
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                // Reinitialize if size changes
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                engine.stop()
            }
        })

        // Start button
        btnStart.setOnClickListener {
            startFrameGeneration()
        }

        // Stop button
        btnStop.setOnClickListener {
            stopFrameGeneration()
        }

        // Select game
        btnSelectGame.setOnClickListener {
            showGameList()
        }

        // Mode change
        spinnerMode.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, pos: Int, id: Long) {
                engine.setMode(pos)
                // Update target refresh rate
                val targetRate = when (pos) {
                    1 -> 60f
                    2 -> 90f
                    3 -> 120f
                    else -> 60f
                }
                refreshController.requestRefreshRate(this@MainActivity, targetRate)
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        // Quality slider
        seekQuality.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                engine.setQuality(progress / 100f)
                FrameGenService.updateMode(this@MainActivity,
                    spinnerMode.selectedItemPosition, progress / 100f)
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        // System service toggle
        switchSystemService.setOnCheckedChangeListener { _, isChecked ->
            if (isChecked) {
                FrameGenService.start(this)
            } else {
                FrameGenService.stop(this)
            }
            updateServiceStatusUI()
        }

        // Auto-start on boot toggle
        switchAutoStart.setOnCheckedChangeListener { _, isChecked ->
            getSharedPreferences(FrameGenService.PREF_NAME, MODE_PRIVATE)
                .edit()
                .putBoolean(FrameGenService.PREF_AUTO_START_BOOT, isChecked)
                .apply()
        }

        // Open accessibility settings
        btnAccessibility.setOnClickListener {
            val intent = Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)
            startActivity(intent)
            Toast.makeText(this, "Enable 'FrameGen Game Detector'", Toast.LENGTH_LONG).show()
        }

        // FPS Overlay toggle
        switchFpsOverlay.setOnCheckedChangeListener { _, isChecked ->
            if (isChecked) {
                // Check overlay permission
                if (!Settings.canDrawOverlays(this)) {
                    switchFpsOverlay.isChecked = false
                    val intent = Intent(
                        Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                        android.net.Uri.parse("package:$packageName")
                    )
                    startActivity(intent)
                    Toast.makeText(this, "Allow 'Display over other apps' for FrameGen", Toast.LENGTH_LONG).show()
                } else {
                    val targetFps = when (spinnerMode.selectedItemPosition) {
                        1 -> 60f; 2 -> 90f; 3 -> 120f; else -> 60f
                    }
                    FpsOverlayService.show(this, targetFps)
                }
            } else {
                FpsOverlayService.hide(this)
            }
        }
    }

    private fun startFrameGeneration() {
        val surface = surfaceView.holder.surface
        if (!surface.isValid) {
            Toast.makeText(this, "Surface not ready", Toast.LENGTH_SHORT).show()
            return
        }

        val mode = spinnerMode.selectedItemPosition
        val quality = seekQuality.progress / 100f
        val targetFps = when (mode) {
            1 -> 60
            2 -> 90
            3 -> 120
            else -> 60
        }

        // Request matching refresh rate
        refreshController.requestMaxRefreshRate(this)

        // Initialize and start engine
        val success = engine.init(surface, assets, mode, quality, targetFps)
        if (!success) {
            Toast.makeText(this, "Engine initialization failed", Toast.LENGTH_LONG).show()
            return
        }

        engine.start()

        btnStart.isEnabled = false
        btnStop.isEnabled = true

        // Start stats monitoring
        startStatsMonitor()

        // Launch selected game
        selectedGame?.let { game ->
            gameLauncher.launchGame(game.packageName)
        }

        Toast.makeText(this, "Frame generation active: ${targetFps}fps", Toast.LENGTH_SHORT).show()
    }

    private fun stopFrameGeneration() {
        engine.stop()
        statsJob?.cancel()

        btnStart.isEnabled = true
        btnStop.isEnabled = false

        txtStats.text = "Stopped"
    }

    private fun startStatsMonitor() {
        statsJob = lifecycleScope.launch(Dispatchers.IO) {
            while (isActive) {
                val stats = engine.getStats()
                val temp = engine.getGpuTemperature()
                val throttled = engine.isThermalThrottled()

                withContext(Dispatchers.Main) {
                    txtStats.text = buildString {
                        appendLine("FPS          %.1f".format(stats.effectiveFps))
                        appendLine("Capture      %.2f ms".format(stats.captureMs))
                        appendLine("Motion       %.2f ms".format(stats.motionMs))
                        appendLine("Interpolate  %.2f ms".format(stats.interpolationMs))
                        appendLine("Present      %.2f ms".format(stats.presentMs))
                        appendLine("Total        %.2f ms".format(stats.totalMs))
                        appendLine("Generated    ${stats.framesGenerated}")
                        append("Dropped      ${stats.framesDropped}")
                    }
                    txtStats.setTextColor(
                        if (stats.effectiveFps > 50) android.graphics.Color.parseColor("#00ff88")
                        else if (stats.effectiveFps > 30) android.graphics.Color.parseColor("#ffaa00")
                        else android.graphics.Color.parseColor("#e63946")
                    )

                    txtGpuTemp.text = "%.0f°C".format(temp)
                    val tempColor = when {
                        throttled -> android.graphics.Color.parseColor("#e63946")
                        temp >= 70 -> android.graphics.Color.parseColor("#ffaa00")
                        else -> android.graphics.Color.parseColor("#00ff88")
                    }
                    txtGpuTemp.setTextColor(tempColor)
                }

                delay(500) // Update every 500ms
            }
        }
    }

    private fun showGameList() {
        lifecycleScope.launch(Dispatchers.IO) {
            val games = gameLauncher.getInstalledGames()

            withContext(Dispatchers.Main) {
                val adapter = ArrayAdapter(
                    this@MainActivity,
                    android.R.layout.simple_list_item_2,
                    android.R.id.text1,
                    games.map { "${it.appName}\n${if (it.isVulkanGame) "✓ Vulkan" else "○ OpenGL"}" }
                )

                gameListView.adapter = adapter
                gameListView.visibility = View.VISIBLE

                gameListView.setOnItemClickListener { _, _, position, _ ->
                    selectedGame = games[position]
                    btnSelectGame.text = "Game: ${games[position].appName}"
                    gameListView.visibility = View.GONE
                }
            }
        }
    }

    private fun showDisplayInfo() {
        val info = refreshController.getDisplayInfo()
        txtDisplayInfo.text = "${info.currentRate.toInt()} Hz"
    }

    override fun onDestroy() {
        statsJob?.cancel()
        engine.destroy()
        try { unregisterReceiver(stateReceiver) } catch (_: Exception) {}
        super.onDestroy()
    }

    override fun onResume() {
        super.onResume()
        updateServiceStatusUI()
    }

    override fun onPause() {
        super.onPause()
        // Don't stop engine — service keeps it running in the background
    }

    private fun updateServiceStatusUI() {
        val serviceOn = FrameGenService.isRunning
        val generating = FrameGenService.isActivelyGenerating
        val accessibilityOn = GameDetectorService.isServiceActive
        val game = FrameGenService.currentGamePackage

        switchSystemService.isChecked = serviceOn

        // Update status dot in header
        val txtStatusDot = findViewById<TextView>(R.id.txtStatusDot)
        if (generating) {
            txtStatusDot.text = "ON"
            txtStatusDot.setTextColor(android.graphics.Color.parseColor("#00ff88"))
        } else if (serviceOn) {
            txtStatusDot.text = "IDLE"
            txtStatusDot.setTextColor(android.graphics.Color.parseColor("#00d4ff"))
        } else {
            txtStatusDot.text = "OFF"
            txtStatusDot.setTextColor(android.graphics.Color.parseColor("#555577"))
        }

        val status = buildString {
            append(if (serviceOn) "● " else "○ ")
            append("Service: ")
            appendLine(if (serviceOn) "Running" else "Stopped")

            append(if (accessibilityOn) "● " else "○ ")
            append("Detector: ")
            appendLine(if (accessibilityOn) "Active" else "Disabled")

            append(if (generating) "● " else "○ ")
            append("FrameGen: ")
            if (generating) {
                appendLine("Active")
                append("  └ ")
                append(game?.split(".")?.lastOrNull() ?: game ?: "unknown")
            } else {
                append(if (serviceOn) "Waiting for game..." else "Off")
            }
        }

        txtServiceStatus.text = status
        txtServiceStatus.setTextColor(
            if (generating) android.graphics.Color.parseColor("#00ff88")
            else android.graphics.Color.parseColor("#5a5a8a")
        )

        // Accessibility button
        if (!accessibilityOn) {
            btnAccessibility.background = getDrawable(R.drawable.btn_warning)
            btnAccessibility.text = "Enable Game Detector"
        } else {
            btnAccessibility.background = getDrawable(R.drawable.btn_start)
            btnAccessibility.text = "Game Detector Active"
        }
    }
}
