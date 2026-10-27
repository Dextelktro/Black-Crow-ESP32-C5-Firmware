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

// Release signing credentials, loaded from key.properties (git-ignored; CI
// writes this file from secrets before building — see release.yml)
val keystoreProperties = Properties()
val keystorePropertiesFile = rootProject.file("key.properties")
if (keystorePropertiesFile.exists()) {
    keystoreProperties.load(keystorePropertiesFile.inputStream())
}

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

    signingConfigs {
        create("release") {
            if (keystorePropertiesFile.exists()) {
                keyAlias = keystoreProperties["keyAlias"] as String
                keyPassword = keystoreProperties["keyPassword"] as String
                storeFile = file(keystoreProperties["storeFile"] as String)
                storePassword = keystoreProperties["storePassword"] as String
            }
        }
    }

    buildTypes {
        release {
            // Real release signing when key.properties is present (CI, or a
            // local release build); falls back to debug signing only when
            // it's missing (e.g. a contributor's local checkout), so
            // `flutter run --release` still works out of the box for them.
            signingConfig = if (keystorePropertiesFile.exists()) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
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
