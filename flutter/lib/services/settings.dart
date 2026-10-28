import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

/// Physical board the connected firmware is running on. Selected by the user
/// (the firmware does not report it); drives board-specific UI such as the
/// pin map on the Hardware Peripheral screen and the monochrome vs RGB LED
/// controls. Defaults to [devkit] so existing behaviour is unchanged.
enum BoardVariant {
  devkit,
  xiao;

  String get id => this == BoardVariant.xiao ? 'xiao' : 'devkit';

  String get label =>
      this == BoardVariant.xiao ? 'XIAO ESP32-C5' : 'ESP32-C5 DevKitC-1';

  /// true when the onboard status LED is a single-colour LED driven by PWM
  /// (intensity only), false for the DevKit's addressable WS2812 RGB LED.
  bool get ledIsMonochrome => this == BoardVariant.xiao;

  static BoardVariant fromId(String? id) =>
      id == 'xiao' ? BoardVariant.xiao : BoardVariant.devkit;
}

class Settings extends ChangeNotifier {
  static const _defaultPort = 7331;

  int _apiPort = _defaultPort;
  bool _apiEnabled = true;
  bool _darkMode = true; // default: dark theme
  BoardVariant _board = BoardVariant.devkit;
  String? _loadIssue;

  int get apiPort => _apiPort;
  bool get apiEnabled => _apiEnabled;
  bool get darkMode => _darkMode;
  BoardVariant get board => _board;
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
      final board = map['board'];
      if (port != null && (port is! int || port < 1024 || port > 65535)) {
        throw const FormatException('apiPort must be between 1024 and 65535');
      }
      if (enabled != null && enabled is! bool) {
        throw const FormatException('apiEnabled must be a boolean');
      }
      if (dark != null && dark is! bool) {
        throw const FormatException('darkMode must be a boolean');
      }
      if (board != null && (board is! String || (board != 'devkit' && board != 'xiao'))) {
        throw const FormatException('board must be "devkit" or "xiao"');
      }
      _apiPort = port as int? ?? _defaultPort;
      _apiEnabled = enabled as bool? ?? true;
      _darkMode = dark as bool? ?? true;
      _board = BoardVariant.fromId(board as String?);
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
      'board': _board.id,
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

  Future<void> setBoard(BoardVariant board) async {
    if (_board == board) return;
    final previous = _board;
    _board = board;
    notifyListeners();
    try {
      await _persist();
    } catch (_) {
      _board = previous;
      notifyListeners();
      rethrow;
    }
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
