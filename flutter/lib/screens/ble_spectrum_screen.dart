import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';
import '../widgets/waterfall.dart';

const _kAccent = Color(0xFF00E5FF);
const int _kBins = 10;
const int _kMaxRows = 140;
const int _kAverageFrames = 20;

class BleSpectrumScreen extends StatefulWidget {
  const BleSpectrumScreen({super.key});

  @override
  State<BleSpectrumScreen> createState() => _BleSpectrumScreenState();
}

class _BleSpectrumScreenState extends State<BleSpectrumScreen> {
  DeviceController? _ctrl;

  final List<List<double>> _rows = [];
  final List<List<bool>> _rowValidity = [];
  final List<List<double>> _averageHistory = [];
  List<double> _live = List<double>.filled(_kBins, 0);
  final List<double> _average = List<double>.filled(_kBins, 0);
  List<bool> _liveValid = List<bool>.filled(_kBins, false);
  int _lastFrameId = 0;
  DateTime? _lastFrameAt;
  int _rev = 0;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      final ctrl = context.read<DeviceController>();
      unawaited(
        ctrl.bleSpectrumStart().catchError((Object e) {
          if (!mounted) return;
          ScaffoldMessenger.of(
            context,
          ).showSnackBar(SnackBar(content: Text('BLE spectrum failed: $e')));
        }),
      );
    });
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
  }

  @override
  void dispose() {
    unawaited(_ctrl?.bleSpectrumStop().catchError((Object _) {}));
    super.dispose();
  }

  /// Each frame shows the share of received reports in every RSSI bucket,
  /// separating signal distribution from advertising traffic volume.
  void _ingest(DeviceController ctrl) {
    final bins = ctrl.bleSpectrumBins;
    if (ctrl.bleSpectrumFrameId == 0 ||
        ctrl.bleSpectrumFrameId == _lastFrameId) {
      return;
    }
    _lastFrameId = ctrl.bleSpectrumFrameId;
    _lastFrameAt = DateTime.now();
    final row = List<double>.filled(_kBins, 0);
    final hasSamples = ctrl.bleSpectrumTotal > 0;
    for (var i = 0; i < _kBins && i < bins.length; i++) {
      row[i] = hasSamples ? bins[i] / ctrl.bleSpectrumTotal : 0;
    }
    if (hasSamples) {
      _averageHistory.add(row);
      if (_averageHistory.length > _kAverageFrames) {
        _averageHistory.removeAt(0);
      }
    }
    for (var i = 0; i < _kBins; i++) {
      var total = 0.0;
      for (final previous in _averageHistory) {
        total += previous[i];
      }
      _average[i] = _averageHistory.isEmpty
          ? 0
          : total / _averageHistory.length;
    }
    _live = row;
    _liveValid = List<bool>.filled(_kBins, hasSamples, growable: false);
    _rows.add(row);
    _rowValidity.add(List<bool>.of(_liveValid));
    if (_rows.length > _kMaxRows) _rows.removeAt(0);
    if (_rowValidity.length > _kMaxRows) _rowValidity.removeAt(0);
    _rev++;
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    _ingest(ctrl);

    // Compact scope height instead of filling the whole screen.
    final wfH = (MediaQuery.of(context).size.height * 0.34).clamp(220.0, 360.0);

    return Scaffold(
      backgroundColor: const Color(0xFF02090D),
      appBar: AppBar(
        backgroundColor: Colors.black,
        iconTheme: const IconThemeData(color: _kAccent),
        title: const Text(
          'BLE SPECTRUM',
          style: TextStyle(
            color: _kAccent,
            fontWeight: FontWeight.bold,
            letterSpacing: 2,
            fontSize: 16,
          ),
        ),
      ),
      body: Column(
        children: [
          DeviceIssueBanner(controller: ctrl),
          Padding(
            padding: const EdgeInsets.fromLTRB(14, 12, 14, 6),
            child: Wrap(
              spacing: 7,
              runSpacing: 6,
              crossAxisAlignment: WrapCrossAlignment.center,
              children: [
                _chip(
                  ctrl.bleSpectrumTotal > 0
                      ? ctrl.bleSpectrumMean == null
                            ? 'PEAK ${ctrl.bleSpectrumPeak} dBm'
                            : 'MEAN ${ctrl.bleSpectrumMean} · '
                                  'PK ${ctrl.bleSpectrumPeak} dBm'
                      : 'MEAN — · PK —',
                  const Color(0xFF39FF14),
                ),
                _chip('${ctrl.bleSpectrumTotal} REPORTS / 150ms', _kAccent),
                _chip(
                  'AGE ${_lastFrameAt == null ? '—' : '${DateTime.now().difference(_lastFrameAt!).inMilliseconds}ms'}',
                  _kAccent,
                ),
                if (ctrl.bleSpectrumRunning)
                  const SizedBox(
                    width: 15,
                    height: 15,
                    child: CircularProgressIndicator(
                      strokeWidth: 2,
                      color: _kAccent,
                    ),
                  ),
              ],
            ),
          ),

          // ── Live level strip ────────────────────────────────
          Row(
            children: [
              const Padding(
                padding: EdgeInsets.only(left: 8),
                child: SpectrumYAxis(
                  labels: ['100%', '75%', '50%', '25%', '0%'],
                ),
              ),
              Expanded(
                child: Container(
                  height: 110,
                  margin: const EdgeInsets.fromLTRB(4, 4, 12, 8),
                  decoration: BoxDecoration(
                    borderRadius: BorderRadius.circular(12),
                    border: Border.all(color: _kAccent.withValues(alpha: 0.18)),
                  ),
                  clipBehavior: Clip.antiAlias,
                  child: SpectrumStrip(
                    values: _live,
                    averageValues: _average,
                    valid: _liveValid,
                    rev: _rev,
                    accent: _kAccent,
                    showGrid: true,
                    linearTrace: false,
                    stepped: false,
                    maxHold: true,
                  ),
                ),
              ),
            ],
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(12, 0, 12, 4),
            child: Align(
              alignment: Alignment.centerLeft,
              child: Text.rich(
                TextSpan(
                  children: [
                    TextSpan(
                      text: 'CURRENT ',
                      style: TextStyle(color: _kAccent),
                    ),
                    TextSpan(
                      text: 'AVG UP TO 20 FRAMES ',
                      style: TextStyle(color: Color(0xFFFFD166)),
                    ),
                    TextSpan(
                      text: '· SHARE OF REPORTS BY RSSI BUCKET',
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
                      border: Border.all(
                        color: _kAccent.withValues(alpha: 0.18),
                      ),
                    ),
                    clipBehavior: Clip.antiAlias,
                    child: _rows.isEmpty
                        ? const Center(
                            child: Text(
                              'Listening…',
                              style: TextStyle(color: Colors.white38),
                            ),
                          )
                        : WaterfallView(
                            rows: _rows,
                            bins: _kBins,
                            rev: _rev,
                            maxRows: _kMaxRows,
                            validRows: _rowValidity,
                            smooth: true,
                            gamma: 1.0,
                          ),
                  ),
                ),
              ],
            ),
          ),

          // ── Signal-level axis ───────────────────────────────
          const Padding(
            padding: EdgeInsets.fromLTRB(60, 2, 12, 4),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                _Axis('≤−100'),
                _Axis('−88'),
                _Axis('−76'),
                _Axis('−64'),
                _Axis('−52'),
                _Axis('≥−46'),
              ],
            ),
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(60, 0, 12, 4),
            child: Align(
              alignment: Alignment.centerLeft,
              child: Text(
                'ADVERTISEMENT RSSI BUCKET (dBm) · NOT FREQUENCY',
                style: TextStyle(
                  color: Colors.white30,
                  fontSize: 9,
                  fontFamily: 'monospace',
                ),
              ),
            ),
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(14, 0, 14, 6),
            child: WaterfallLegend(
              low: 'no reports',
              high: 'all reports in bucket',
            ),
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(16, 0, 16, 14),
            child: Text(
              'Each frame shows the percentage of received advertisement '
              'reports in each RSSI bucket. Mean/peak RSSI summarize received '
              'packets; report volume is shown separately. This is not RF FFT. '
              'X = weaker to stronger RSSI, not frequency. Time flows downward.',
              style: TextStyle(
                color: Colors.white38,
                fontSize: 11,
                height: 1.4,
              ),
            ),
          ),
        ],
      ),
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
}

class _Axis extends StatelessWidget {
  const _Axis(this.t);
  final String t;
  @override
  Widget build(BuildContext context) => Text(
    t,
    style: const TextStyle(
      color: Colors.white30,
      fontSize: 9,
      fontFamily: 'monospace',
    ),
  );
}
