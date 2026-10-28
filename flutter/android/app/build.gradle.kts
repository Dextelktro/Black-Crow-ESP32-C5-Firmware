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
val releaseSigningPropertyKeys = listOf(
    "keyAlias",
    "keyPassword",
    "storeFile",
    "storePassword",
)

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
        // Unique Application ID for this fork. Kept distinct from upstream
        // deauther forks so that a differently-signed build of another app
        // cannot share this package name and make the install fail at 100%
        // with "App not installed". The internal `namespace` above is a
        // separate, build-only identifier and is intentionally left unchanged.
        applicationId = "com.blackcrow.deauther"
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
            enableV2Signing = true
            keystoreProperties.getProperty("keyAlias")?.let { keyAlias = it }
            keystoreProperties.getProperty("keyPassword")?.let { keyPassword = it }
            keystoreProperties.getProperty("storeFile")?.let { storeFile = file(it) }
            keystoreProperties.getProperty("storePassword")?.let { storePassword = it }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("release")
        }
    }
}

val validateReleaseSigning = tasks.register("validateReleaseSigning") {
    doLast {
        if (!keystorePropertiesFile.isFile) {
            throw GradleException(
                "Android release builds require android/key.properties with the permanent release keystore credentials.",
            )
        }
        val missingProperties = releaseSigningPropertyKeys.filter {
            keystoreProperties.getProperty(it).isNullOrBlank()
        }
        if (missingProperties.isNotEmpty()) {
            throw GradleException(
                "Missing Android release signing properties: ${missingProperties.joinToString()}",
            )
        }
        val keystoreFile = file(keystoreProperties.getProperty("storeFile"))
        if (!keystoreFile.isFile) {
            throw GradleException(
                "Android release keystore does not exist: $keystoreFile",
            )
        }
    }
}

tasks.configureEach {
    if (name != validateReleaseSigning.name && name.contains("Release", ignoreCase = true)) {
        dependsOn(validateReleaseSigning)
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
