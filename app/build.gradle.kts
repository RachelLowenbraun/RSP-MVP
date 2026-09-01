plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.rsp.timing"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "com.rsp.timing"
        // Redline Patch 7: clinical tier requires min API 33 (Android 13).
        // The M0 spike enforces the same floor so timing-API surface is stable.
        minSdk = 33
        targetSdk = 34
        versionCode = 1
        versionName = "0.1-m0"

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_PLATFORM=android-33"
                )
                cppFlags += listOf("-std=c++17", "-fno-exceptions", "-fno-rtti", "-Wall", "-Werror")
            }
        }

        ndk {
            // Modern 64-bit only; matches the clinical device allowlist expectation.
            abiFilters += listOf("arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Release IS the timing-validation build. Debug is only for verbose logs.
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
        debug {
            isMinifyEnabled = false
            isDebuggable = true
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        viewBinding = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.4")
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.8.4")
    // No third-party analytics / crash SDKs. Zero. Per Redline Patch 10 / spec §15.3.
}
