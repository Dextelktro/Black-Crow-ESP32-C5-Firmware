package com.brito.deauther.deauther

import android.os.Build
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "deauther/platform")
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "androidSdkInt" -> result.success(Build.VERSION.SDK_INT)
                    "appInfo" -> {
                        val packageInfo = packageManager.getPackageInfo(packageName, 0)
                        val versionCode = if (Build.VERSION.SDK_INT >= 28) {
                            packageInfo.longVersionCode
                        } else {
                            @Suppress("DEPRECATION")
                            packageInfo.versionCode.toLong()
                        }
                        result.success(
                            mapOf(
                                "appVersion" to "${packageInfo.versionName ?: "Unknown"} ($versionCode)",
                                "androidVersion" to "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})",
                                "flutterVersion" to BuildConfig.FLUTTER_SDK_VERSION,
                            ),
                        )
                    }
                    else -> result.notImplemented()
                }
            }
    }
}
