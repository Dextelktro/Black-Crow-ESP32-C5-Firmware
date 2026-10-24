import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/subghz_models.dart';
import '../services/device_controller.dart';
import '../widgets/subghz_kit.dart';
import 'subghz_common.dart';

/// Read RAW mode: capture any OOK burst as a pulse train, replay it, and save
/// it to a `.sub` RAW file.
class SubghzReadRawScreen extends StatefulWidget {
  const SubghzReadRawScreen({super.key});

  @override
  State<SubghzReadRawScreen> createState() => _SubghzReadRawScreenState();
}

class _SubghzReadRawScreenState extends State<SubghzReadRawScreen> {
  DeviceController? _ctrl;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
  }

  @override
  void dispose() {
    if (_ctrl?.subghzReadRawRunning ?? false) _ctrl!.subghzStop();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final running = ctrl.subghzReadRawRunning;
    final bursts = ctrl.subghzBursts;
    final hasCapture = bursts.isNotEmpty;
    final txBusy = ctrl.subghzTxRunning;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Read RAW'),
        actions: [
          if (bursts.isNotEmpty)
            IconButton(
              tooltip: 'Clear list',
              icon: const Icon(Icons.clear_all),
              onPressed: () => setState(ctrl.subghzBursts.clear),
            ),
        ],
      ),
      body: Column(
        children: [
          SubghzNoticeBanner(ctrl: ctrl),
          Expanded(
            child: ListView(
              padding: const EdgeInsets.all(16),
              children: [
                SubghzConfigStrip(ctrl: ctrl),
                const SizedBox(height: 14),
                SgButton(
                  label: running ? 'STOP CAPTURE' : 'START CAPTURE',
                  icon: running ? Icons.stop : Icons.fiber_manual_record,
                  danger: running,
                  onPressed: () =>
                      running ? ctrl.subghzStop() : ctrl.subghzReadRawStart(),
                ),
                const SizedBox(height: 10),
                Row(
                  children: [
                    Expanded(
                      child: SgButton(
                        label: 'TRANSMIT LAST',
                        icon: Icons.wifi_tethering,
                        filled: false,
                        busy: txBusy,
                        onPressed: hasCapture && !running && !txBusy
                            ? () => ctrl.subghzTransmitLast()
                            : null,
                      ),
                    ),
                    const SizedBox(width: 10),
                    Expanded(
                      child: SgButton(
                        label: 'SAVE LAST',
                        icon: Icons.save_alt,
                        filled: false,
                        onPressed:
                            hasCapture && !txBusy ? () => _saveLast(ctrl) : null,
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 18),
                Row(
                  children: [
                    const SgSectionLabel('CAPTURED BURSTS'),
                    const Spacer(),
                    if (running)
                      const Text(
                        '● REC',
                        style: TextStyle(
                          color: kSgDanger,
                          fontSize: 11,
                          fontWeight: FontWeight.bold,
                          letterSpacing: 1,
                        ),
                      ),
                  ],
                ),
                const SizedBox(height: 4),
                if (bursts.isEmpty)
                  _Empty(running: running)
                else
                  for (var i = 0; i < bursts.length; i++)
                    _BurstCard(burst: bursts[i], latest: i == 0),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Future<void> _saveLast(DeviceController ctrl) async {
    final name = await promptSubghzName(context, ctrl);
    if (name == null) return;
    if (ctrl.subghzReadRawRunning) await ctrl.subghzStop();
    await Future<void>.delayed(const Duration(milliseconds: 150));
    await ctrl.subghzSave(name);
  }
}

class _Empty extends StatelessWidget {
  const _Empty({required this.running});
  final bool running;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 40),
      alignment: Alignment.center,
      child: Column(
        children: [
          Icon(
            running ? Icons.graphic_eq : Icons.waves,
            color: Colors.white24,
            size: 42,
          ),
          const SizedBox(height: 12),
          Text(
            running
                ? 'Listening… trigger a remote near the antenna.'
                : 'Start a capture, then press a remote nearby.',
            textAlign: TextAlign.center,
            style: const TextStyle(color: Colors.white38, fontSize: 13, height: 1.5),
          ),
        ],
      ),
    );
  }
}

class _BurstCard extends StatelessWidget {
  const _BurstCard({required this.burst, required this.latest});
  final SubghzRawBurst burst;
  final bool latest;

  @override
  Widget build(BuildContext context) {
    final color = latest ? kSgAmber : Colors.white24;
    return Container(
      margin: const EdgeInsets.only(bottom: 10),
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.03),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: color.withValues(alpha: 0.35)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              if (latest)
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                  decoration: BoxDecoration(
                    color: kSgAmber.withValues(alpha: 0.18),
                    borderRadius: BorderRadius.circular(5),
                  ),
                  child: const Text(
                    'LAST',
                    style: TextStyle(
                      color: kSgAmber,
                      fontSize: 9,
                      fontWeight: FontWeight.bold,
                      letterSpacing: 1,
                    ),
                  ),
                ),
              if (latest) const SizedBox(width: 8),
              Text(
                '#${burst.seq}',
                style: const TextStyle(
                  color: Colors.white54,
                  fontSize: 12,
                  fontFamily: 'monospace',
                ),
              ),
              if (burst.truncated) ...[
                const SizedBox(width: 8),
                const Icon(Icons.warning_amber, color: kSgDanger, size: 14),
                const SizedBox(width: 2),
                const Text(
                  'truncated',
                  style: TextStyle(color: kSgDanger, fontSize: 10),
                ),
              ],
              const Spacer(),
              Text(
                timeAgo(burst.at),
                style: const TextStyle(color: Colors.white38, fontSize: 11),
              ),
            ],
          ),
          const SizedBox(height: 10),
          PulseTrain(durations: burst.durations),
          const SizedBox(height: 10),
          Row(
            children: [
              Expanded(child: SgStat(label: 'PULSES', value: '${burst.count}')),
              Expanded(
                child: SgStat(label: 'FREQ', value: '${sgMhz(burst.freqHz)} MHz'),
              ),
              Expanded(
                child: SgStat(
                  label: 'RSSI',
                  value: '${burst.rssi} dBm',
                  color: sgRssiColor(burst.rssi),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}
