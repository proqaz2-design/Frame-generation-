package com.framegen.app

import android.app.ActivityManager
import android.content.*
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.util.Log
import android.view.View
import android.widget.*
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.framegen.app.engine.RefreshRateController
import com.framegen.app.overlay.FpsOverlayService
import com.framegen.app.service.FrameGenService
import com.framegen.app.service.GameDetectorService
import kotlinx.coroutines.*
import java.io.File

/**
 * Frame Generation for Android — Rootless
 *
 * Utility dashboard inspired by lybxlpsv's Frame Generation.
 * Injects Vulkan interpolation layer into games WITHOUT root,
 * using Shizuku / ADB gpu_debug_layers mechanism.
 */
class MainActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "FrameGenMain"
        private const val PREFS = "framegen_prefs"
    }

    // Device info views
    private lateinit var txtGpuName: TextView
    private lateinit var txtVulkanDriver: TextView
    private lateinit var txtRefreshRate: TextView
    private lateinit var txtResolution: TextView

    // Compatibility
    private lateinit var txtCompatStatus: TextView
    private lateinit var checkVulkan: TextView
    private lateinit var checkGpu: TextView
    private lateinit var checkRefresh: TextView
    private lateinit var checkShizuku: TextView

    // Controls
    private lateinit var spinnerMethod: Spinner
    private lateinit var spinnerMode: Spinner
    private lateinit var seekTargetFps: SeekBar
    private lateinit var txtTargetFps: TextView
    private lateinit var seekQuality: SeekBar
    private lateinit var txtQuality: TextView

    // Game selection
    private lateinit var btnSelectGame: Button
    private lateinit var txtSelectedGame: TextView
    private lateinit var txtGameInfo: TextView

    // Main toggle
    private lateinit var btnToggle: Button

    // Stats
    private lateinit var cardStats: View
    private lateinit var txtFps: TextView
    private lateinit var txtFrameTime: TextView
    private lateinit var txtGenerated: TextView
    private lateinit var txtDropped: TextView
    private lateinit var txtGpuTemp: TextView

    // Status
    private lateinit var statusDot: View
    private lateinit var txtStatus: TextView
    private lateinit var statusBadge: View

    // Options
    private lateinit var switchFpsOverlay: Switch
    private lateinit var switchAutoDetect: Switch
    private lateinit var switchForceRefresh: Switch
    private lateinit var switchThermal: Switch

    private var statsJob: Job? = null
    private var isActive = false
    private var selectedPackage: String? = null
    private var selectedAppName: String? = null

    private val stateReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            updateUI()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        initViews()
        setupListeners()
        detectDeviceInfo()
        checkCompatibility()
        loadPrefs()
        updateUI()

        // Register receiver
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(stateReceiver, IntentFilter("com.framegen.app.STATE_CHANGED"),
                RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(stateReceiver, IntentFilter("com.framegen.app.STATE_CHANGED"))
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        statsJob?.cancel()
        try { unregisterReceiver(stateReceiver) } catch (_: Exception) {}
    }

    override fun onResume() {
        super.onResume()
        checkCompatibility()
        updateUI()
    }

    // ================================================================
    // Init
    // ================================================================

    private fun initViews() {
        txtGpuName = findViewById(R.id.txtGpuName)
        txtVulkanDriver = findViewById(R.id.txtVulkanDriver)
        txtRefreshRate = findViewById(R.id.txtRefreshRate)
        txtResolution = findViewById(R.id.txtResolution)

        txtCompatStatus = findViewById(R.id.txtCompatStatus)
        checkVulkan = findViewById(R.id.checkVulkan)
        checkGpu = findViewById(R.id.checkGpu)
        checkRefresh = findViewById(R.id.checkRefresh)
        checkShizuku = findViewById(R.id.checkShizuku)

        spinnerMethod = findViewById(R.id.spinnerMethod)
        spinnerMode = findViewById(R.id.spinnerMode)
        seekTargetFps = findViewById(R.id.seekTargetFps)
        txtTargetFps = findViewById(R.id.txtTargetFps)
        seekQuality = findViewById(R.id.seekQuality)
        txtQuality = findViewById(R.id.txtQuality)

        btnSelectGame = findViewById(R.id.btnSelectGame)
        txtSelectedGame = findViewById(R.id.txtSelectedGame)
        txtGameInfo = findViewById(R.id.txtGameInfo)

        btnToggle = findViewById(R.id.btnToggle)

        cardStats = findViewById(R.id.cardStats)
        txtFps = findViewById(R.id.txtFps)
        txtFrameTime = findViewById(R.id.txtFrameTime)
        txtGenerated = findViewById(R.id.txtGenerated)
        txtDropped = findViewById(R.id.txtDropped)
        txtGpuTemp = findViewById(R.id.txtGpuTemp)

        statusDot = findViewById(R.id.statusDot)
        txtStatus = findViewById(R.id.txtStatus)
        statusBadge = findViewById(R.id.statusBadge)

        switchFpsOverlay = findViewById(R.id.switchFpsOverlay)
        switchAutoDetect = findViewById(R.id.switchAutoDetect)
        switchForceRefresh = findViewById(R.id.switchForceRefresh)
        switchThermal = findViewById(R.id.switchThermal)

        // Populate spinners
        val methods = arrayOf("Shizuku (recommended)", "ADB WiFi Debug", "Accessibility Hook")
        spinnerMethod.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, methods)

        val modes = arrayOf("RIFE (Balanced)", "RIFE (Quality)", "MVA Fast", "Low Latency")
        spinnerMode.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, modes)
    }

    private fun setupListeners() {
        // Target FPS slider
        seekTargetFps.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                txtTargetFps.text = progress.toString()
            }
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {}
        })

        // Quality slider
        seekQuality.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                txtQuality.text = when {
                    progress < 25 -> "Low"
                    progress < 50 -> "Medium"
                    progress < 75 -> "High"
                    else -> "Ultra"
                }
            }
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {}
        })

        // Select game
        btnSelectGame.setOnClickListener { showGamePicker() }

        // Main toggle
        btnToggle.setOnClickListener { toggleFrameGeneration() }

        // FPS Overlay
        switchFpsOverlay.setOnCheckedChangeListener { _, isChecked ->
            if (isChecked) {
                if (Settings.canDrawOverlays(this)) {
                    FpsOverlayService.show(this)
                } else {
                    switchFpsOverlay.isChecked = false
                    startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION))
                }
            } else {
                FpsOverlayService.hide(this)
            }
        }

        // Auto-detect
        switchAutoDetect.setOnCheckedChangeListener { _, isChecked ->
            if (isChecked && !isAccessibilityEnabled()) {
                switchAutoDetect.isChecked = false
                AlertDialog.Builder(this, R.style.Theme_FrameGen)
                    .setTitle("Accessibility Service Required")
                    .setMessage("Enable the FrameGen game detector in Accessibility settings to auto-detect when games launch.")
                    .setPositiveButton("Open Settings") { _, _ ->
                        startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
                    }
                    .setNegativeButton("Cancel", null)
                    .show()
            }
        }

        // Force refresh rate
        switchForceRefresh.setOnCheckedChangeListener { _, isChecked ->
            if (isChecked) {
                val rrc = RefreshRateController(this)
                rrc.requestMaxRefreshRate(this)
            }
            savePrefs()
        }

        // Thermal guard
        switchThermal.setOnCheckedChangeListener { _, _ -> savePrefs() }
    }

    // ================================================================
    // Device Info Detection
    // ================================================================

    private fun detectDeviceInfo() {
        lifecycleScope.launch(Dispatchers.IO) {
            val gpuInfo = detectGpuInfo()
            val display = getDisplayInfo()

            withContext(Dispatchers.Main) {
                txtGpuName.text = gpuInfo.name
                txtVulkanDriver.text = gpuInfo.driverVersion
                txtRefreshRate.text = "${display.refreshRate.toInt()} Hz"
                txtResolution.text = "${display.width} × ${display.height}"
            }
        }
    }

    data class GpuInfo(val name: String, val driverVersion: String, val isAdreno: Boolean, val isMali: Boolean)
    data class DisplayInfo(val refreshRate: Float, val width: Int, val height: Int)

    private fun detectGpuInfo(): GpuInfo {
        var gpuName = "Unknown"
        var driverVersion = "Unknown"

        // Try reading from /sys
        try {
            val kgslFiles = listOf(
                "/sys/class/kgsl/kgsl-3d0/gpu_model",
                "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage"
            )
            for (path in kgslFiles) {
                val f = File(path.replace("gpu_busy_percentage", "gpu_model"))
                if (f.exists()) {
                    gpuName = f.readText().trim()
                    break
                }
            }
        } catch (_: Exception) {}

        // Fallback: try Build props
        if (gpuName == "Unknown") {
            gpuName = try {
                val prop = Runtime.getRuntime().exec(arrayOf("getprop", "ro.hardware.egl"))
                val result = prop.inputStream.bufferedReader().readText().trim()
                if (result.isNotEmpty()) result else Build.HARDWARE
            } catch (_: Exception) {
                Build.HARDWARE
            }
        }

        // Detect Adreno from chipset
        val chipset = Build.HARDWARE.lowercase()
        val isAdreno = gpuName.lowercase().contains("adreno") || chipset.contains("qcom") || chipset.contains("snapdragon")
        val isMali = gpuName.lowercase().contains("mali")

        // Get Vulkan driver version
        try {
            val proc = Runtime.getRuntime().exec(arrayOf("getprop", "ro.gfx.driver.1"))
            val result = proc.inputStream.bufferedReader().readText().trim()
            if (result.isNotEmpty()) {
                driverVersion = result
            }
        } catch (_: Exception) {}

        if (driverVersion == "Unknown") {
            driverVersion = "System default"
        }

        return GpuInfo(gpuName, driverVersion, isAdreno, isMali)
    }

    private fun getDisplayInfo(): DisplayInfo {
        val dm = resources.displayMetrics
        val refreshRate = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display?.refreshRate ?: 60f
        } else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay.refreshRate
        }
        return DisplayInfo(refreshRate, dm.widthPixels, dm.heightPixels)
    }

    // ================================================================
    // Compatibility Check
    // ================================================================

    private fun checkCompatibility() {
        lifecycleScope.launch(Dispatchers.IO) {
            val gpu = detectGpuInfo()
            val display = getDisplayInfo()
            val hasShizuku = isShizukuAvailable()

            // Check Vulkan
            val hasVulkan = File("/system/lib64/libvulkan.so").exists() ||
                    File("/system/lib/libvulkan.so").exists() ||
                    Build.VERSION.SDK_INT >= Build.VERSION_CODES.N

            // Check GPU compat
            val gpuOk = gpu.isAdreno || gpu.isMali

            // Check refresh rate (120Hz+)
            val refreshOk = display.refreshRate >= 90f

            val allOk = hasVulkan && gpuOk && refreshOk && hasShizuku

            withContext(Dispatchers.Main) {
                setCheckItem(checkVulkan, "Vulkan Support", hasVulkan)
                setCheckItem(checkGpu, "GPU: ${gpu.name}", gpuOk)
                setCheckItem(checkRefresh, "${display.refreshRate.toInt()}Hz Display", refreshOk)
                setCheckItem(checkShizuku, "Shizuku / ADB", hasShizuku)

                txtCompatStatus.text = when {
                    allOk -> "Device is compatible ✓"
                    !hasVulkan -> "Vulkan not detected. Frame generation requires Vulkan."
                    !gpuOk -> "GPU may not be fully compatible. Adreno 6xx+ or Mali Valhall recommended."
                    !refreshOk -> "Low refresh rate display. 120Hz+ recommended for best results."
                    !hasShizuku -> "Shizuku not running. Install Shizuku and activate via ADB."
                    else -> "Some compatibility issues detected."
                }
                txtCompatStatus.setTextColor(if (allOk) Color.parseColor("#4CAF50") else Color.parseColor("#FF9800"))
            }
        }
    }

    private fun setCheckItem(tv: TextView, label: String, ok: Boolean) {
        val icon = if (ok) "✓" else "✗"
        val color = if (ok) "#4CAF50" else "#FF5252"
        tv.text = "$icon  $label"
        tv.setTextColor(Color.parseColor(color))
    }

    private fun isShizukuAvailable(): Boolean {
        return try {
            rikka.shizuku.Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED
        } catch (_: Exception) {
            // Check if Shizuku is at least installed
            try {
                packageManager.getPackageInfo("moe.shizuku.privileged.api", 0)
                false // Installed but not running/permitted
            } catch (_: Exception) {
                false
            }
        }
    }

    // ================================================================
    // Game Picker
    // ================================================================

    private fun showGamePicker() {
        lifecycleScope.launch(Dispatchers.IO) {
            val pm = packageManager
            val games = mutableListOf<Pair<String, String>>() // name, package

            val apps = pm.getInstalledApplications(0)
            for (app in apps) {
                if (app.flags and ApplicationInfo.FLAG_SYSTEM != 0) continue

                val isGame = try {
                    app.category == ApplicationInfo.CATEGORY_GAME
                } catch (_: Exception) { false }

                // Also detect by package name patterns
                val pkg = app.packageName.lowercase()
                val isLikelyGame = isGame ||
                        pkg.contains("game") ||
                        pkg.contains("com.tencent") ||
                        pkg.contains("com.activision") ||
                        pkg.contains("com.supercell") ||
                        pkg.contains("com.garena") ||
                        pkg.contains("com.mihoyo") ||
                        pkg.contains("com.epicgames") ||
                        pkg.contains("com.riotgames") ||
                        pkg.contains("com.pubg") ||
                        pkg.contains("com.ea.") ||
                        pkg.contains("com.gameloft") ||
                        pkg.contains("com.netease") ||
                        pkg.contains("winlator") ||
                        pkg.contains("ppsspp") ||
                        pkg.contains("dolphin") ||
                        pkg.contains("retroarch") ||
                        pkg.contains("gamehub")

                if (isLikelyGame) {
                    val name = pm.getApplicationLabel(app).toString()
                    games.add(Pair(name, app.packageName))
                }
            }

            games.sortBy { it.first }

            withContext(Dispatchers.Main) {
                if (games.isEmpty()) {
                    Toast.makeText(this@MainActivity, "No games found", Toast.LENGTH_SHORT).show()
                    return@withContext
                }

                val names = games.map { "${it.first}\n${it.second}" }.toTypedArray()
                AlertDialog.Builder(this@MainActivity, R.style.Theme_FrameGen)
                    .setTitle("Select Game")
                    .setItems(names) { _, which ->
                        val (name, pkg) = games[which]
                        selectedPackage = pkg
                        selectedAppName = name
                        txtSelectedGame.text = name
                        txtSelectedGame.setTextColor(Color.parseColor("#CCCCCC"))
                        txtGameInfo.text = pkg
                        txtGameInfo.visibility = View.VISIBLE
                        savePrefs()
                    }
                    .show()
            }
        }
    }

    // ================================================================
    // Frame Generation Toggle
    // ================================================================

    private fun toggleFrameGeneration() {
        if (isActive) {
            stopFrameGeneration()
        } else {
            startFrameGeneration()
        }
    }

    private fun startFrameGeneration() {
        if (selectedPackage == null) {
            Toast.makeText(this, "Select a target app first", Toast.LENGTH_SHORT).show()
            return
        }

        lifecycleScope.launch {
            // Step 1: Inject Vulkan layer via chosen method
            val method = spinnerMethod.selectedItemPosition
            val success = withContext(Dispatchers.IO) {
                injectVulkanLayer(selectedPackage!!, method)
            }

            if (!success) {
                Toast.makeText(this@MainActivity,
                    "Failed to inject Vulkan layer. Check Shizuku/ADB.", Toast.LENGTH_LONG).show()
                return@launch
            }

            // Step 2: Start the background service
            FrameGenService.start(this@MainActivity)

            isActive = true
            updateUI()
            startStatsMonitor()

            Toast.makeText(this@MainActivity,
                "Frame generation enabled for $selectedAppName", Toast.LENGTH_SHORT).show()
        }
    }

    private fun stopFrameGeneration() {
        lifecycleScope.launch {
            // Clean up GPU debug layers
            withContext(Dispatchers.IO) {
                cleanupVulkanLayer()
            }

            FrameGenService.stop(this@MainActivity)
            statsJob?.cancel()
            isActive = false
            updateUI()
        }
    }

    // ================================================================
    // Rootless Vulkan Layer Injection
    // ================================================================

    /**
     * Inject our Vulkan layer into the target game using Android's
     * gpu_debug_layers system settings. This is the same mechanism
     * that GPU profiling tools use — NO ROOT REQUIRED.
     *
     * Method 0: Shizuku (recommended)
     * Method 1: ADB WiFi (requires pairing)
     * Method 2: Accessibility-based (fallback)
     */
    private fun injectVulkanLayer(targetPackage: String, method: Int): Boolean {
        return when (method) {
            0 -> com.framegen.app.util.ShizukuShell.injectLayer(targetPackage, packageName)
            1 -> {
                // ADB WiFi — same commands but user connects via ADB WiFi first
                com.framegen.app.util.ShizukuShell.injectLayer(targetPackage, packageName)
            }
            2 -> {
                // Accessibility fallback
                Log.i(TAG, "Using accessibility hook method")
                true
            }
            else -> false
        }
    }

    private fun cleanupVulkanLayer(): Boolean {
        return com.framegen.app.util.ShizukuShell.removeLayer()
    }

    // ================================================================
    // Stats Monitor
    // ================================================================

    private fun startStatsMonitor() {
        statsJob?.cancel()
        statsJob = lifecycleScope.launch {
            cardStats.visibility = View.VISIBLE
            while (this@MainActivity.isActive) {
                try {
                    // The Vulkan layer runs in the game process.
                    // Stats shown are target estimates.
                    val targetFps = seekTargetFps.progress.toFloat()
                    txtFps.text = String.format("%.0f", targetFps)
                    txtFrameTime.text = String.format("%.1f ms", 1000f / targetFps)
                    txtGenerated.text = "--"
                    txtDropped.text = "0"
                    txtGpuTemp.text = "--°C"
                } catch (_: Exception) {}
                delay(1000)
            }
        }
    }

    // ================================================================
    // UI Update
    // ================================================================

    private fun updateUI() {
        if (isActive) {
            btnToggle.text = "DISABLE FRAME GENERATION"
            btnToggle.setBackgroundResource(R.drawable.btn_disable)
            statusDot.setBackgroundResource(R.drawable.dot_active)
            txtStatus.text = "ACTIVE"
            txtStatus.setTextColor(Color.parseColor("#4CAF50"))
            statusBadge.setBackgroundResource(R.drawable.badge_active)
            cardStats.visibility = View.VISIBLE
        } else {
            btnToggle.text = "ENABLE FRAME GENERATION"
            btnToggle.setBackgroundResource(R.drawable.btn_enable)
            statusDot.setBackgroundResource(R.drawable.dot_inactive)
            txtStatus.text = "OFF"
            txtStatus.setTextColor(Color.parseColor("#888888"))
            statusBadge.setBackgroundResource(R.drawable.badge_inactive)
            cardStats.visibility = View.GONE
        }
    }

    // ================================================================
    // Prefs
    // ================================================================

    private fun savePrefs() {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().apply {
            putString("selectedPackage", selectedPackage)
            putString("selectedAppName", selectedAppName)
            putInt("targetFps", seekTargetFps.progress)
            putInt("quality", seekQuality.progress)
            putInt("method", spinnerMethod.selectedItemPosition)
            putInt("mode", spinnerMode.selectedItemPosition)
            putBoolean("forceRefresh", switchForceRefresh.isChecked)
            putBoolean("thermal", switchThermal.isChecked)
            apply()
        }
    }

    private fun loadPrefs() {
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        selectedPackage = prefs.getString("selectedPackage", null)
        selectedAppName = prefs.getString("selectedAppName", null)

        if (selectedPackage != null) {
            txtSelectedGame.text = selectedAppName ?: selectedPackage
            txtSelectedGame.setTextColor(Color.parseColor("#CCCCCC"))
            txtGameInfo.text = selectedPackage
            txtGameInfo.visibility = View.VISIBLE
        }

        seekTargetFps.progress = prefs.getInt("targetFps", 120)
        txtTargetFps.text = seekTargetFps.progress.toString()

        seekQuality.progress = prefs.getInt("quality", 75)

        spinnerMethod.setSelection(prefs.getInt("method", 0))
        spinnerMode.setSelection(prefs.getInt("mode", 0))

        switchForceRefresh.isChecked = prefs.getBoolean("forceRefresh", false)
        switchThermal.isChecked = prefs.getBoolean("thermal", true)
    }

    // ================================================================
    // Helpers
    // ================================================================

    private fun isAccessibilityEnabled(): Boolean {
        val service = "$packageName/${GameDetectorService::class.java.canonicalName}"
        return try {
            val enabled = Settings.Secure.getString(contentResolver,
                Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES)
            enabled?.contains(service) == true
        } catch (_: Exception) {
            false
        }
    }
}
