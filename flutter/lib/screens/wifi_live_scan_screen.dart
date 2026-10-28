import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';

const _kAccent = Color(0xFF4FA8FF);

class WifiLiveScanScreen extends StatefulWidget {
  const WifiLiveScanScreen({super.key});

  @override
  State<WifiLiveScanScreen> createState() => _WifiLiveScanScreenState();
}

class _WifiLiveScanScreenState extends State<WifiLiveScanScreen> {
  String _mode = 'balanced';
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
    unawaited(_ctrl?.wlscanStop().catchError((Object _) {}));
    super.dispose();
  }

  void _start() {
    final ctrl = context.read<DeviceController>();
    unawaited(ctrl
        .wlscanStart(mode: _mode, intervalMs: 3000)
        .catchError((Object e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Live scan failed: $e')),
      );
    }));
  }

  Future<void> _setMode(String m) async {
    if (m == _mode) return;
    setState(() => _mode = m);
    final ctrl = context.read<DeviceController>();
    await ctrl.wlscanStop().catchError((Object _) {});
    await Future<void>.delayed(const Duration(milliseconds: 150));
    if (mounted) _start();
  }

  static String _fmtDist(int rssi) {
    final m = math.pow(10.0, (-45.0 - rssi) / 27.0).toDouble();
    if (m < 1.0) return '<1m';
    if (m < 10.0) return '${m.toStringAsFixed(1)}m';
    return '${m.round()}m';
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final aps = ctrl.wlscanAps;

    return Scaffold(
      appBar: AppBar(
        iconTheme: const IconThemeData(color: _kAccent),
        title: const Text(
          'WIFI LIVE SCAN',
          style: TextStyle(
              color: _kAccent,
              fontWeight: FontWeight.bold,
              letterSpacing: 2,
              fontSize: 16),
        ),
        actions: [
          if (ctrl.wlscanRunning)
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

          // ── Controls + stats ────────────────────────────────
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 12, 12, 6),
            child: Row(
              children: [
                _stat('APS', '${aps.length}'),
                const SizedBox(width: 8),
                _stat('UNIQUE', '${ctrl.wlscanUnique}'),
                const SizedBox(width: 8),
                _stat('CYCLES', '${ctrl.wlscanCycles}'),
              ],
            ),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            child: Row(
              children: [
                for (final m in const ['fast', 'balanced', 'deep']) ...[
                  Expanded(
                    child: Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 3),
                      child: ChoiceChip(
                        selected: _mode == m,
                        label: SizedBox(
                          width: double.infinity,
                          child: Text(
                            m.toUpperCase(),
                            textAlign: TextAlign.center,
                            style: TextStyle(
                              color: _mode == m ? Colors.black : Colors.white70,
                              fontSize: 11,
                              fontWeight: FontWeight.bold,
                            ),
                          ),
                        ),
                        selectedColor: _kAccent,
                        backgroundColor: Colors.white.withValues(alpha: 0.05),
                        side: BorderSide(color: _kAccent.withValues(alpha: 0.3)),
                        onSelected: (_) => _setMode(m),
                      ),
                    ),
                  ),
                ],
              ],
            ),
          ),
          const SizedBox(height: 6),

          Expanded(
            child: aps.isEmpty
                ? Center(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(Icons.travel_explore,
                            size: 44, color: _kAccent.withValues(alpha: 0.4)),
                        const SizedBox(height: 10),
                        Text(
                          ctrl.wlscanRunning
                              ? 'Scanning the airwaves…'
                              : 'Stopped',
                          style: const TextStyle(color: Colors.white38),
                        ),
                      ],
                    ),
                  )
                : ListView.separated(
                    padding: const EdgeInsets.fromLTRB(12, 0, 12, 12),
                    itemCount: aps.length,
                    separatorBuilder: (_, _) => const SizedBox(height: 8),
                    itemBuilder: (_, i) => _ApCard(
                      ap: aps[i],
                      distance: _fmtDist(aps[i].rssi),
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

class _ApCard extends StatelessWidget {
  const _ApCard({required this.ap, required this.distance});
  final WlscanAp ap;
  final String distance;

  Color _rssiColor(int rssi) {
    if (rssi >= -60) return const Color(0xFF39FF14);
    if (rssi >= -72) return const Color(0xFFFFD000);
    if (rssi >= -84) return const Color(0xFFFF9040);
    return const Color(0xFFFF4040);
  }

  @override
  Widget build(BuildContext context) {
    final rc = _rssiColor(ap.rssi);
    final frac = ((ap.rssi + 100) / 60).clamp(0.0, 1.0);
    final bandColor = ap.is5ghz ? Colors.purpleAccent : Colors.tealAccent;
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
              Expanded(
                child: Text(
                  ap.ssid.isNotEmpty ? ap.ssid : '(hidden)',
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    color: ap.ssid.isNotEmpty ? Colors.white : Colors.white54,
                    fontSize: 15,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                decoration: BoxDecoration(
                  color: bandColor.withValues(alpha: 0.15),
                  borderRadius: BorderRadius.circular(6),
                  border: Border.all(color: bandColor.withValues(alpha: 0.5)),
                ),
                child: Text(ap.is5ghz ? '5G' : '2.4G',
                    style: TextStyle(color: bandColor, fontSize: 10)),
              ),
            ],
          ),
          const SizedBox(height: 4),
          Text(
            ap.bssid,
            style: const TextStyle(
                color: Colors.white54,
                fontFamily: 'monospace',
                fontSize: 11),
          ),
          const SizedBox(height: 8),
          Row(
            children: [
              Text('ch ${ap.channel}',
                  style: const TextStyle(
                      color: Colors.white70,
                      fontSize: 11,
                      fontFamily: 'monospace')),
              const SizedBox(width: 12),
              Icon(Icons.social_distance, size: 13, color: Colors.white38),
              const SizedBox(width: 3),
              Text(distance,
                  style: const TextStyle(
                      color: Colors.white70,
                      fontSize: 11,
                      fontFamily: 'monospace')),
              const Spacer(),
              Text('${ap.rssi} dBm',
                  style: TextStyle(
                      color: rc,
                      fontSize: 12,
                      fontWeight: FontWeight.bold,
                      fontFamily: 'monospace')),
            ],
          ),
          const SizedBox(height: 6),
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
}
