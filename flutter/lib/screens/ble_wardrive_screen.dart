import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';

const _kAccent = Color(0xFF00E5FF);

/// BLE Wardriving — continuous, classified, live BLE discovery.
class BleWardriveScreen extends StatefulWidget {
  const BleWardriveScreen({super.key});

  @override
  State<BleWardriveScreen> createState() => _BleWardriveScreenState();
}

class _BleWardriveScreenState extends State<BleWardriveScreen> {
  bool _active = true;
  DeviceController? _ctrl;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _start();
    });
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
  }

  @override
  void dispose() {
    unawaited(_ctrl?.bleWarStop().catchError((Object _) {}));
    super.dispose();
  }

  void _start() {
    final ctrl = context.read<DeviceController>();
    unawaited(ctrl.bleWarStart(active: _active).catchError((Object e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('BLE wardrive failed: $e')),
      );
    }));
  }

  Future<void> _setActive(bool v) async {
    if (v == _active) return;
    setState(() => _active = v);
    final ctrl = context.read<DeviceController>();
    await ctrl.bleWarStop().catchError((Object _) {});
    await Future<void>.delayed(const Duration(milliseconds: 200));
    if (mounted) _start();
  }

  /// Rough BLE distance estimate (txPower≈-59 dBm @1m, path-loss n≈2.5).
  static String _fmtDist(int rssi) {
    final m = math.pow(10.0, (-59.0 - rssi) / 25.0).toDouble();
    if (m < 1.0) return '<1m';
    if (m < 10.0) return '${m.toStringAsFixed(1)}m';
    return '${m.round()}m';
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final devices = ctrl.bleWarDevices;

    return Scaffold(
      appBar: AppBar(
        iconTheme: const IconThemeData(color: _kAccent),
        title: const Text(
          'BLE WARDRIVE',
          style: TextStyle(
              color: _kAccent,
              fontWeight: FontWeight.bold,
              letterSpacing: 2,
              fontSize: 16),
        ),
        actions: [
          if (ctrl.bleWarRunning)
            const Padding(
              padding: EdgeInsets.only(right: 16),
              child: Center(
                child: SizedBox(
                  width: 16,
                  height: 16,
                  child: CircularProgressIndicator(
                      strokeWidth: 2, color: _kAccent),
                ),
              ),
            ),
        ],
      ),
      body: Column(
        children: [
          DeviceIssueBanner(controller: ctrl),

          // ── Stats ───────────────────────────────────────────
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 12, 12, 6),
            child: Row(
              children: [
                _stat('DEVICES', '${devices.length}'),
                const SizedBox(width: 8),
                _stat('UNIQUE', '${ctrl.bleWarUnique}'),
                const SizedBox(width: 8),
                _stat('CYCLES', '${ctrl.bleWarCycles}'),
              ],
            ),
          ),

          // ── Active / Passive toggle ─────────────────────────
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            child: Row(
              children: [
                for (final entry in const [
                  ['ACTIVE', 'true'],
                  ['PASSIVE', 'false'],
                ]) ...[
                  Expanded(
                    child: Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 3),
                      child: ChoiceChip(
                        selected: _active == (entry[1] == 'true'),
                        label: SizedBox(
                          width: double.infinity,
                          child: Text(
                            entry[0],
                            textAlign: TextAlign.center,
                            style: TextStyle(
                              color: _active == (entry[1] == 'true')
                                  ? Colors.black
                                  : Colors.white70,
                              fontSize: 11,
                              fontWeight: FontWeight.bold,
                            ),
                          ),
                        ),
                        selectedColor: _kAccent,
                        backgroundColor: Colors.white.withValues(alpha: 0.05),
                        side: BorderSide(color: _kAccent.withValues(alpha: 0.3)),
                        onSelected: (_) => _setActive(entry[1] == 'true'),
                      ),
                    ),
                  ),
                ],
              ],
            ),
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(16, 6, 16, 2),
            child: Align(
              alignment: Alignment.centerLeft,
              child: Text(
                'Active sends scan requests → gets device names.',
                style: TextStyle(color: Colors.white38, fontSize: 10),
              ),
            ),
          ),
          const SizedBox(height: 4),

          Expanded(
            child: devices.isEmpty
                ? Center(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(Icons.bluetooth_searching,
                            size: 44, color: _kAccent.withValues(alpha: 0.4)),
                        const SizedBox(height: 10),
                        Text(
                          ctrl.bleWarRunning
                              ? 'Listening for BLE devices…'
                              : 'Stopped',
                          style: const TextStyle(color: Colors.white38),
                        ),
                      ],
                    ),
                  )
                : ListView.separated(
                    padding: const EdgeInsets.fromLTRB(12, 0, 12, 12),
                    itemCount: devices.length,
                    separatorBuilder: (_, _) => const SizedBox(height: 8),
                    itemBuilder: (_, i) => _DevCard(
                      dev: devices[i],
                      distance: _fmtDist(devices[i].rssi),
                    ),
                  ),
          ),
        ],
      ),
    );
  }

  Widget _stat(String label, String value) {
    return Expanded(
      child: Container(
        padding: const EdgeInsets.symmetric(vertical: 8),
        decoration: BoxDecoration(
          color: Colors.black.withValues(alpha: 0.4),
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: _kAccent.withValues(alpha: 0.25)),
        ),
        child: Column(
          children: [
            Text(value,
                style: const TextStyle(
                    color: _kAccent,
                    fontSize: 16,
                    fontWeight: FontWeight.bold,
                    fontFamily: 'monospace')),
            Text(label,
                style: TextStyle(
                    color: Colors.white.withValues(alpha: 0.5),
                    fontSize: 9,
                    letterSpacing: 1)),
          ],
        ),
      ),
    );
  }
}

class _DevCard extends StatelessWidget {
  const _DevCard({required this.dev, required this.distance});
  final BleWarDevice dev;
  final String distance;

  static const _vendorColors = <String, Color>{
    'apple': Color(0xFFE0E0E0),
    'google': Color(0xFF34A853),
    'microsoft': Color(0xFF00A4EF),
    'samsung': Color(0xFF1428A0),
    'nordic': Color(0xFF00A9CE),
    'espressif': Color(0xFFE7352C),
    'tile': Color(0xFF6ED6C4),
  };

  Color get _vendorColor =>
      _vendorColors[dev.vendor] ?? _kAccent.withValues(alpha: 0.8);

  IconData get _dtypeIcon {
    switch (dev.dtype) {
      case 'airpods':
        return Icons.headphones;
      case 'watch':
        return Icons.watch;
      case 'findmy':
        return Icons.my_location;
      case 'tile':
        return Icons.location_searching;
      case 'fastpair':
        return Icons.bluetooth_connected;
      case 'exposure':
        return Icons.coronavirus;
      case 'eddystone':
        return Icons.sensors;
      case 'nearby':
      case 'handoff':
      case 'airdrop':
      case 'apple':
        return Icons.phone_iphone;
      default:
        return Icons.bluetooth;
    }
  }

  Color _rssiColor(int rssi) {
    if (rssi >= -60) return const Color(0xFF39FF14);
    if (rssi >= -75) return const Color(0xFFFFD000);
    if (rssi >= -88) return const Color(0xFFFF9040);
    return const Color(0xFFFF4040);
  }

  Color _privColor(String priv) {
    switch (priv) {
      case 'pub':
        return const Color(0xFFFFD000); // trackable public MAC
      case 'static':
        return const Color(0xFFFF9040);
      case 'rpa':
        return const Color(0xFF39FF14); // privacy-rotating
      default:
        return Colors.white38;
    }
  }

  @override
  Widget build(BuildContext context) {
    final rc = _rssiColor(dev.rssi);
    final frac = ((dev.rssi + 100) / 70).clamp(0.0, 1.0);
    final name = dev.name.isNotEmpty ? dev.name : '(no name)';
    final vc = _vendorColor;

    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: _kAccent.withValues(alpha: 0.2)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(
                width: 34,
                height: 34,
                decoration: BoxDecoration(
                  color: vc.withValues(alpha: 0.14),
                  borderRadius: BorderRadius.circular(9),
                  border: Border.all(color: vc.withValues(alpha: 0.5)),
                ),
                child: Icon(_dtypeIcon, color: vc, size: 18),
              ),
              const SizedBox(width: 10),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      name,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(
                        color: dev.name.isNotEmpty
                            ? Colors.white
                            : Colors.white54,
                        fontSize: 15,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    Text(
                      dev.addr,
                      style: const TextStyle(
                          color: Colors.white54,
                          fontFamily: 'monospace',
                          fontSize: 11),
                    ),
                  ],
                ),
              ),
              Text('${dev.rssi} dBm',
                  style: TextStyle(
                      color: rc,
                      fontSize: 12,
                      fontWeight: FontWeight.bold,
                      fontFamily: 'monospace')),
            ],
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 6,
            runSpacing: 6,
            children: [
              if (dev.vendor != '-') _tag(dev.vendor, vc),
              if (dev.dtype != '-')
                _tag(dev.dtype, _kAccent.withValues(alpha: 0.6)),
              _tag(dev.priv, _privColor(dev.priv)),
              if (dev.company != '----') _tag('id ${dev.company}', Colors.white24),
              _tag(distance, Colors.white24),
            ],
          ),
          const SizedBox(height: 8),
          ClipRRect(
            borderRadius: BorderRadius.circular(4),
            child: LinearProgressIndicator(
              value: frac,
              minHeight: 5,
              backgroundColor: Colors.white12,
              valueColor: AlwaysStoppedAnimation<Color>(rc),
            ),
          ),
        ],
      ),
    );
  }

  Widget _tag(String text, Color border) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(5),
        border: Border.all(color: border),
      ),
      child: Text(
        text,
        style: const TextStyle(
            color: Colors.white70, fontSize: 10, fontFamily: 'monospace'),
      ),
    );
  }
}
