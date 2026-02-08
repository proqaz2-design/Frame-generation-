# ProGuard rules for FrameGen

# Keep JNI methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep FrameGenEngine
-keep class com.framegen.app.engine.** { *; }

# Keep Shizuku
-keep class rikka.shizuku.** { *; }

# Keep Vulkan layer name references
-keepclassmembers class * {
    @android.webkit.JavascriptInterface <methods>;
}
