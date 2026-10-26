import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'screens/scan_screen.dart';
import 'services/api_server.dart';
import 'services/settings.dart';

void main() {
  final settings = Settings();
  final api = ApiServer(settings);
  runApp(DeautherApp(settings: settings, api: api));
}

class DeautherApp extends StatelessWidget {
  const DeautherApp({super.key, required this.settings, required this.api});

  final Settings settings;
  final ApiServer api;

  static const _canvas = Color(0xFF07110F);
  static const _neutralOutline = Color(0xFF344640);

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
          darkTheme: ThemeData(
            colorScheme: ColorScheme.fromSeed(
              seedColor: Colors.tealAccent,
              brightness: Brightness.dark,
            ).copyWith(
              surface: _canvas,
              outline: const Color(0xFF65736E),
              outlineVariant: _neutralOutline,
            ),
            scaffoldBackgroundColor: _canvas,
            useMaterial3: true,
          ),
          theme: ThemeData(
            colorScheme: ColorScheme.fromSeed(
              seedColor: Colors.teal,
              brightness: Brightness.light,
            ).copyWith(
              outline: const Color(0xFF65736E),
              outlineVariant: _neutralOutline,
            ),
            useMaterial3: true,
          ),
          home: const ScanScreen(),
        ),
      ),
    );
  }
}
