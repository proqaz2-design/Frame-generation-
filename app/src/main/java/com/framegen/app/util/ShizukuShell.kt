package com.framegen.app.util

import android.content.pm.PackageManager
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import rikka.shizuku.Shizuku
import java.io.BufferedReader
import java.io.InputStreamReader

/**
 * ShizukuShell — executes shell commands with ADB-level privileges via Shizuku.
 *
 * This is what makes rootless Vulkan layer injection possible.
 * Shizuku provides shell (UID 2000) access which can modify
 * system settings like gpu_debug_layers.
 */
object ShizukuShell {

    private const val TAG = "ShizukuShell"

    data class Result(val exitCode: Int, val output: String, val error: String)

    /**
     * Check if Shizuku is available and authorized.
     */
    fun isAvailable(): Boolean {
        return try {
            Shizuku.pingBinder() &&
                Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED
        } catch (e: Exception) {
            Log.w(TAG, "Shizuku not available", e)
            false
        }
    }

    /**
     * Check if Shizuku binder is alive (installed and running).
     */
    fun isRunning(): Boolean {
        return try {
            Shizuku.pingBinder()
        } catch (e: Exception) {
            false
        }
    }

    /**
     * Request Shizuku permission from the user.
     */
    fun requestPermission(requestCode: Int) {
        try {
            Shizuku.requestPermission(requestCode)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to request Shizuku permission", e)
        }
    }

    /**
     * Execute a shell command via Shizuku with ADB-level privileges.
     * Uses Shizuku's binder to run the command as shell user (UID 2000).
     *
     * Since Shizuku.newProcess() is private in API 13.1.5,
     * we use reflection or the IShizukuService binder directly.
     */
    fun exec(command: String): Result {
        if (!isAvailable()) {
            return Result(-1, "", "Shizuku not available or not authorized")
        }

        return try {
            // Approach 1: Try Shizuku.newProcess via reflection
            // (it exists but is marked private in some builds)
            val process = try {
                val method = Shizuku::class.java.getDeclaredMethod(
                    "newProcess",
                    Array<String>::class.java,
                    Array<String>::class.java,
                    String::class.java
                )
                method.isAccessible = true
                method.invoke(null,
                    arrayOf("sh", "-c", command),
                    null,
                    null
                ) as Process
            } catch (reflectError: Exception) {
                Log.w(TAG, "Shizuku.newProcess reflection failed, trying binder", reflectError)

                // Approach 2: Try via Shizuku binder using hidden API bypass
                try {
                    val binderMethod = Shizuku::class.java.getDeclaredMethod("getBinder")
                    binderMethod.isAccessible = true
                    val binder = binderMethod.invoke(null)
                    if (binder != null) {
                        // IShizukuService.newProcess via AIDL
                        val serviceClass = Class.forName("rikka.shizuku.server.IShizukuService\$Stub")
                        val asInterface = serviceClass.getDeclaredMethod("asInterface",
                            android.os.IBinder::class.java)
                        val service = asInterface.invoke(null, binder)
                        val newProcessMethod = service!!.javaClass.getDeclaredMethod(
                            "newProcess",
                            Array<String>::class.java,
                            Array<String>::class.java,
                            String::class.java
                        )
                        val remoteProcess = newProcessMethod.invoke(service,
                            arrayOf("sh", "-c", command), null, null)

                        // Wrap in a Process-like object
                        val getInputStream = remoteProcess!!.javaClass.getMethod("getInputStream")
                        val getErrorStream = remoteProcess.javaClass.getMethod("getErrorStream")
                        val waitFor = remoteProcess.javaClass.getMethod("waitFor")

                        val stdout = BufferedReader(InputStreamReader(
                            getInputStream.invoke(remoteProcess) as java.io.InputStream)).readText()
                        val stderr = BufferedReader(InputStreamReader(
                            getErrorStream.invoke(remoteProcess) as java.io.InputStream)).readText()
                        val exitCode = waitFor.invoke(remoteProcess) as Int

                        return Result(exitCode, stdout.trim(), stderr.trim())
                    }
                    throw Exception("Binder is null")
                } catch (binderError: Exception) {
                    Log.w(TAG, "Shizuku binder approach failed", binderError)
                    // Last resort: plain Runtime.exec (will only work for non-privileged commands)
                    Runtime.getRuntime().exec(arrayOf("sh", "-c", command))
                }
            }

            val stdout = BufferedReader(InputStreamReader(process.inputStream)).readText()
            val stderr = BufferedReader(InputStreamReader(process.errorStream)).readText()
            val exitCode = process.waitFor()

            Log.d(TAG, "exec: $command → exit=$exitCode")
            if (stderr.isNotEmpty()) Log.w(TAG, "stderr: $stderr")

            Result(exitCode, stdout.trim(), stderr.trim())
        } catch (e: Exception) {
            Log.e(TAG, "exec failed: $command", e)
            Result(-1, "", e.message ?: "Unknown error")
        }
    }

    /**
     * Execute multiple commands sequentially. Stops on first failure.
     */
    fun execAll(commands: List<String>): Boolean {
        for (cmd in commands) {
            val result = exec(cmd)
            if (result.exitCode != 0) {
                Log.e(TAG, "Command failed: $cmd (exit=${result.exitCode}, err=${result.error})")
                return false
            }
        }
        return true
    }

    /**
     * Suspend version for coroutine contexts.
     */
    suspend fun execAsync(command: String): Result = withContext(Dispatchers.IO) {
        exec(command)
    }

    // ================================================================
    // GPU Debug Layer Management
    // ================================================================

    /**
     * Configure Android's gpu_debug_layers to inject our Vulkan layer
     * into a target game. This is the core rootless injection mechanism.
     *
     * After calling this, the next time [targetPackage] starts,
     * Android will load VkLayer_framegen.so from [ourPackage]'s
     * native library directory into the game's Vulkan instance.
     */
    fun injectLayer(targetPackage: String, ourPackage: String): Boolean {
        Log.i(TAG, "Injecting VkLayer into $targetPackage from $ourPackage")

        return execAll(listOf(
            "settings put global enable_gpu_debug_layers 1",
            "settings put global gpu_debug_app $targetPackage",
            "settings put global gpu_debug_layers VK_LAYER_FRAMEGEN_capture",
            "settings put global gpu_debug_layer_app $ourPackage"
        ))
    }

    /**
     * Remove the gpu_debug_layers settings (cleanup).
     */
    fun removeLayer(): Boolean {
        Log.i(TAG, "Removing GPU debug layer settings")

        return execAll(listOf(
            "settings put global enable_gpu_debug_layers 0",
            "settings delete global gpu_debug_app",
            "settings delete global gpu_debug_layers",
            "settings delete global gpu_debug_layer_app"
        ))
    }

    /**
     * Check if gpu_debug_layers is currently configured for a package.
     */
    fun getActiveTarget(): String? {
        val result = exec("settings get global gpu_debug_app")
        return if (result.exitCode == 0 && result.output != "null" && result.output.isNotEmpty()) {
            result.output
        } else {
            null
        }
    }

    /**
     * Grant WRITE_SECURE_SETTINGS permission to our app for direct
     * Settings.Global access (optional optimization).
     */
    fun grantSettingsPermission(ourPackage: String): Boolean {
        return exec("pm grant $ourPackage android.permission.WRITE_SECURE_SETTINGS").exitCode == 0
    }

    /**
     * Write a config value that the Vulkan layer can read.
     * The layer reads from system properties set via settings.
     */
    fun writeLayerConfig(key: String, value: String): Boolean {
        return exec("settings put global framegen_$key $value").exitCode == 0
    }

    /**
     * Write full layer configuration.
     */
    fun configureLayer(
        mode: Int = 1,
        quality: Float = 0.5f,
        targetFps: Int = 60
    ): Boolean {
        return execAll(listOf(
            "settings put global framegen_mode $mode",
            "settings put global framegen_quality $quality",
            "settings put global framegen_target_fps $targetFps",
            "settings put global framegen_enabled 1"
        ))
    }
}
