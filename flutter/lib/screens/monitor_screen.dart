import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/ap_entry.dart';
import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';

const _kAccent = Color(0xFF4FA8FF);

class MonitorScreen extends StatefulWidget {
  const MonitorScreen({super.key, required this.controller, required this.ap});

  final DeviceController controller;
  final ApEntry ap;

  @override
  State<MonitorScreen> createState() => _MonitorScreenState();
}

class _MonitorScreenState extends State<MonitorScreen> {
  @override
  void initState() {
    super.initState();
    // Defer the start command until after first frame so we don't notify
    // listeners mid-build.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      unawaited(
        widget.controller
            .monitorStart(widget.ap.idx,
                channel: widget.ap.channel, is5ghz: widget.ap.is5ghz)
            .catchError((Object e) {
          if (!mounted) return;
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(content: Text('Monitor failed: $e')),
          );
        }),
      );
    });
  }

  @override
  void dispose() {
    // Fire-and-forget stop; the screen is going away.
    unawaited(widget.controller.monitorStop().catchError((Object _) {}));
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final ap = widget.ap;
    final liveRssi = ctrl.monitorRssi != 0 ? ctrl.monitorRssi : ap.rssi;

    return Scaffold(
      appBar: AppBar(
        iconTheme: const IconThemeData(color: _kAccent),
        title: Text(
          ap.ssid.isNotEmpty ? ap.ssid : '(hidden network)',
          style: const TextStyle(
            color: _kAccent,
            fontWeight: FontWeight.bold,
            letterSpacing: 1.5,
            fontSize: 16,
          ),
        ),
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          DeviceIssueBanner(controller: ctrl),

          // ── Band + live status row ──────────────────────────
          Row(
            children: [
              _BandChip(is5ghz: ap.is5ghz),
              const SizedBox(width: 8),
              _LiveDot(running: ctrl.monitorRunning),
              const Spacer(),
              Text(
                'ch ${ap.channel}',
                style: const TextStyle(
                  color: _kAccent,
                  fontFamily: 'monospace',
                  fontSize: 14,
                  fontWeight: FontWeight.bold,
                ),
              ),
            ],
          ),
          const SizedBox(height: 20),

          // ── RSSI gauge ──────────────────────────────────────
          Center(
            child: SizedBox(
              width: 220,
              height: 150,
              child: CustomPaint(
                painter: _RssiGaugePainter(rssi: liveRssi),
                child: Center(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      const SizedBox(height: 30),
                      Text(
                        '$liveRssi',
                        style: TextStyle(
                          color: _rssiColor(liveRssi),
                          fontSize: 44,
                          fontWeight: FontWeight.w800,
                          fontFamily: 'monospace',
                          height: 1,
                        ),
                      ),
                      const Text(
                        'dBm',
                        style: TextStyle(
                            color: Colors.white54, fontSize: 12),
                      ),
                      const SizedBox(height: 2),
                      Text(
                        _rssiQuality(liveRssi),
                        style: TextStyle(
                          color: _rssiColor(liveRssi),
                          fontSize: 12,
                          fontWeight: FontWeight.bold,
                          letterSpacing: 1.5,
                        ),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ),
          const SizedBox(height: 20),

          // ── Live traffic stats ──────────────────────────────
          Row(
            children: [
              Expanded(
                  child: _StatCard(
                      label: 'FPS',
                      value: '${ctrl.monitorFps}',
                      icon: Icons.speed)),
              const SizedBox(width: 8),
              Expanded(
                  child: _StatCard(
                      label: 'FRAMES',
                      value: _fmt(ctrl.monitorFrames),
                      icon: Icons.layers)),
              const SizedBox(width: 8),
              Expanded(
                  child: _StatCard(
                      label: 'CLIENTS',
                      value: '${ctrl.monitorStas}',
                      icon: Icons.devices)),
            ],
          ),
          const SizedBox(height: 8),
          Row(
            children: [
              Expanded(
                  child: _StatCard(
                      label: 'MGMT',
                      value: _fmt(ctrl.monitorMgmt),
                      icon: Icons.settings_input_antenna)),
              const SizedBox(width: 8),
              Expanded(
                  child: _StatCard(
                      label: 'DATA',
                      value: _fmt(ctrl.monitorData),
                      icon: Icons.data_usage)),
            ],
          ),
          const SizedBox(height: 20),

          // ── Identity / profile ──────────────────────────────
          _ProfileTile(label: 'SSID', value: ap.ssid.isNotEmpty ? ap.ssid : '(hidden)'),
          _ProfileTile(label: 'BSSID / MAC', value: ap.bssid, mono: true),
          _ProfileTile(label: 'CHANNEL', value: '${ap.channel}'),
          _ProfileTile(
              label: 'BAND', value: ap.is5ghz ? '5 GHz' : '2.4 GHz'),
          _ProfileTile(
              label: 'SIGNAL',
              value: '$liveRssi dBm · ${_rssiPercent(liveRssi)}%'),
          const SizedBox(height: 8),
          Center(
            child: Text(
              ctrl.monitorRunning
                  ? 'Live — leave this screen to stop monitoring'
                  : 'Monitor stopped',
              style: const TextStyle(color: Colors.white38, fontSize: 11),
            ),
          ),
        ],
      ),
    );
  }

  static String _fmt(int n) {
    if (n >= 1000000) return '${(n / 1000000).toStringAsFixed(1)}M';
    if (n >= 1000) return '${(n / 1000).toStringAsFixed(1)}k';
    return '$n';
  }

  static int _rssiPercent(int rssi) =>
      (((rssi + 90) / 60) * 100).clamp(0, 100).round();

  static String _rssiQuality(int rssi) {
    if (rssi >= -50) return 'EXCELLENT';
    if (rssi >= -60) return 'GOOD';
    if (rssi >= -70) return 'FAIR';
    if (rssi >= -80) return 'WEAK';
    return 'POOR';
  }

  static Color _rssiColor(int rssi) {
    if (rssi >= -50) return const Color(0xFF39FF14);
    if (rssi >= -60) return const Color(0xFF8FFF8F);
    if (rssi >= -70) return const Color(0xFFFFD000);
    if (rssi >= -80) return const Color(0xFFFF9040);
    return const Color(0xFFFF4040);
  }
}

class _RssiGaugePainter extends CustomPainter {
  _RssiGaugePainter({required this.rssi});
  final int rssi;

  @override
  void paint(Canvas canvas, Size size) {
    final center = Offset(size.width / 2, size.height - 10);
    final radius = size.width / 2 - 8;
    const start = math.pi; // 180° (left)
    const sweep = math.pi; // half circle

    final track = Paint()
      ..color = Colors.white12
      ..style = PaintingStyle.stroke
      ..strokeWidth = 12
      ..strokeCap = StrokeCap.round;
    canvas.drawArc(Rect.fromCircle(center: center, radius: radius), start,
        sweep, false, track);

    final frac = (((rssi + 90) / 60)).clamp(0.0, 1.0);
    final col = _MonitorScreenState._rssiColor(rssi);
    final prog = Paint()
      ..shader = SweepGradient(
        startAngle: start,
        endAngle: start + sweep,
        colors: [col.withValues(alpha: 0.4), col],
      ).createShader(Rect.fromCircle(center: center, radius: radius))
      ..style = PaintingStyle.stroke
      ..strokeWidth = 12
      ..strokeCap = StrokeCap.round;
    canvas.drawArc(Rect.fromCircle(center: center, radius: radius), start,
        sweep * frac, false, prog);

    // needle
    final ang = start + sweep * frac;
    final tip = Offset(
      center.dx + (radius - 2) * math.cos(ang),
      center.dy + (radius - 2) * math.sin(ang),
    );
    canvas.drawLine(
        center,
        tip,
        Paint()
          ..color = col
          ..strokeWidth = 2.5
          ..strokeCap = StrokeCap.round);
    canvas.drawCircle(center, 5, Paint()..color = col);
  }

  @override
  bool shouldRepaint(_RssiGaugePainter old) => old.rssi != rssi;
}

class _BandChip extends StatelessWidget {
  const _BandChip({required this.is5ghz});
  final bool is5ghz;

  @override
  Widget build(BuildContext context) {
    final c = is5ghz ? Colors.purpleAccent : Colors.tealAccent;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
      decoration: BoxDecoration(
        color: c.withValues(alpha: 0.15),
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: c.withValues(alpha: 0.5)),
      ),
      child: Text(
        is5ghz ? '5 GHz' : '2.4 GHz',
        style: TextStyle(
            color: c, fontSize: 12, fontWeight: FontWeight.bold),
      ),
    );
  }
}

class _LiveDot extends StatelessWidget {
  const _LiveDot({required this.running});
  final bool running;

  @override
  Widget build(BuildContext context) {
    final c = running ? const Color(0xFF39FF14) : Colors.white30;
    return Row(
      children: [
        Container(
          width: 8,
          height: 8,
          decoration: BoxDecoration(
            color: c,
            shape: BoxShape.circle,
            boxShadow:
                running ? [BoxShadow(color: c, blurRadius: 8)] : null,
          ),
        ),
        const SizedBox(width: 6),
        Text(
          running ? 'LIVE' : 'IDLE',
          style: TextStyle(
              color: c, fontSize: 11, fontWeight: FontWeight.bold),
        ),
      ],
    );
  }
}

class _StatCard extends StatelessWidget {
  const _StatCard(
      {required this.label, required this.value, required this.icon});
  final String label;
  final String value;
  final IconData icon;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 12, horizontal: 8),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: _kAccent.withValues(alpha: 0.25)),
      ),
      child: Column(
        children: [
          Icon(icon, color: _kAccent.withValues(alpha: 0.7), size: 16),
          const SizedBox(height: 6),
          FittedBox(
            fit: BoxFit.scaleDown,
            child: Text(
              value,
              style: const TextStyle(
                color: _kAccent,
                fontSize: 20,
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

class _ProfileTile extends StatelessWidget {
  const _ProfileTile(
      {required this.label, required this.value, this.mono = false});
  final String label;
  final String value;
  final bool mono;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(
            width: 110,
            child: Text(
              label,
              style: TextStyle(
                color: Colors.white.withValues(alpha: 0.45),
                fontSize: 11,
                letterSpacing: 1.2,
              ),
            ),
          ),
          Expanded(
            child: SelectableText(
              value,
              style: TextStyle(
                color: Colors.white,
                fontSize: 13,
                fontFamily: mono ? 'monospace' : null,
                fontWeight: FontWeight.w600,
              ),
            ),
          ),
        ],
      ),
    );
  }
}
