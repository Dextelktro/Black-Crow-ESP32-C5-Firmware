import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/subghz_kit.dart';
import '../widgets/waterfall.dart';
import 'subghz_common.dart';

/// Frequency Analyzer: hops a fixed list across every band and reports the
/// strongest frequency it hears each cycle.
class SubghzAnalyzerScreen extends StatefulWidget {
  const SubghzAnalyzerScreen({super.key});

  @override
  State<SubghzAnalyzerScreen> createState() => _SubghzAnalyzerScreenState();
}

class _SubghzAnalyzerScreenState extends State<SubghzAnalyzerScreen> {
  DeviceController? _ctrl;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
  }

  @override
  void dispose() {
    if (_ctrl?.subghzAnalyzerRunning ?? false) _ctrl!.subghzStop();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final running = ctrl.subghzAnalyzerRunning;
    final hasPeak = ctrl.subghzAnalyzerPeakHz > 0 && ctrl.subghzAnalyzerRssi > -128;
    final history = ctrl.subghzAnalyzerHistory;
    final values = <double>[for (final r in history) sgRssiFraction(r)];

    return Scaffold(
      appBar: AppBar(title: const Text('Frequency Analyzer')),
      body: Column(
        children: [
          SubghzNoticeBanner(ctrl: ctrl),
          Expanded(
            child: ListView(
              padding: const EdgeInsets.all(16),
              children: [
                // Peak readout
                Container(
                  padding: const EdgeInsets.all(20),
                  decoration: BoxDecoration(
                    borderRadius: BorderRadius.circular(18),
                    gradient: const LinearGradient(
                      colors: [kSgInk2, kSgInk],
                      begin: Alignment.topLeft,
                      end: Alignment.bottomRight,
                    ),
                    border: Border.all(color: kSgAmber.withValues(alpha: 0.4)),
                  ),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(
                        children: [
                          const Text(
                            'STRONGEST FREQUENCY',
                            style: TextStyle(
                              color: Colors.white54,
                              fontSize: 11,
                              letterSpacing: 1.4,
                              fontWeight: FontWeight.bold,
                            ),
                          ),
                          const Spacer(),
                          if (running)
                            const Text(
                              '● SWEEPING',
                              style: TextStyle(
                                color: kSgAmber,
                                fontSize: 10,
                                fontWeight: FontWeight.bold,
                                letterSpacing: 1,
                              ),
                            ),
                        ],
                      ),
                      const SizedBox(height: 10),
                      SgFreqReadout(
                        hz: hasPeak ? ctrl.subghzAnalyzerPeakHz : 0,
                        inBand: true,
                      ),
                      const SizedBox(height: 14),
                      _RssiBar(
                        dbm: hasPeak ? ctrl.subghzAnalyzerRssi : -128,
                      ),
                    ],
                  ),
                ),
                const SizedBox(height: 16),
                const SgSectionLabel('PEAK RSSI · TIME'),
                SgPanel(
                  padding: const EdgeInsets.all(10),
                  child: SizedBox(
                    height: 120,
                    child: values.isEmpty
                        ? const Center(
                            child: Text(
                              'No samples yet',
                              style: TextStyle(color: Colors.white38),
                            ),
                          )
                        : SpectrumStrip(
                            values: values,
                            rev: values.length,
                            accent: kSgAmber,
                            linearTrace: true,
                            showGrid: true,
                          ),
                  ),
                ),
                const SizedBox(height: 16),
                SgButton(
                  label: running ? 'STOP' : 'START ANALYZER',
                  icon: running ? Icons.stop : Icons.play_arrow,
                  danger: running,
                  onPressed: () =>
                      running ? ctrl.subghzStop() : ctrl.subghzAnalyzerStart(),
                ),
                const SizedBox(height: 14),
                const Text(
                  'The analyzer hops a fixed frequency list across all bands and '
                  'reports the loudest each cycle. Hold a remote button down to '
                  'pin its frequency, then set it in Radio Settings for Read.',
                  style: TextStyle(color: Colors.white38, fontSize: 11, height: 1.4),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _RssiBar extends StatelessWidget {
  const _RssiBar({required this.dbm});
  final int dbm;

  @override
  Widget build(BuildContext context) {
    final frac = sgRssiFraction(dbm);
    final color = sgRssiColor(dbm);
    final has = dbm > -128;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            const Text('RSSI',
                style: TextStyle(color: Colors.white54, fontSize: 11)),
            const Spacer(),
            Text(
              has ? '$dbm dBm' : '-- dBm',
              style: TextStyle(
                color: has ? color : Colors.white38,
                fontSize: 15,
                fontWeight: FontWeight.w800,
                fontFamily: 'monospace',
              ),
            ),
          ],
        ),
        const SizedBox(height: 6),
        ClipRRect(
          borderRadius: BorderRadius.circular(4),
          child: LinearProgressIndicator(
            value: has ? frac : 0,
            minHeight: 8,
            backgroundColor: Colors.white.withValues(alpha: 0.08),
            valueColor: AlwaysStoppedAnimation(color),
          ),
        ),
      ],
    );
  }
}
