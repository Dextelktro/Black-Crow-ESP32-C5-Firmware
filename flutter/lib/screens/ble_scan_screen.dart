import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';

const _kAccent = Color(0xFF4FA8FF);

class BleScanScreen extends StatelessWidget {
  const BleScanScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('BLE Scan')),
      body: const BleScanBody(),
    );
  }
}

/// BLE scan screen body — detects nearby BLE devices via the firmware scanner.
class BleScanBody extends StatefulWidget {
  const BleScanBody({super.key});

  @override
  State<BleScanBody> createState() => _BleScanBodyState();
}

class _BleScanBodyState extends State<BleScanBody> {
  final _durCtrl = TextEditingController(text: '30');
  bool _active = true;
  Timer? _ticker;
  late final DeviceController _ctrl;
  late final Future<void> _entryCleanup;

  @override
  void dispose() {
    _ticker?.cancel();
    _ticker = null;
    _durCtrl.dispose();
    unawaited(_cleanUpScan());
    super.dispose();
  }

  @override
  void initState() {
    super.initState();
    _ctrl = context.read<DeviceController>();
    _entryCleanup = _ctrl.resetBleScanForScreen();
    unawaited(
      _entryCleanup.catchError((Object error) {
        debugPrint('BLE scan screen reset failed: $error');
      }),
    );
  }

  Future<void> _cleanUpScan() async {
    try {
      await _entryCleanup;
    } catch (error) {
      debugPrint('BLE scan screen startup cleanup failed: $error');
    }
    try {
      await _ctrl.resetBleScanForScreen();
    } catch (error) {
      debugPrint('BLE scan screen exit cleanup failed: $error');
    }
  }

  void _syncTicker(bool running) {
    if (running && _ticker == null) {
      _ticker = Timer.periodic(const Duration(seconds: 1), (_) {
        if (mounted) setState(() {});
      });
    } else if (!running && _ticker != null) {
      _ticker!.cancel();
      _ticker = null;
    }
  }

  void _run(Future<void> action) {
    unawaited(
      action.catchError((Object e, StackTrace s) {
        debugPrint('ble-scan action failed: $e\n$s');
        if (!mounted) return;
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text('BLE scan failed: $e')));
      }),
    );
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final running = ctrl.bleScanRunning;
    _syncTicker(running);
    final devices = ctrl.bleScanDevices;

    return Container(
      color: const Color(0xFF02060D),
      child: Column(
        children: [
          DeviceIssueBanner(controller: ctrl),

          // ── Stats ──────────────────────────────────────────
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 6, 12, 8),
            child: Row(
              children: [
                Expanded(
                  child: _Stat(label: 'FOUND', value: '${ctrl.bleScanFound}'),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: _Stat(
                    label: 'ELAPSED',
                    value: '${ctrl.bleScanElapsed}s',
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: _Stat(
                    label: 'MODE',
                    value: ctrl.bleScanActive ? 'ACTIVE' : 'PASSIVE',
                  ),
                ),
              ],
            ),
          ),

          // ── Controls ───────────────────────────────────────
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            child: Row(
              children: [
                if (!running) ...[
                  SizedBox(
                    width: 74,
                    child: TextField(
                      controller: _durCtrl,
                      keyboardType: TextInputType.number,
                      style: const TextStyle(color: Colors.white),
                      decoration: const InputDecoration(
                        labelText: 'Secs',
                        labelStyle: TextStyle(color: Colors.white54),
                        isDense: true,
                        border: OutlineInputBorder(),
                      ),
                      inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: InkWell(
                      onTap: () => setState(() => _active = !_active),
                      borderRadius: BorderRadius.circular(8),
                      child: Container(
                        height: 44,
                        padding: const EdgeInsets.symmetric(horizontal: 8),
                        decoration: BoxDecoration(
                          borderRadius: BorderRadius.circular(8),
                          border: Border.all(
                            color: _kAccent.withValues(alpha: 0.4),
                          ),
                        ),
                        child: Row(
                          children: [
                            Icon(
                              _active
                                  ? Icons.check_box
                                  : Icons.check_box_outline_blank,
                              color: _kAccent,
                              size: 18,
                            ),
                            const SizedBox(width: 6),
                            const Flexible(
                              child: Text(
                                'Active (names)',
                                style: TextStyle(
                                  color: Colors.white70,
                                  fontSize: 11,
                                ),
                              ),
                            ),
                          ],
                        ),
                      ),
                    ),
                  ),
                ],
                if (running) const Spacer(),
                const SizedBox(width: 8),
                SizedBox(
                  height: 44,
                  child: ElevatedButton.icon(
                    style: ElevatedButton.styleFrom(
                      backgroundColor: running
                          ? const Color(0xFF8B0000)
                          : _kAccent,
                      foregroundColor: running ? Colors.white : Colors.black,
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(8),
                      ),
                    ),
                    onPressed: () {
                      if (running) {
                        _run(ctrl.bleScanStop());
                      } else {
                        final secs = int.tryParse(_durCtrl.text) ?? 30;
                        _run(
                          ctrl.bleScanStart(active: _active, durationSec: secs),
                        );
                      }
                    },
                    icon: Icon(running ? Icons.stop : Icons.radar, size: 18),
                    label: Text(
                      running ? 'STOP' : 'SCAN',
                      style: const TextStyle(
                        fontWeight: FontWeight.bold,
                        letterSpacing: 1.2,
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 8),

          // ── Device list ────────────────────────────────────
          Expanded(
            child: devices.isEmpty
                ? Center(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(
                          Icons.bluetooth_searching,
                          size: 44,
                          color: _kAccent.withValues(alpha: 0.4),
                        ),
                        const SizedBox(height: 10),
                        Text(
                          running
                              ? 'Scanning for BLE devices…'
                              : 'Tap SCAN to detect nearby BLE devices',
                          style: const TextStyle(color: Colors.white38),
                          textAlign: TextAlign.center,
                        ),
                      ],
                    ),
                  )
                : ListView.separated(
                    padding: const EdgeInsets.fromLTRB(12, 0, 12, 12),
                    itemCount: devices.length,
                    separatorBuilder: (_, _) => const SizedBox(height: 8),
                    itemBuilder: (_, i) => _DeviceCard(dev: devices[i]),
                  ),
          ),

          if (!running && devices.isNotEmpty)
            Padding(
              padding: const EdgeInsets.fromLTRB(12, 0, 12, 10),
              child: SizedBox(
                width: double.infinity,
                height: 38,
                child: OutlinedButton.icon(
                  style: OutlinedButton.styleFrom(
                    side: const BorderSide(color: Colors.white24),
                    foregroundColor: Colors.white60,
                  ),
                  onPressed: () => _run(ctrl.bleScanClear()),
                  icon: const Icon(Icons.clear_all, size: 16),
                  label: const Text('CLEAR'),
                ),
              ),
            ),
        ],
      ),
    );
  }
}

class _Stat extends StatelessWidget {
  const _Stat({required this.label, required this.value});
  final String label;
  final String value;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 8),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: _kAccent.withValues(alpha: 0.25)),
      ),
      child: Column(
        children: [
          FittedBox(
            fit: BoxFit.scaleDown,
            child: Text(
              value,
              style: const TextStyle(
                color: _kAccent,
                fontSize: 16,
                fontWeight: FontWeight.bold,
                fontFamily: 'monospace',
              ),
            ),
          ),
          const SizedBox(height: 2),
          Text(
            label,
            style: TextStyle(
              color: Colors.white.withValues(alpha: 0.5),
              fontSize: 9,
              letterSpacing: 1,
              fontFamily: 'monospace',
            ),
          ),
        ],
      ),
    );
  }
}

class _DeviceCard extends StatelessWidget {
  const _DeviceCard({required this.dev});
  final BleScanDevice dev;

  Color _rssiColor(int rssi) {
    if (rssi >= -60) return const Color(0xFF39FF14);
    if (rssi >= -75) return const Color(0xFFFFD000);
    if (rssi >= -88) return const Color(0xFFFF9040);
    return const Color(0xFFFF4040);
  }

  @override
  Widget build(BuildContext context) {
    final rc = _rssiColor(dev.rssi);
    final name = dev.name.isNotEmpty ? dev.name : '(no name)';
    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: _kAccent.withValues(alpha: 0.2)),
      ),
      child: Row(
        children: [
          // RSSI badge
          Container(
            width: 52,
            padding: const EdgeInsets.symmetric(vertical: 8),
            decoration: BoxDecoration(
              color: rc.withValues(alpha: 0.12),
              borderRadius: BorderRadius.circular(8),
              border: Border.all(color: rc.withValues(alpha: 0.4)),
            ),
            child: Column(
              children: [
                Text(
                  '${dev.rssi}',
                  style: TextStyle(
                    color: rc,
                    fontWeight: FontWeight.bold,
                    fontFamily: 'monospace',
                    fontSize: 14,
                  ),
                ),
                const Text(
                  'dBm',
                  style: TextStyle(color: Colors.white38, fontSize: 8),
                ),
              ],
            ),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  name,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    color: dev.name.isNotEmpty ? Colors.white : Colors.white54,
                    fontSize: 14,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  dev.addr,
                  style: const TextStyle(
                    color: Colors.white54,
                    fontFamily: 'monospace',
                    fontSize: 11,
                  ),
                ),
                const SizedBox(height: 4),
                Row(
                  children: [
                    _tag(
                      dev.addrType == 0 ? 'public' : 'random',
                      Colors.white24,
                    ),
                    if (dev.mfg != '----') ...[
                      const SizedBox(width: 6),
                      _tag('mfg ${dev.mfg}', _kAccent.withValues(alpha: 0.4)),
                    ],
                    const SizedBox(width: 6),
                    _tag('×${dev.seen}', Colors.white24),
                  ],
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _tag(String text, Color border) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(4),
        border: Border.all(color: border),
      ),
      child: Text(
        text,
        style: const TextStyle(
          color: Colors.white60,
          fontSize: 9,
          fontFamily: 'monospace',
        ),
      ),
    );
  }
}
