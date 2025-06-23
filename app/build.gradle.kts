import org.apache.tools.ant.util.JavaEnvUtils.VERSION_11

plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.jxj.CompileFfmpeg"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.jxj.CompileFfmpeg"
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        
        // NDK配置
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
        
        // 强制使用NDK 21来避免GWP-ASan问题
        ndkVersion = "21.4.7075529"
        
        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++11", "-fexceptions")
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",  // 修改为 c++_shared
                    "-DANDROID_PLATFORM=android-24",
                    "-DANDROID_ARM_MODE=arm",
                    "-DANDROID_ARM_NEON=TRUE",
                    "-DANDROID_CPP_FEATURES=exceptions",
                    "-DCMAKE_BUILD_TYPE=Release"
                )
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        viewBinding = true
    }

    // 移除 jniLibs 配置，让 CMake 处理 FFmpeg 库
    // sourceSets {
    //     getByName("main") {
    //         jniLibs {
    //             srcDirs("src/main/cpp/ffmpeg")
    //         }
    //     }
    // }
}

// FFmpeg 库文件已经在正确位置，不需要复制任务

dependencies {
    implementation(libs.appcompat)
    implementation(libs.material)
    implementation(libs.constraintlayout)
    testImplementation(libs.junit)
    androidTestImplementation(libs.ext.junit)
    androidTestImplementation(libs.espresso.core)
}