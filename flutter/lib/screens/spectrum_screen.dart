import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';
import '../widgets/waterfall.dart';

const _k24 = Color(0xFF2EE6C5);
const _k5 = Color(0xFFB06CFF);
const int _kMaxRows = 140;
const int _kAverageSweeps = 20;

class SpectrumScreen extends StatefulWidget {
  const SpectrumScreen({super.key});

  @override
  State<SpectrumScreen> createState() => _SpectrumScreenState();
}

class _SpectrumScreenState extends State<SpectrumScreen>
    with SingleTickerProviderStateMixin {
  bool _include5 = true;
  DeviceController? _ctrl;

  final List<List<double>> _rows = [];
  final List<List<bool>> _rowValidity = [];
  final List<List<double>> _averageHistory = [];
  final List<List<bool>> _averageValidityHistory = [];
  List<double> _live = const [];
  List<double> _average = const [];
  List<bool> _liveValidity = const [];
  List<bool> _averageValidity = const [];
  int _wfRev = 0;
  int _liveRev = 0;
  int _lastSweep = -1;
  int _lastLen = -1;

  // ── stats meters (render fps + sweep rate) ──
  Ticker? _frameTicker;
  int _frameCount = 0;
  Timer? _fpsTimer;
  int _fps = 0;
  double _sweepRate = 0; // sweeps / second (EMA)
  int _rateSweep = -1;
  double _rateAtMs = 0;

  @override
  void initState() {
    super.initState();
    // Count rendered frames; the waterfall animates continuously so this is a
    // real display-fps readout. Refresh the number twice a second.
    _frameTicker = createTicker((_) => _frameCount++)..start();
    _fpsTimer = Timer.periodic(const Duration(milliseconds: 500), (_) {
      if (!mounted) return;
      setState(() {
        _fps = _frameCount * 2;
        _frameCount = 0;
      });
    });
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
    _fpsTimer?.cancel();
    _frameTicker?.dispose();
    unawaited(_ctrl?.spectrumStop().catchError((Object _) {}));
    super.dispose();
  }

  void _start() {
    final ctrl = context.read<DeviceController>();
    unawaited(
      ctrl.spectrumStart(include5ghz: _include5).catchError((Object e) {
        if (!mounted) return;
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text('Spectrum failed: $e')));
      }),
    );
  }

  Future<void> _toggle5(bool v) async {
    setState(() {
      _include5 = v;
      _rows.clear();
      _rowValidity.clear();
      _averageHistory.clear();
      _averageValidityHistory.clear();
      _average = const [];
      _averageValidity = const [];
      _lastSweep = -1;
      _lastLen = -1;
      _rateSweep = -1;
      _sweepRate = 0;
      _wfRev++;
    });
    final ctrl = context.read<DeviceController>();
    await ctrl.spectrumStop().catchError((Object _) {});
    await Future<void>.delayed(const Duration(milliseconds: 150));
    if (mounted) _start();
  }

  /// Fold the controller's latest channel snapshot into the live strip, and
  /// commit one waterfall row per completed sweep. Runs in build.
  ///
  /// Only channels with received frames have an RSSI sample. Empty channels
  /// remain gaps instead of being drawn as a fabricated RSSI floor.
  void _ingest(DeviceController ctrl) {
    final chans = ctrl.spectrumChannels;
    final now = DateTime.now();

    if (chans.length != _lastLen) {
      _rows.clear();
      _rowValidity.clear();
      _averageHistory.clear();
      _averageValidityHistory.clear();
      _average = const [];
      _averageValidity = const [];
      _lastLen = chans.length;
      _lastSweep = -1;
      _wfRev++;
    }

    final inst = List<double>.filled(chans.length, 0);
    final validity = List<bool>.filled(chans.length, false);
    for (var i = 0; i < chans.length; i++) {
      final c = chans[i];
      validity[i] =
          c.valid &&
          c.count > 0 &&
          now.difference(c.sampledAt) <= const Duration(seconds: 3);
      if (validity[i]) {
        inst[i] = ((c.rssi + 100) / 60).clamp(0.0, 1.0);
      }
    }
    _live = inst;
    _liveValidity = validity;
    _liveRev++;

    if (chans.isNotEmpty && ctrl.spectrumSweep != _lastSweep) {
      _lastSweep = ctrl.spectrumSweep;
      _rows.add(List<double>.of(_live));
      _rowValidity.add(List<bool>.of(_liveValidity));
      _averageHistory.add(List<double>.of(_live));
      _averageValidityHistory.add(List<bool>.of(_liveValidity));
      if (_averageHistory.length > _kAverageSweeps) {
        _averageHistory.removeAt(0);
        _averageValidityHistory.removeAt(0);
      }
      final average = List<double>.filled(chans.length, 0);
      final averageValidity = List<bool>.filled(chans.length, false);
      for (var i = 0; i < chans.length; i++) {
        var total = 0.0;
        var samples = 0;
        for (var j = 0; j < _averageHistory.length; j++) {
          if (_averageValidityHistory[j][i]) {
            total += _averageHistory[j][i];
            samples++;
          }
        }
        if (samples > 0) {
          average[i] = total / samples;
          averageValidity[i] = true;
        }
      }
      _average = average;
      _averageValidity = averageValidity;
      if (_rows.length > _kMaxRows) _rows.removeAt(0);
      if (_rowValidity.length > _kMaxRows) _rowValidity.removeAt(0);
      _wfRev++;

      // sweep-rate EMA ("capture speed")
      final now = DateTime.now().millisecondsSinceEpoch.toDouble();
      if (_rateSweep >= 0) {
        final ds = ctrl.spectrumSweep - _rateSweep;
        final dt = now - _rateAtMs;
        if (ds > 0 && dt > 0) {
          final inst = ds * 1000.0 / dt;
          _sweepRate = _sweepRate == 0 ? inst : _sweepRate * 0.6 + inst * 0.4;
        }
      }
      _rateSweep = ctrl.spectrumSweep;
      _rateAtMs = now;
    }
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    _ingest(ctrl);
    final chans = ctrl.spectrumChannels;
    final hasMeanRssi = chans.any((c) => c.hasMeanRssi);

    SpecChannel? peak;
    var active = 0;
    var bestPeak = -128;
    DateTime? latestSampleAt;
    for (final c in chans) {
      if (c.valid &&
          (latestSampleAt == null || c.sampledAt.isAfter(latestSampleAt))) {
        latestSampleAt = c.sampledAt;
      }
      if (c.count > 0 &&
          c.valid &&
          DateTime.now().difference(c.sampledAt) <=
              const Duration(seconds: 3)) {
        active++;
        if (peak == null || c.rssi > peak.rssi) peak = c;
        if (c.peakRssi > bestPeak) bestPeak = c.peakRssi;
      }
    }

    // Band boundary (2.4 → 5 GHz) and a few axis ticks from the sorted list.
    final boundary = chans.indexWhere((c) => c.is5ghz);
    final markers = <int>[if (boundary > 0) boundary];
    final ticks = _axisTicks(chans);

    // Compact, reference-like scope size instead of filling the whole screen.
    final wfH = (MediaQuery.of(context).size.height * 0.34).clamp(220.0, 360.0);
    final rate = _sweepRate >= 10
        ? _sweepRate.toStringAsFixed(0)
        : _sweepRate.toStringAsFixed(1);
    final sampleAge = latestSampleAt == null
        ? '—'
        : '${DateTime.now().difference(latestSampleAt).inMilliseconds}ms';

    return Scaffold(
      backgroundColor: const Color(0xFF02100C),
      appBar: AppBar(
        backgroundColor: Colors.black,
        iconTheme: const IconThemeData(color: _k24),
        title: const Text(
          'WIFI SPECTRUM',
          style: TextStyle(
            color: _k24,
            fontWeight: FontWeight.bold,
            letterSpacing: 2,
            fontSize: 16,
          ),
        ),
      ),
      body: Column(
        children: [
          DeviceIssueBanner(controller: ctrl),

          // ── Mode line (honest: this is a channel-activity scan, not an SDR) ─
          Padding(
            padding: const EdgeInsets.fromLTRB(14, 10, 14, 2),
            child: Row(
              children: [
                Text(
                  'CHANNEL SCAN · ${_include5 ? '2.4+5 GHz' : '2.4 GHz'} · '
                  '${chans.length} ch · radio RSSI',
                  style: const TextStyle(
                    color: Colors.white38,
                    fontSize: 11,
                    fontFamily: 'monospace',
                  ),
                ),
                const Spacer(),
                if (ctrl.spectrumRunning)
                  const SizedBox(
                    width: 13,
                    height: 13,
                    child: CircularProgressIndicator(
                      strokeWidth: 2,
                      color: _k24,
                    ),
                  ),
              ],
            ),
          ),

          // ── Header readout chips ────────────────────────────
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 6, 12, 6),
            child: Wrap(
              spacing: 8,
              runSpacing: 6,
              children: [
                _chip('SWEEP ${ctrl.spectrumSweep}', _k24),
                _chip(
                  peak != null
                      ? '${hasMeanRssi ? 'MEAN' : 'RSSI'} '
                            '${peak.freq}MHz ${peak.rssi} dBm'
                      : 'RSSI —',
                  const Color(0xFFFFD000),
                ),
                _chip(
                  bestPeak > -128 ? 'PKT PEAK $bestPeak dBm' : 'PKT PEAK —',
                  const Color(0xFFFF9C66),
                ),
              ],
            ),
          ),

          // ── Live spectrum strip ─────────────────────────────
          Row(
            children: [
              const Padding(
                padding: EdgeInsets.only(left: 8),
                child: SpectrumYAxis(
                  labels: ['-40', '-55', '-70', '-85', '-100'],
                ),
              ),
              Expanded(
                child: Container(
                  height: 110,
                  margin: const EdgeInsets.fromLTRB(4, 2, 12, 8),
                  decoration: BoxDecoration(
                    borderRadius: BorderRadius.circular(12),
                    border: Border.all(color: _k24.withValues(alpha: 0.18)),
                  ),
                  clipBehavior: Clip.antiAlias,
                  child: _live.isEmpty
                      ? const Center(
                          child: Text(
                            'Sampling channels…',
                            style: TextStyle(color: Colors.white38),
                          ),
                        )
                      : SpectrumStrip(
                          values: _live,
                          valid: _liveValidity,
                          averageValues: _average,
                          averageValid: _averageValidity,
                          rev: _liveRev,
                          accent: _k24,
                          markers: markers,
                          markerColor: _k5.withValues(alpha: 0.35),
                          showGrid: true,
                          linearTrace: false,
                          stepped: false,
                          bridgeGaps: true,
                          maxHold: true,
                        ),
                ),
              ),
            ],
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(12, 0, 12, 3),
            child: Align(
              alignment: Alignment.centerLeft,
              child: Text.rich(
                TextSpan(
                  children: [
                    TextSpan(
                      text: 'CURRENT ',
                      style: TextStyle(color: _k24),
                    ),
                    TextSpan(
                      text: 'AVG UP TO 20 SWEEPS ',
                      style: TextStyle(color: Color(0xFFFFD166)),
                    ),
                    TextSpan(
                      text:
                          '· MEAN PACKET RSSI · GAPS = VISUAL ESTIMATE, NOT MEASURED',
                      style: TextStyle(color: Colors.white30),
                    ),
                  ],
                ),
                style: TextStyle(fontSize: 9, fontFamily: 'monospace'),
              ),
            ),
          ),

          // ── Waterfall history (compact, fixed height) ───────
          SizedBox(
            height: wfH,
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                const SizedBox(width: 60),
                Expanded(
                  child: Container(
                    margin: const EdgeInsets.fromLTRB(0, 0, 12, 6),
                    decoration: BoxDecoration(
                      borderRadius: BorderRadius.circular(12),
                      border: Border.all(color: _k24.withValues(alpha: 0.18)),
                    ),
                    clipBehavior: Clip.antiAlias,
                    child: _rows.isEmpty
                        ? const Center(
                            child: Text(
                              'Building waterfall…',
                              style: TextStyle(color: Colors.white38),
                            ),
                          )
                        : WaterfallView(
                            rows: _rows,
                            validRows: _rowValidity,
                            bins: _lastLen,
                            rev: _wfRev,
                            maxRows: _kMaxRows,
                            markers: markers,
                            markerColor: _k5.withValues(alpha: 0.35),
                            smooth: true,
                            gamma: 1.0,
                          ),
                  ),
                ),
              ],
            ),
          ),

          // ── Channel axis ────────────────────────────────────
          if (ticks.isNotEmpty)
            Padding(
              padding: const EdgeInsets.fromLTRB(60, 2, 12, 2),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  for (final t in ticks)
                    Text(
                      '${chans.firstWhere((c) => c.channel == t).freq}',
                      style: const TextStyle(
                        color: Colors.white30,
                        fontSize: 9,
                        fontFamily: 'monospace',
                      ),
                    ),
                ],
              ),
            ),
          const Padding(
            padding: EdgeInsets.fromLTRB(60, 0, 12, 4),
            child: Align(
              alignment: Alignment.centerLeft,
              child: Text(
                'CHANNEL CENTER FREQUENCY (MHz) · DISCRETE CHANNEL SLOTS',
                style: TextStyle(
                  color: Colors.white30,
                  fontSize: 9,
                  fontFamily: 'monospace',
                ),
              ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(14, 2, 14, 6),
            child: Align(
              alignment: Alignment.centerLeft,
              child: WaterfallLegend(
                low: 'no RSSI sample',
                high: hasMeanRssi
                    ? 'stronger mean packet RSSI'
                    : 'stronger peak packet RSSI',
              ),
            ),
          ),

          // ── Stats bar (fps · capture rate · active · peak) ──
          Padding(
            padding: const EdgeInsets.fromLTRB(14, 0, 14, 4),
            child: _statsBar([
              ('UI FPS', '$_fps'),
              ('SWEEP/s', rate),
              ('SAMPLE AGE', sampleAge),
              ('ACTIVE', '$active/${chans.length}'),
              (
                hasMeanRssi ? 'BEST MEAN' : 'BEST RSSI',
                peak != null ? '${peak.rssi} dBm' : '—',
              ),
            ]),
          ),

          const Spacer(),

          // ── Band note + 5 GHz toggle ────────────────────────
          Padding(
            padding: const EdgeInsets.fromLTRB(14, 0, 14, 12),
            child: Row(
              children: [
                _legendDot(_k5.withValues(alpha: 0.5), '2.4 ┆ 5 GHz split'),
                const Spacer(),
                const Text(
                  '5 GHz',
                  style: TextStyle(color: Colors.white54, fontSize: 12),
                ),
                Switch(
                  value: _include5,
                  activeThumbColor: _k5,
                  onChanged: _toggle5,
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  /// Up to five evenly spaced channel numbers from the sorted channel list.
  List<int> _axisTicks(List<SpecChannel> chans) {
    final n = chans.length;
    if (n == 0) return const [];
    if (n <= 5) return [for (final c in chans) c.channel];
    return [
      for (var i = 0; i < 5; i++) chans[(i * (n - 1) / 4).round()].channel,
    ];
  }

  Widget _statsBar(List<(String, String)> items) {
    final children = <Widget>[];
    for (var i = 0; i < items.length; i++) {
      final (label, value) = items[i];
      children.add(
        RichText(
          text: TextSpan(
            children: [
              TextSpan(
                text: '$label ',
                style: const TextStyle(
                  color: Colors.white30,
                  fontSize: 10,
                  fontFamily: 'monospace',
                ),
              ),
              TextSpan(
                text: value,
                style: const TextStyle(
                  color: Colors.white70,
                  fontSize: 11,
                  fontWeight: FontWeight.bold,
                  fontFamily: 'monospace',
                ),
              ),
            ],
          ),
        ),
      );
      if (i < items.length - 1) {
        children.add(
          const Text(
            '  ·  ',
            style: TextStyle(color: Colors.white12, fontSize: 11),
          ),
        );
      }
    }
    return FittedBox(
      fit: BoxFit.scaleDown,
      child: Row(children: children),
    );
  }

  Widget _chip(String t, Color c) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
    decoration: BoxDecoration(
      color: c.withValues(alpha: 0.12),
      borderRadius: BorderRadius.circular(8),
      border: Border.all(color: c.withValues(alpha: 0.4)),
    ),
    child: Text(
      t,
      style: TextStyle(
        color: c,
        fontSize: 11,
        fontWeight: FontWeight.bold,
        fontFamily: 'monospace',
      ),
    ),
  );

  Widget _legendDot(Color c, String label) => Row(
    children: [
      Container(
        width: 10,
        height: 10,
        decoration: BoxDecoration(color: c, shape: BoxShape.circle),
      ),
      const SizedBox(width: 6),
      Text(label, style: const TextStyle(color: Colors.white54, fontSize: 12)),
    ],
  );
}
