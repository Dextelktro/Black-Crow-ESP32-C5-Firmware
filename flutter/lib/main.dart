import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'screens/scan_screen.dart';
import 'services/api_server.dart';
import 'services/settings.dart';
import 'services/transition_aware_observer.dart';
import 'theme/app_theme.dart';

/// One stable observer for the app's lifetime. It opens [UiTransitionGate]
/// while routes animate so BLE telemetry stops rebuilding whole screens during
/// a push/pop. Kept at file scope so the [MaterialApp] rebuilds (driven by the
/// [Settings] consumer) reuse the same instance instead of re-registering it.
final _transitionObserver = TransitionAwareObserver();

void main() {
  final settings = Settings();
  final api = ApiServer(settings);
  runApp(DeautherApp(settings: settings, api: api));
}

class DeautherApp extends StatelessWidget {
  const DeautherApp({super.key, required this.settings, required this.api});

  final Settings settings;
  final ApiServer api;

  @override
  Widget build(BuildContext context) {
    return MultiProvider(
      providers: [
        ChangeNotifierProvider.value(value: settings),
        ChangeNotifierProvider.value(value: api),
      ],
      child: Consumer<Settings>(
        builder: (context, s, _) => MaterialApp(
          title: 'Black Crow',
          debugShowCheckedModeBanner: false,
          themeMode: s.darkMode ? ThemeMode.dark : ThemeMode.light,
          darkTheme: buildDarkTheme(),
          theme: buildLightTheme(),
          navigatorObservers: [_transitionObserver],
          home: const ScanScreen(),
        ),
      ),
    );
  }
}
