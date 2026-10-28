import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'screens/scan_screen.dart';
import 'services/api_server.dart';
import 'services/settings.dart';
import 'theme/app_theme.dart';
import 'widgets/glass_surface.dart';

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
          // Paint the ambient layer behind every route so the translucent
          // glass cards have real colour to reveal.
          builder: (context, child) => AppBackground(child: child ?? const SizedBox.shrink()),
          home: const ScanScreen(),
        ),
      ),
    );
  }
}
