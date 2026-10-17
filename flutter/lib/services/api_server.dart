import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

import 'device_controller.dart';
import 'settings.dart';

class ApiServer extends ChangeNotifier {
  ApiServer(this._settings) {
    _settings.addListener(_onSettingsChanged);
    _onSettingsChanged();
  }

  final Settings _settings;
  DeviceController? _ctrl;
  HttpServer? _server;
  Future<void> _lifecycleQueue = Future<void>.value();
  int? _boundPort;
  String? _error;
  bool _disposed = false;

  bool get isRunning => _server != null;
  int? get boundPort => _boundPort;
  String? get error => _error;
  int get port => _settings.apiPort;

  void attach(DeviceController ctrl) => _ctrl = ctrl;
  void detach() => _ctrl = null;

  void _onSettingsChanged() {
    if (_disposed) return;
    _lifecycleQueue = _lifecycleQueue.then((_) => _applySettings()).catchError((
      Object error,
      StackTrace stackTrace,
    ) {
      _setError('API server failed: $error');
      debugPrint('API server lifecycle failed: $error\n$stackTrace');
    });
  }

  Future<void> _applySettings() async {
    if (_disposed) return;
    if (!_settings.apiEnabled) {
      await _stopServer();
      _setError(null);
      return;
    }
    if (_server != null && _boundPort == _settings.apiPort) return;

    await _stopServer();
    final server = await HttpServer.bind(
      InternetAddress.loopbackIPv4,
      _settings.apiPort,
    );
    if (_disposed) {
      await server.close(force: true);
      return;
    }
    _server = server;
    _boundPort = server.port;
    server.listen(
      _handle,
      onError: (Object error, StackTrace stackTrace) {
        _setError('API server stream failed: $error');
        debugPrint('API server stream failed: $error\n$stackTrace');
      },
      onDone: () {
        if (!identical(_server, server)) return;
        _server = null;
        _boundPort = null;
        _setError('API server stopped unexpectedly');
      },
    );
    _setError(null);
    if (!_disposed) notifyListeners();
  }

  Future<void> _stopServer() async {
    final server = _server;
    _server = null;
    _boundPort = null;
    await server?.close(force: true);
  }

  void _setError(String? error) {
    if (_error == error) return;
    _error = error;
    if (!_disposed) notifyListeners();
  }

  Future<void> _handle(HttpRequest req) async {
    req.response.headers.contentType = ContentType.json;
    req.response.headers.set('Access-Control-Allow-Origin', '*');

    final ctrl = _ctrl;
    final path = req.uri.path;
    final method = req.method;

    try {
      if (path == '/' && method == 'GET') {
        await _ok(req, {
          'status': 'ok',
          'connected': ctrl != null && !ctrl.connectionLost,
        });
        return;
      }
      if (ctrl == null) {
        await _err(req, 503, 'no device connected');
        return;
      }
      switch ('$method $path') {
        case 'GET /device':
          await _ok(req, _deviceJson(ctrl));
          return;
        case 'GET /aps':
          await _ok(req, {
            'aps': ctrl.aps
                .map(
                  (a) => {
                    'idx': a.idx,
                    'channel': a.channel,
                    'band': a.is5ghz ? '5GHz' : '2.4GHz',
                    'rssi': a.rssi,
                    'bssid': a.bssid,
                    'ssid': a.ssid,
                  },
                )
                .toList(),
          });
          return;
        case 'GET /log':
          await _ok(req, {'log': ctrl.rawLog});
          return;
        case 'POST /scan':
          await ctrl.scanWifi();
          await _ok(req, {'ok': true});
          return;
        case 'POST /attack/start':
          await ctrl.startAttack();
          await _ok(req, {'ok': true});
          return;
        case 'POST /attack/stop':
          await ctrl.stopAttack();
          await _ok(req, {'ok': true});
          return;
        case 'POST /nuke':
          final body = await _readJsonObject(req);
          final secs = body['secs'];
          if (secs != null && secs is! int) {
            await _err(req, 400, 'secs must be an integer');
            return;
          }
          await ctrl.nuke(secs as int? ?? 30);
          await _ok(req, {'ok': true});
          return;
        case 'POST /cmd':
          final body = await _readJsonObject(req);
          final cmd = body['cmd'];
          if (cmd is! String || cmd.trim().isEmpty) {
            await _err(req, 400, 'cmd must be a non-empty string');
            return;
          }
          await ctrl.send(cmd);
          await _ok(req, {'ok': true});
          return;
        default:
          await _err(req, 404, 'not found');
          return;
      }
    } on FormatException catch (error) {
      await _err(req, 400, 'invalid JSON: ${error.message}');
    } catch (e) {
      await _err(req, 500, '$e');
    }
  }

  Map<String, dynamic> _deviceJson(DeviceController ctrl) => {
    'connected': !ctrl.connectionLost,
    'attacking': ctrl.attacking,
    'attackMode': ctrl.attackMode,
    'scanning': ctrl.scanning,
    'selectedIdxs': ctrl.selectedIdxs.toList(),
    'apCount': ctrl.aps.length,
    'chipTemperatureC': ctrl.chipTemperatureC,
    'chipTemperatureUpdatedAt': ctrl.chipTemperatureUpdatedAt
        ?.toIso8601String(),
  };

  Future<String> _readBody(HttpRequest req) async {
    final bytes = await req.fold<List<int>>([], (a, b) => a..addAll(b));
    return utf8.decode(bytes);
  }

  Future<Map<String, dynamic>> _readJsonObject(HttpRequest req) async {
    final decoded = jsonDecode(await _readBody(req));
    if (decoded is! Map<String, dynamic>) {
      throw const FormatException('request body must be a JSON object');
    }
    return decoded;
  }

  Future<void> _ok(HttpRequest req, Map<String, dynamic> data) async {
    req.response
      ..statusCode = 200
      ..write(jsonEncode(data));
    await req.response.close();
  }

  Future<void> _err(HttpRequest req, int code, String msg) async {
    req.response
      ..statusCode = code
      ..write(jsonEncode({'error': msg}));
    await req.response.close();
  }

  @override
  void dispose() {
    _settings.removeListener(_onSettingsChanged);
    _disposed = true;
    final server = _server;
    _server = null;
    _boundPort = null;
    if (server != null) {
      unawaited(
        server.close(force: true).then<void>((_) {}).catchError((
          Object error,
          StackTrace stackTrace,
        ) {
          debugPrint('API server close failed: $error\n$stackTrace');
        }),
      );
    }
    super.dispose();
  }
}
