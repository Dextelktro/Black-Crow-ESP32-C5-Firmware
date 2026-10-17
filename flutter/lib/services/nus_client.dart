// Nordic UART Service (NUS) BLE client.
//
// Pairs with the firmware's transport_ble.c GATT layout:
//   Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX char: 6E400002-…   (write / write-no-rsp — host → device)
//   TX char: 6E400003-…   (notify              — device → host)
//
// Public surface:
//   - scan()              streams discovered peripherals advertising the NUS
//   - connect(device)     hooks up TX/RX, returns Stream<String> of incoming
//                         text and a `send()` to write commands
//   - NusConnection.disconnect()       graceful tear-down
//   - NusConnection.disconnected       Future that completes when peer drops

import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

class NusUuids {
  static final Guid service = Guid('6e400001-b5a3-f393-e0a9-e50e24dcca9e');
  static final Guid rx      = Guid('6e400002-b5a3-f393-e0a9-e50e24dcca9e');
  static final Guid tx      = Guid('6e400003-b5a3-f393-e0a9-e50e24dcca9e');
}

class NusConnection {
  NusConnection._({
    required this.device,
    required BluetoothCharacteristic rx,
    required this.incoming,
    required this.disconnected,
    required Future<void> Function() doDisconnect,
  })  : _rx = rx,
        _doDisconnect = doDisconnect;

  final BluetoothDevice device;
  final BluetoothCharacteristic _rx;

  /// Stream of UTF-8 decoded notifications from the TX characteristic.
  /// Only emits NEW notifications (not the cached `lastValueStream`), so
  /// a fresh connection never sees stale data from a previous session.
  final Stream<String> incoming;

  /// Resolves when the BLE link drops for any reason — graceful disconnect,
  /// peer drop, RF loss. Use this to drive UI state transitions instead of
  /// polling `device.isConnected`.
  final Future<void> disconnected;

  final Future<void> Function() _doDisconnect;
  bool _disconnectCalled = false;
  Future<void> _writeQueue = Future<void>.value();

  /// Sends a line to the device. Writes are serialized so commands issued
  /// during setup cannot collide in the platform GATT queue.
  Future<void> send(String text) {
    if (_disconnectCalled) {
      return Future<void>.error(StateError('BLE connection is closing'));
    }
    if (!text.endsWith('\n')) text = '$text\n';
    final bytes = utf8.encode(text);
    if (bytes.length > 255) {
      return Future<void>.error(
          ArgumentError('BLE CLI commands must not exceed 254 bytes'));
    }
    final operation = _writeQueue.then((_) async {
      if (!device.isConnected) {
        throw StateError('BLE device disconnected before command was sent');
      }
      final mtu = device.mtuNow;
      final maxPayload = mtu > 3 ? mtu - 3 : 20;
      const withoutResponse = false;
      try {
        for (var offset = 0; offset < bytes.length; offset += maxPayload) {
          final remaining = bytes.length - offset;
          final end = offset + (remaining < maxPayload ? remaining : maxPayload);
          await _rx.write(
            bytes.sublist(offset, end),
            withoutResponse: withoutResponse,
          ).timeout(const Duration(seconds: 8));
        }
      } catch (error, stackTrace) {
        if (device.isConnected) {
          try {
            await device.disconnect().timeout(const Duration(seconds: 4));
          } catch (cleanupError) {
            debugPrint('BLE disconnect after failed write failed: $cleanupError');
          }
        }
        Error.throwWithStackTrace(error, stackTrace);
      }
    });
    _writeQueue = operation.catchError((Object _) {});
    return operation;
  }

  Future<void> disconnect() async {
    if (_disconnectCalled) return;
    _disconnectCalled = true;
    await _writeQueue;
    await _doDisconnect();
  }
}

class NusClient {
  StreamSubscription<List<ScanResult>>? _scanSub;
  int _scanGeneration = 0;

  /// Streams scan results filtered to peripherals advertising the NUS service
  /// or named like our firmware. Caller is expected to call [stopScan] when
  /// done. Safe to call multiple times.
  Stream<List<ScanResult>> scan({Duration timeout = const Duration(seconds: 8)}) {
    final controller = StreamController<List<ScanResult>>();
    final generation = ++_scanGeneration;
    unawaited(_startScan(controller, generation, timeout));
    controller.onCancel = () async {
      try {
        if (generation == _scanGeneration) await stopScan();
      } catch (error) {
        debugPrint('BLE scan cleanup failed: $error');
      } finally {
        if (!controller.isClosed) await controller.close();
      }
    };
    return controller.stream;
  }

  Future<void> _startScan(
    StreamController<List<ScanResult>> controller,
    int generation,
    Duration timeout,
  ) async {
    try {
      await _stopActiveScan();
    } catch (error, stackTrace) {
      await _failScan(controller, error, stackTrace);
      return;
    }
    if (generation != _scanGeneration || controller.isClosed) return;

    _scanSub = FlutterBluePlus.onScanResults.listen(
      (results) {
        final devices = <String, ScanResult>{};
        for (final result in results) {
          if (_looksLikeDevice(result)) {
            devices[result.device.remoteId.str] = result;
          }
        }
        if (!controller.isClosed) controller.add(devices.values.toList());
      },
      onError: (Object error, StackTrace stackTrace) {
        if (!controller.isClosed) controller.addError(error, stackTrace);
      },
    );

    try {
      await FlutterBluePlus.startScan(
        timeout: timeout,
        withServices: Platform.isAndroid ? [NusUuids.service] : [],
        androidScanMode: AndroidScanMode.lowLatency,
      );
    } catch (error, stackTrace) {
      if (!Platform.isAndroid || generation != _scanGeneration) {
        if (generation == _scanGeneration) {
          await _failScan(controller, error, stackTrace);
        }
        return;
      }

      debugPrint(
          'BLE service-filter scan failed; retrying without UUID filter: $error');
      try {
        await FlutterBluePlus.startScan(timeout: timeout);
      } catch (fallbackError, fallbackStackTrace) {
        if (generation == _scanGeneration) {
          await _failScan(controller, fallbackError, fallbackStackTrace);
        }
      }
    }
  }

  Future<void> _failScan(
    StreamController<List<ScanResult>> controller,
    Object error,
    StackTrace stackTrace,
  ) async {
    await _scanSub?.cancel();
    _scanSub = null;
    if (!controller.isClosed) {
      controller
        ..addError(error, stackTrace)
        ..close();
    }
  }

  Future<void> stopScan() async {
    _scanGeneration++;
    await _stopActiveScan();
  }

  Future<void> _stopActiveScan() async {
    try {
      if (FlutterBluePlus.isScanningNow) {
        await FlutterBluePlus
            .stopScan()
            .timeout(const Duration(seconds: 5));
      }
    } finally {
      await _scanSub?.cancel();
      _scanSub = null;
    }
  }

  bool _looksLikeDevice(ScanResult r) {
    final uuids = r.advertisementData.serviceUuids.map((u) => u.str.toLowerCase()).toList();
    if (uuids.contains(NusUuids.service.str.toLowerCase())) return true;
    final name = (r.advertisementData.advName.isNotEmpty
        ? r.advertisementData.advName
        : r.device.platformName);
    final normalizedName = name.toLowerCase();
    return normalizedName.contains('radio c5') ||
        normalizedName.contains('deauther') ||
        normalizedName.contains('esp32c5') ||
        normalizedName.startsWith('c5-d');
  }

  /// Connect, discover services, subscribe to TX notifications, return a
  /// [NusConnection] exposing the bidirectional channel.
  ///
  /// Safe to call against the same `device` repeatedly — any prior link is
  /// torn down and the GATT cache invalidated (Android) before re-opening.
  Future<NusConnection> connect(BluetoothDevice device) async {
    await stopScan();

    // If the platform still thinks we're connected from a prior session,
    // tear it down first. flutter_blue_plus will otherwise return immediately
    // from connect() with a stale handle and service discovery will fail.
    if (device.isConnected) {
      await device.disconnect().timeout(const Duration(seconds: 4));
      await _waitForState(device, BluetoothConnectionState.disconnected,
          const Duration(seconds: 3));
    }

    // Android caches GATT services per-device. If the firmware was rebuilt
    // (handles renumbered) the cache is stale and discoverServices returns
    // garbage. Force-clear before connecting.
    if (Platform.isAndroid) {
      try {
        await device
            .clearGattCache()
            .timeout(const Duration(seconds: 4));
      } catch (error) {
        debugPrint('BLE GATT cache clear unavailable: $error');
      }
    }

    final disconnectedCompleter = Completer<void>();
    var hasConnected = false;
    final stateSub = device.connectionState.listen((s) {
      if (s == BluetoothConnectionState.connected) {
        hasConnected = true;
      } else if (hasConnected &&
          s == BluetoothConnectionState.disconnected
          && !disconnectedCompleter.isCompleted) {
        disconnectedCompleter.complete();
      }
    });

    BluetoothCharacteristic? tx;
    try {
      await device.connect(
          timeout: const Duration(seconds: 12), autoConnect: false);
      await _waitForState(device, BluetoothConnectionState.connected,
          const Duration(seconds: 12));

      if (Platform.isAndroid) {
        try {
          await device.requestMtu(247, timeout: 8);
        } catch (error) {
          debugPrint(
              'BLE MTU negotiation unavailable; using MTU ${device.mtuNow}: $error');
        }
      }

      final services = await device
          .discoverServices()
          .timeout(const Duration(seconds: 15));
      final svc = services.firstWhere(
        (s) => s.uuid == NusUuids.service,
        orElse: () => throw StateError('NUS service not found on this device'),
      );
      final rx = svc.characteristics.firstWhere(
        (c) => c.uuid == NusUuids.rx,
        orElse: () => throw StateError('NUS RX characteristic missing'),
      );
      tx = svc.characteristics.firstWhere(
        (c) => c.uuid == NusUuids.tx,
        orElse: () => throw StateError('NUS TX characteristic missing'),
      );
      if (!rx.properties.write) {
        throw StateError('NUS RX characteristic does not support acknowledged writes');
      }
      if (!tx.properties.notify) {
        throw StateError('NUS TX characteristic does not support notifications');
      }

      await tx
          .setNotifyValue(true)
          .timeout(const Duration(seconds: 10));

      final incoming =
          tx.onValueReceived.transform(const Utf8Decoder(allowMalformed: true));

      Future<void> doDisconnect() async {
        await _disconnectDevice(device, tx!, stateSub);
      }

      return NusConnection._(
        device: device,
        rx: rx,
        incoming: incoming,
        disconnected: disconnectedCompleter.future,
        doDisconnect: doDisconnect,
      );
    } catch (error, stackTrace) {
      await stateSub.cancel();
      if (device.isConnected) {
        try {
          await device.disconnect().timeout(const Duration(seconds: 4));
          await _waitForState(device, BluetoothConnectionState.disconnected,
              const Duration(seconds: 3));
        } catch (cleanupError) {
          debugPrint('BLE cleanup after connection setup failed: $cleanupError');
        }
      }
      Error.throwWithStackTrace(error, stackTrace);
    }
  }

  Future<void> _disconnectDevice(
    BluetoothDevice device,
    BluetoothCharacteristic tx,
    StreamSubscription<BluetoothConnectionState> stateSub,
  ) async {
    try {
      if (device.isConnected) {
        try {
          await tx
              .setNotifyValue(false)
              .timeout(const Duration(seconds: 4));
        } catch (error) {
          debugPrint('Unable to disable BLE notifications during disconnect: $error');
        }
        await device.disconnect().timeout(const Duration(seconds: 4));
        await _waitForState(device, BluetoothConnectionState.disconnected,
            const Duration(seconds: 3));
      }
    } finally {
      await stateSub.cancel();
    }
  }

  Future<void> _waitForState(
    BluetoothDevice device,
    BluetoothConnectionState target,
    Duration timeout,
  ) async {
    await device.connectionState
        .firstWhere((s) => s == target)
        .timeout(timeout);
  }
}
