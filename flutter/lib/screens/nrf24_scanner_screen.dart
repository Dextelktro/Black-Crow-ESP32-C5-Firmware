import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';
import '../widgets/nrf_kit.dart';
import '../widgets/waterfall.dart';

const _kAccent = Color(0xFF00E0B8);
const _kPeak = Color(0xFFFFD000);
const int _kMaxRows = 140; // waterfall history depth
const int _kChannels = 126;
const int _kAverageSweeps = 20;

/// Live BLE marker channels drawn as faint guides (nRF ch → ~BLE adv freqs).
const _markerCh = <int>[2, 26, 80];
const _markerColor = Color(0x33B06CFF);

/// nRF24 spectrum scanner. Runs the firmware RPD occupancy sweep and renders a
/// true scrolling time×frequency heatmap in near real time.
class Nrf24ScannerScreen extends StatefulWidget {
  const Nrf24ScannerScreen({super.key});

  @override
  State<Nrf24ScannerScreen> createState() => _Nrf24ScannerScreenState();
}

class _Nrf24ScannerScreenState extends State<Nrf24ScannerScreen> {
  DeviceController? _ctrl;
  Timer? _poll;

  final List<List<double>> _rows = [];
  final List<List<double>> _averageHistory = [];
  List<double> _live = List<double>.filled(_kChannels, 0);
  final List<double> _average = List<double>.filled(_kChannels, 0);
  int _lastSweep = -1;
  DateTime? _lastSweepAt;
  int _rev = 0;
  bool _refreshing = false;
  String? _lastRefreshError;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      final ctrl = context.read<DeviceController>();
      unawaited(
        ctrl.nrfWaterfallStart().catchError((Object e) {
          if (!mounted) return;
          ScaffoldMessenger.of(
            context,
          ).showSnackBar(SnackBar(content: Text('nRF scan failed: $e')));
        }),
      );
      _poll = Timer.periodic(const Duration(milliseconds: 750), (_) {
        unawaited(_refreshScan());
      });
    });
  }

  Future<void> _refreshScan() async {
    final ctrl = _ctrl;
    if (_refreshing || ctrl == null || !ctrl.nrfScanRunning) return;
    _refreshing = true;
    try {
      await ctrl.nrfScanRefresh();
      _lastRefreshError = null;
    } catch (error) {
      final message = error.toString();
      if (mounted && message != _lastRefreshError) {
        _lastRefreshError = message;
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('nRF scan refresh failed: $message')),
        );
      }
    } finally {
      _refreshing = false;
    }
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
  }

  @override
  void dispose() {
    _poll?.cancel();
    unawaited(_ctrl?.nrfWaterfallStop().catchError((Object _) {}));
    super.dispose();
  }

  /// Fold one complete firmware sweep into the waterfall.
  void _ingest(DeviceController ctrl) {
    final ch = ctrl.nrfScanChannels;
    if (ctrl.nrfScanSweep == 0 || ctrl.nrfScanSweep == _lastSweep) return;
    _lastSweep = ctrl.nrfScanSweep;
    _lastSweepAt = DateTime.now();
    final denom = ctrl.nrfScanSamples < 1 ? 1 : ctrl.nrfScanSamples;
    final row = List<double>.filled(_kChannels, 0);
    for (final e in ch) {
      if (e.channel >= 0 && e.channel < _kChannels) {
        row[e.channel] = (e.rpdDetections / denom).clamp(0.0, 1.0);
      }
    }
    _averageHistory.add(row);
    if (_averageHistory.length > _kAverageSweeps) {
      _averageHistory.removeAt(0);
    }
    for (var i = 0; i < _kChannels; i++) {
      var total = 0.0;
      for (final previous in _averageHistory) {
        total += previous[i];
      }
      _average[i] = total / _averageHistory.length;
    }
    _live = row;
    _rows.add(row);
    if (_rows.length > _kMaxRows) _rows.removeAt(0);
    _rev++;
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    _ingest(ctrl);
    final active = _live.where((v) => v > 0).length;

    // Compact scope height instead of filling the whole screen.
    final wfH = (MediaQuery.of(context).size.height * 0.34).clamp(220.0, 360.0);

    return Scaffold(
      appBar: AppBar(
        iconTheme: const IconThemeData(color: _kAccent),
        title: const Text(
          'nRF SCANNER',
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
            padding: const EdgeInsets.fromLTRB(14, 12, 14, 8),
            child: Row(
              children: [
                nrfChip(
                  'RPD CH ${ctrl.nrfScanPeakCh} · '
                  '${ctrl.nrfScanPeakRpdDetections}/${ctrl.nrfScanSamples}',
                  _kPeak,
                ),
                const SizedBox(width: 8),
                nrfChip('ACTIVE $active', _kAccent),
                nrfChip(
                  'AGE ${_lastSweepAt == null ? '—' : '${DateTime.now().difference(_lastSweepAt!).inMilliseconds}ms'}',
                  _kAccent,
                ),
                const Spacer(),
                if (ctrl.nrfScanRunning)
                  const NrfPulseDot(color: _kAccent, size: 9),
              ],
            ),
          ),

          // ── Live spectrum strip (instantaneous) ─────────────
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
                  margin: const EdgeInsets.fromLTRB(4, 0, 12, 8),
                  decoration: nrfPanel(_kAccent, glow: 0.06, radius: 12),
                  clipBehavior: Clip.antiAlias,
                  child: SpectrumStrip(
                    values: _live,
                    averageValues: _average,
                    valid: List<bool>.filled(
                      _kChannels,
                      _lastSweep > 0,
                      growable: false,
                    ),
                    rev: _rev,
                    accent: _kAccent,
                    markers: _markerCh,
                    markerColor: _markerColor,
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
                      text: 'AVG UP TO 20 SWEEPS ',
                      style: TextStyle(color: Color(0xFFFFD166)),
                    ),
                    TextSpan(
                      text: '· RPD DETECTION FRACTION',
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
                              'Building waterfall…',
                              style: TextStyle(color: Colors.white38),
                            ),
                          )
                        : WaterfallView(
                            rows: _rows,
                            bins: _kChannels,
                            rev: _rev,
                            maxRows: _kMaxRows,
                            markers: _markerCh,
                            markerColor: _markerColor,
                            smooth: true,
                            gamma: 1.0,
                          ),
                  ),
                ),
              ],
            ),
          ),

          // ── Frequency axis ──────────────────────────────────
          const Padding(
            padding: EdgeInsets.fromLTRB(60, 2, 12, 6),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                _Axis('2400'),
                _Axis('2432'),
                _Axis('2464'),
                _Axis('2496'),
                _Axis('2525'),
              ],
            ),
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(60, 0, 12, 4),
            child: Align(
              alignment: Alignment.centerLeft,
              child: Text(
                'CHANNEL CENTER FREQUENCY (MHz)',
                style: TextStyle(
                  color: Colors.white30,
                  fontSize: 9,
                  fontFamily: 'monospace',
                ),
              ),
            ),
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(14, 0, 14, 8),
            child: WaterfallLegend(
              low: '0/32 RPD detections',
              high: '32/32 RPD detections',
            ),
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(16, 0, 16, 12),
            child: Text(
              'RPD threshold detections per 32 separate RX windows, by channel '
              '(2400–2525 MHz). RPD is an occupancy proxy, not dBm. '
              'Time flows downward; purple guides mark BLE advertising channels.',
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
