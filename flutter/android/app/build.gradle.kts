import com.android.build.api.dsl.ApplicationExtension
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.io.File
import java.util.Properties

plugins {
    id("com.android.application")
    id("kotlin-android")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

val flutterSdkPath = Properties().apply {
    rootProject.file("local.properties").inputStream().use { load(it) }
}.getProperty("flutter.sdk")
    ?: throw GradleException("flutter.sdk is not set in android/local.properties")
val flutterVersionFile = File(flutterSdkPath, "bin/cache/flutter.version.json")
val flutterSdkVersion = Regex("\"frameworkVersion\"\\s*:\\s*\"([^\"]+)\"")
    .find(flutterVersionFile.readText())
    ?.groupValues
    ?.get(1)
    ?: throw GradleException("Unable to read Flutter SDK version from $flutterVersionFile")

extensions.configure<ApplicationExtension> {
    namespace = "com.brito.deauther.deauther"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        // TODO: Specify your own unique Application ID (https://developer.android.com/studio/build/application-id.html).
        applicationId = "com.brito.deauther.deauther"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        minSdk = flutter.minSdkVersion
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName
        buildConfigField("String", "FLUTTER_SDK_VERSION", "\"$flutterSdkVersion\"")
    }

    buildTypes {
        release {
            // TODO: Add your own signing config for the release build.
            // Signing with the debug keys for now, so `flutter run --release` works.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}
