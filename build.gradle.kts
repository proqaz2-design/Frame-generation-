buildscript {
    extra.apply {
        set("compileSdk", 34)
        set("minSdk", 28)
        set("targetSdk", 34)
        set("ndkVersion", "26.1.10909125")
    }
}

plugins {
    id("com.android.application") version "8.2.0" apply false
    id("org.jetbrains.kotlin.android") version "1.9.20" apply false
}
