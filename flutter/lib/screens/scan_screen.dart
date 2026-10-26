import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:provider/provider.dart';

import '../services/api_server.dart';
import '../services/device_controller.dart';
import '../services/nus_client.dart';
import 'device_screen.dart';
import 'settings_screen.dart';

class ScanScreen extends StatefulWidget {
  const ScanScreen({super.key});

  @override
  State<ScanScreen> createState() => _ScanScreenState();
}

class _ScanScreenState extends State<ScanScreen> {
  static const MethodChannel _platform = MethodChannel('deauther/platform');

  final NusClient _client = NusClient();
  StreamSubscription<List<ScanResult>>? _resultsSub;
  StreamSubscription<bool>? _scanningSub;
  StreamSubscription<BluetoothAdapterState>? _adapterSub;
  List<ScanResult> _results = [];
  List<ScanResult>? _pendingResults;
  bool _scanning = false;
  bool _scanStarting = false;
  bool _connecting = false;
  bool _autoRescan = true;
  Timer? _rescanTimer;
  Timer? _resultUpdateTimer;
  String? _error;

  @override
  void initState() {
    super.initState();
    _scanningSub = FlutterBluePlus.isScanning.listen(
      (s) {
        if (!mounted) return;
        final wasScanning = _scanning;
        setState(() => _scanning = s);
        if (s) _scanStarting = false;
        // When a scan cycle ends naturally, schedule the next one in 5 seconds.
        if (wasScanning && !s && !_connecting && _autoRescan) {
          _rescanTimer?.cancel();
          _resultUpdateTimer?.cancel();
          _rescanTimer = Timer(const Duration(seconds: 5), () {
            if (mounted && !_connecting && _autoRescan) _maybeStartScan();
          });
        }
      },
      onError: (Object error, StackTrace stackTrace) {
        if (!mounted) return;
        setState(() => _error = 'Bluetooth scan state failed: $error');
        debugPrint('Bluetooth scan state failed: $error\n$stackTrace');
      },
    );
    // Kick off a scan when BT adapter turns on (handles the case where BT was
    // off at startup and the user enables it via the system dialog).
    _adapterSub = FlutterBluePlus.adapterState.listen(
      (state) {
        if (state == BluetoothAdapterState.on &&
            !_scanning &&
            !_scanStarting &&
            !_connecting &&
            _autoRescan &&
            mounted) {
          _maybeStartScan();
        }
      },
      onError: (Object error, StackTrace stackTrace) {
        if (!mounted) return;
        setState(() => _error = 'Bluetooth adapter state failed: $error');
        debugPrint('Bluetooth adapter state failed: $error\n$stackTrace');
      },
    );
    _maybeStartScan();
  }

  @override
  void dispose() {
    _rescanTimer?.cancel();
    _resultUpdateTimer?.cancel();
    _resultsSub?.cancel();
    _scanningSub?.cancel();
    _adapterSub?.cancel();
    unawaited(_client.stopScan().catchError((Object error) {
      debugPrint('BLE scan cleanup failed: $error');
    }));
    super.dispose();
  }

  Future<void> _maybeStartScan() async {
    if (_scanning || _scanStarting || _connecting) return;
    _scanStarting = true;
    try {
      if (!await _ensurePermissions() || !mounted) return;
      if (!await _ensureAdapterOn() || !mounted) return;
      _startScan();
    } catch (e) {
      if (mounted) setState(() => _error = 'BLE scan setup failed: $e');
    } finally {
      if (!_scanning) _scanStarting = false;
    }
  }

  Future<bool> _ensurePermissions() async {
    if (!Platform.isAndroid) return true;
    try {
      final sdkInt = await _platform.invokeMethod<int>('androidSdkInt');
      if (sdkInt == null) throw StateError('Unable to determine Android version');
      final permissions = sdkInt >= 31
          ? [Permission.bluetoothScan, Permission.bluetoothConnect]
          : [Permission.locationWhenInUse];
      final statuses = await permissions.request();
      final ok = statuses.values.every((s) => s.isGranted || s.isLimited);
      if (!ok && mounted) {
        setState(() => _error = 'Bluetooth scan permission denied. Allow it in app settings.');
      }
      if (ok && sdkInt < 31) {
        final locationService = await Permission.location.serviceStatus;
        if (!locationService.isEnabled) {
          if (mounted) {
            setState(() => _error = 'Enable Location services to scan for BLE devices.');
          }
          return false;
        }
      }
      return ok;
    } catch (e) {
      if (mounted) setState(() => _error = 'Unable to check Bluetooth permissions: $e');
      return false;
    }
  }

  Future<bool> _ensureAdapterOn() async {
    final state = await FlutterBluePlus.adapterState.first;
    if (state == BluetoothAdapterState.on) return true;
    if (Platform.isAndroid) {
      try {
        await FlutterBluePlus.turnOn();
        // turnOn() shows the system dialog but doesn't block until BT is on.
        // The adapterState listener in initState will call _maybeStartScan when
        // it actually transitions to on; bail here so we don't double-scan.
        return false;
      } catch (e) {
        if (mounted) setState(() => _error = 'Unable to enable Bluetooth: $e');
        return false;
      }
    }
    if (mounted) setState(() => _error = 'Bluetooth is off — enable it and retry.');
    return false;
  }

  void _startScan() {
    _rescanTimer?.cancel();
    setState(() {
      _error = null;
      _results = [];
    });
    _pendingResults = null;
    _resultUpdateTimer?.cancel();
    _resultUpdateTimer = null;
    _resultsSub?.cancel();
    _resultsSub = _client.scan().listen((rs) {
      _pendingResults = rs;
      if (_resultUpdateTimer != null) return;
      _resultUpdateTimer = Timer(const Duration(milliseconds: 200), () {
        _resultUpdateTimer = null;
        if (mounted) setState(() => _results = _pendingResults ?? []);
      });
    }, onError: (e) {
      _scanStarting = false;
      _resultUpdateTimer?.cancel();
      _resultUpdateTimer = null;
      if (mounted) {
        setState(() => _error = 'BLE scan failed: $e');
      }
    });
  }

  Future<void> _connect(BluetoothDevice device) async {
    if (_connecting) return;
    _rescanTimer?.cancel();
    setState(() {
      _error = null;
      _connecting = true;
    });
    var dialogVisible = true;
    showDialog(
      context: context,
      barrierDismissible: false,
      builder: (_) => const Center(child: CircularProgressIndicator()),
    );
    DeviceController? controller;
    ApiServer? api;
    var apiAttached = false;
    try {
      final resultsSub = _resultsSub;
      _resultsSub = null;
      await resultsSub?.cancel();
      final conn = await _client.connect(device);
      controller = DeviceController(conn);
      if (!mounted) {
        await conn.disconnect();
        return;
      }
      api = context.read<ApiServer>();
      api.attach(controller);
      apiAttached = true;
      Navigator.of(context).pop(); // dismiss spinner
      dialogVisible = false;
      await Navigator.of(context).push(
        MaterialPageRoute(builder: (_) => DeviceScreen(controller: controller!)),
      );
      // Await the full BLE teardown so the ESP32 receives a clean disconnect
      // and restarts advertising before we scan again.
      await controller.conn.disconnect();
    } catch (e) {
      if (mounted) {
        if (dialogVisible) {
          Navigator.of(context).pop();
          dialogVisible = false;
        }
        setState(() => _error = 'BLE connection failed: $e');
      }
    } finally {
      if (apiAttached) api!.detach();
      if (dialogVisible && mounted) {
        Navigator.of(context).pop();
        dialogVisible = false;
      }
      controller?.dispose();
      if (mounted) {
        setState(() => _connecting = false);
        // Give the BLE stack and the ESP32 a moment to settle before rescanning.
        await Future.delayed(const Duration(seconds: 2));
        if (mounted) _maybeStartScan();
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Black Crow'),
        actions: [
          IconButton(
            icon: Icon(_scanning ? Icons.stop : Icons.refresh),
            tooltip: _scanning ? 'Stop scan' : 'Scan',
            onPressed: () {
              if (_scanning) {
                _autoRescan = false;
                _rescanTimer?.cancel();
                unawaited(_client.stopScan().catchError((Object error) {
                  if (mounted) setState(() => _error = 'Unable to stop BLE scan: $error');
                }));
              } else {
                _autoRescan = true;
                _maybeStartScan();
              }
            },
          ),
          PopupMenuButton<String>(
            icon: const Icon(Icons.menu),
            onSelected: (value) {
              if (value == 'settings') {
                Navigator.of(context).push(
                  MaterialPageRoute(builder: (_) => const SettingsScreen()),
                );
              }
            },
            itemBuilder: (_) => const [
              PopupMenuItem(value: 'settings', child: Text('Settings')),
            ],
          ),
        ],
      ),
      body: Column(
        children: [
          if (_scanning) const LinearProgressIndicator(),
          if (_error != null)
            Container(
              width: double.infinity,
              color: Colors.red.shade100,
              padding: const EdgeInsets.all(12),
              child: Text(_error!, style: const TextStyle(color: Colors.red)),
            ),
          Expanded(
            child: _results.isEmpty
                ? Center(
                    child: Text(
                      _scanning ? 'Scanning…' : 'No devices found',
                      style: Theme.of(context).textTheme.bodyMedium,
                    ),
                  )
                : ListView.separated(
                    itemCount: _results.length,
                    separatorBuilder: (_, _) => const Divider(height: 1),
                    itemBuilder: (context, i) => _resultTile(_results[i]),
                  ),
          ),
        ],
      ),
    );
  }

  Widget _resultTile(ScanResult r) {
    final name = r.advertisementData.advName.isNotEmpty
        ? r.advertisementData.advName
        : (r.device.platformName.isNotEmpty ? r.device.platformName : '(unnamed)');
    return ListTile(
      leading: const Icon(Icons.bluetooth),
      title: Text(name),
      subtitle: Text('${r.device.remoteId}\n${r.rssi} dBm'),
      isThreeLine: true,
      onTap: () => _connect(r.device),
    );
  }
}
