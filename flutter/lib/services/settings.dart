import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

class Settings extends ChangeNotifier {
  static const _defaultPort = 7331;

  int _apiPort = _defaultPort;
  bool _apiEnabled = true;
  bool _darkMode = true; // default: dark theme
  String? _loadIssue;

  int get apiPort => _apiPort;
  bool get apiEnabled => _apiEnabled;
  bool get darkMode => _darkMode;
  String? get loadIssue => _loadIssue;

  late final File _file;

  Settings() {
    final home = Platform.environment['HOME'] ??
        Platform.environment['USERPROFILE'] ??
        '.';
    _file = File('$home/.config/deauther/settings.json');
    _load();
  }

  Future<void> _load() async {
    try {
      final text = await _file.readAsString();
      final map = jsonDecode(text) as Map<String, dynamic>;
      final port = map['apiPort'];
      final enabled = map['apiEnabled'];
      final dark = map['darkMode'];
      if (port != null && (port is! int || port < 1024 || port > 65535)) {
        throw const FormatException('apiPort must be between 1024 and 65535');
      }
      if (enabled != null && enabled is! bool) {
        throw const FormatException('apiEnabled must be a boolean');
      }
      if (dark != null && dark is! bool) {
        throw const FormatException('darkMode must be a boolean');
      }
      _apiPort = port as int? ?? _defaultPort;
      _apiEnabled = enabled as bool? ?? true;
      _darkMode = dark as bool? ?? true;
      notifyListeners();
    } catch (error, stackTrace) {
      // Missing file/directory on first run is not an error. PathNotFoundException
      // covers both ERROR_FILE_NOT_FOUND (2) and ERROR_PATH_NOT_FOUND (3) on
      // Windows as well as ENOENT on POSIX.
      if (error is PathNotFoundException) {
        return;
      }
      if (error is FileSystemException &&
          (error.osError?.errorCode == 2 || error.osError?.errorCode == 3)) {
        return;
      }
      _loadIssue = 'Unable to load saved settings: $error';
      debugPrint('Settings load failed: $error\n$stackTrace');
      notifyListeners();
    }
  }

  Future<void> _persist() async {
    await _file.parent.create(recursive: true);
    await _file.writeAsString(jsonEncode({
      'apiPort': _apiPort,
      'apiEnabled': _apiEnabled,
      'darkMode': _darkMode,
    }));
  }

  Future<void> setApiPort(int port) async {
    if (port < 1024 || port > 65535) {
      throw ArgumentError.value(port, 'port', 'Must be between 1024 and 65535');
    }
    final previous = _apiPort;
    _apiPort = port;
    try {
      await _persist();
    } catch (_) {
      _apiPort = previous;
      rethrow;
    }
    notifyListeners();
  }

  Future<void> setApiEnabled(bool enabled) async {
    final previous = _apiEnabled;
    _apiEnabled = enabled;
    try {
      await _persist();
    } catch (_) {
      _apiEnabled = previous;
      rethrow;
    }
    notifyListeners();
  }

  Future<void> setDarkMode(bool dark) async {
    if (_darkMode == dark) return;
    final previous = _darkMode;
    _darkMode = dark;
    notifyListeners();
    try {
      await _persist();
    } catch (_) {
      _darkMode = previous;
      notifyListeners();
      rethrow;
    }
  }
}
