import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/subghz_kit.dart';
import '../widgets/waterfall.dart';
import 'subghz_common.dart';

/// Spectrum: a swept-RSSI waterfall over a focused band. (The CC1101 is a
/// single-channel radio, so this is honest swept power, not an FFT/IQ SDR.)
class SubghzSpectrumScreen extends StatefulWidget {
  const SubghzSpectrumScreen({super.key});

  @override
  State<SubghzSpectrumScreen> createState() => _SubghzSpectrumScreenState();
}

class _SubghzSpectrumScreenState extends State<SubghzSpectrumScreen> {
  DeviceController? _ctrl;
  int _band = 4; // default to the 433 MHz region (firmware band index 4)

  // Fallback band labels if the firmware table hasn't arrived yet (help text).
  static const _fallback = ['315', '390', '415', '433', '868', '915'];

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      context.read<DeviceController>().subghzRefreshBands();
    });
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
  }

  @override
  void dispose() {
    if (_ctrl?.subghzSpectrumRunning ?? false) _ctrl!.subghzStop();
    super.dispose();
  }

  List<(int, String)> _bandChips(DeviceController ctrl) {
    if (ctrl.subghzBands.isNotEmpty) {
      return [
        for (final b in ctrl.subghzBands) (b.index, sgMhz(b.centerHz, decimals: 0)),
      ];
    }
    return [for (var i = 0; i < _fallback.length; i++) (i + 1, _fallback[i])];
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final running = ctrl.subghzSpectrumRunning;
    final frame = ctrl.subghzSpecFrame;
    final bins = frame?.bins.length ?? 64;
    final liveValues = frame == null
        ? const <double>[]
        : <double>[for (final v in frame.bins) ((v + 110) / 90.0).clamp(0.0, 1.0)];
    final chips = _bandChips(ctrl);

    return Scaffold(
      appBar: AppBar(title: const Text('Spectrum')),
      body: Column(
        children: [
          SubghzNoticeBanner(ctrl: ctrl),
          // Band picker
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 12, 12, 4),
            child: SingleChildScrollView(
              scrollDirection: Axis.horizontal,
              child: Row(
                children: [
                  for (final c in chips) ...[
                    _BandChip(
                      label: '${c.$2} MHz',
                      selected: _band == c.$1,
                      onTap: () {
                        setState(() => _band = c.$1);
                        if (running) ctrl.subghzSpectrumBand(c.$1);
                      },
                    ),
                    const SizedBox(width: 8),
                  ],
                ],
              ),
            ),
          ),
          // Peak readout
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
            child: Row(
              children: [
                Expanded(
                  child: SgStat(
                    label: 'PEAK FREQ',
                    value: frame == null
                        ? '-- MHz'
                        : '${sgMhz(frame.peakFreqHz)} MHz',
                    color: kSgAmber,
                  ),
                ),
                Expanded(
                  child: SgStat(
                    label: 'PEAK',
                    value: frame == null ? '-- dBm' : '${frame.peakDbm} dBm',
                    color: frame == null ? Colors.white38 : sgRssiColor(frame.peakDbm),
                  ),
                ),
                Expanded(
                  child: SgStat(
                    label: 'SPAN',
                    value: frame == null
                        ? '--'
                        : '${sgMhz(frame.spanHz, decimals: 1)} MHz',
                  ),
                ),
              ],
            ),
          ),
          // Waterfall + live strip
          Expanded(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(16, 6, 16, 6),
              child: ClipRRect(
                borderRadius: BorderRadius.circular(14),
                child: Column(
                  children: [
                    Expanded(
                      flex: 3,
                      child: WaterfallView(
                        rows: ctrl.subghzSpecRows,
                        bins: bins,
                        rev: ctrl.subghzSpecRev,
                        gamma: 0.7,
                        background: kSgInk,
                      ),
                    ),
                    Container(height: 1, color: Colors.white10),
                    Expanded(
                      flex: 1,
                      child: Container(
                        color: kSgInk,
                        padding: const EdgeInsets.all(6),
                        child: liveValues.isEmpty
                            ? const Center(
                                child: Text(
                                  'Start a sweep to see the band',
                                  style: TextStyle(color: Colors.white38),
                                ),
                              )
                            : SpectrumStrip(
                                values: liveValues,
                                rev: ctrl.subghzSpecRev,
                                accent: kSgAmber,
                                maxHold: true,
                              ),
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: WaterfallLegend(low: '-110 dBm', high: '-20 dBm'),
          ),
          Padding(
            padding: const EdgeInsets.all(16),
            child: SgButton(
              label: running ? 'STOP SWEEP' : 'START SWEEP',
              icon: running ? Icons.stop : Icons.play_arrow,
              danger: running,
              onPressed: () => running
                  ? ctrl.subghzStop()
                  : ctrl.subghzSpectrumBand(_band),
            ),
          ),
        ],
      ),
    );
  }
}

class _BandChip extends StatelessWidget {
  const _BandChip({
    required this.label,
    required this.selected,
    required this.onTap,
  });
  final String label;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(20),
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
          decoration: BoxDecoration(
            color: selected ? kSgAmber.withValues(alpha: 0.18) : Colors.transparent,
            borderRadius: BorderRadius.circular(20),
            border: Border.all(
              color: kSgAmber.withValues(alpha: selected ? 0.8 : 0.3),
            ),
          ),
          child: Text(
            label,
            style: TextStyle(
              color: selected ? kSgAmber : Colors.white70,
              fontSize: 13,
              fontWeight: FontWeight.bold,
              fontFamily: 'monospace',
            ),
          ),
        ),
      ),
    );
  }
}
