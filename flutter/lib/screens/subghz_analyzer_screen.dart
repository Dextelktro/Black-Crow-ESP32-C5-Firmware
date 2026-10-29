import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/subghz_kit.dart';
import '../widgets/waterfall.dart';
import 'subghz_common.dart';

/// Frequency Analyzer: scans representative frequencies and estimates each
/// hit from a local RSSI peak.
class SubghzAnalyzerScreen extends StatefulWidget {
  const SubghzAnalyzerScreen({super.key});

  @override
  State<SubghzAnalyzerScreen> createState() => _SubghzAnalyzerScreenState();
}

class _SubghzAnalyzerScreenState extends State<SubghzAnalyzerScreen> {
  DeviceController? _ctrl;
  bool _sessionPrepared = false;
  // Live draft while dragging the trigger slider (committed on release).
  double? _triggerDraft;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
    if (!_sessionPrepared) {
      _sessionPrepared = true;
      _ctrl!.subghzResetAnalyzerSession();
    }
  }

  @override
  void dispose() {
    final ctrl = _ctrl;
    if (ctrl != null) {
      closeSubghzSessionOnExit(
        ctrl.subghzCloseAnalyzerSession,
        'Sub-GHz Frequency Analyzer',
      );
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final running = ctrl.subghzAnalyzerRunning;
    final hasPeak =
        ctrl.subghzAnalyzerPeakHz > 0 && ctrl.subghzAnalyzerRssi > -128;
    final history = ctrl.subghzAnalyzerHistory;
    final detections = ctrl.subghzAnalyzerDetections;
    final values = <double>[for (final r in history) sgRssiFraction(r)];
    final trigger = _triggerDraft ?? ctrl.subghzAnalyzerTrigger.toDouble();

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
                            'LATEST FREQUENCY ESTIMATE',
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
                      // Live level (noise floor when idle), always shown so the
                      // trigger can be set above it.
                      _RssiBar(dbm: ctrl.subghzAnalyzerRssi),
                    ],
                  ),
                ),
                const SizedBox(height: 16),
                SgSectionLabel('DETECTION TRIGGER · ${trigger.round()} dBm'),
                SgPanel(
                  padding: const EdgeInsets.fromLTRB(14, 6, 14, 10),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Slider(
                        // Full firmware trigger range (SUBGHZ_ANALYZER_TRIGGER
                        // MIN..MAX = -110..0 dBm). Auto-cal, or a noisy band,
                        // can set the trigger above -40, so the slider must
                        // reach the same ceiling or its knob would disagree
                        // with the label and the user could not raise it.
                        min: -110,
                        max: 0,
                        divisions: 110,
                        value: trigger.clamp(-110, 0),
                        label: '${trigger.round()} dBm',
                        activeColor: kSgAmber,
                        onChanged: (v) => setState(() => _triggerDraft = v),
                        onChangeEnd: (v) {
                          setState(() => _triggerDraft = v);
                          ctrl.subghzSetAnalyzerTrigger(v.round());
                        },
                      ),
                      const Text(
                        'Auto-set just above the measured noise floor when the '
                        'analyzer starts, so noise is ignored. Drag to override: '
                        'raise it if noise still shows, lower it if a weak remote '
                        'is missed. The bar above is the live level.',
                        style: TextStyle(
                          color: Colors.white38,
                          fontSize: 11,
                          height: 1.4,
                        ),
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
                const SgSectionLabel('PREVIOUS DETECTIONS'),
                SgPanel(
                  padding: const EdgeInsets.symmetric(vertical: 4),
                  child: detections.isEmpty
                      ? const Padding(
                          padding: EdgeInsets.all(12),
                          child: Text(
                            'No previous detections',
                            style: TextStyle(color: Colors.white38),
                          ),
                        )
                      : Column(
                          children: [
                            for (final detection in detections)
                              ListTile(
                                dense: true,
                                visualDensity: VisualDensity.compact,
                                leading: const Icon(
                                  Icons.radio,
                                  color: kSgAmber,
                                  size: 18,
                                ),
                                title: Text(
                                  '${sgMhz(detection.freqHz, decimals: 3)} MHz',
                                  style: const TextStyle(
                                    color: Colors.white,
                                    fontFamily: 'monospace',
                                    fontWeight: FontWeight.w700,
                                  ),
                                ),
                                subtitle: Text(
                                  '${detection.rssi} dBm · '
                                  '${detection.at.hour.toString().padLeft(2, '0')}:'
                                  '${detection.at.minute.toString().padLeft(2, '0')}:'
                                  '${detection.at.second.toString().padLeft(2, '0')}',
                                  style: const TextStyle(
                                    color: Colors.white54,
                                    fontSize: 11,
                                  ),
                                ),
                              ),
                          ],
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
                  'Coarse hop at 650 kHz, then a 20 kHz fine scan around the '
                  'peak. Hold the transmitter active while scanning. RSSI peak '
                  'is an estimate, not a calibrated carrier measurement; signals '
                  'between candidates or close together may be missed or merged.',
                  style: TextStyle(
                    color: Colors.white38,
                    fontSize: 11,
                    height: 1.4,
                  ),
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
            const Text(
              'RSSI',
              style: TextStyle(color: Colors.white54, fontSize: 11),
            ),
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
