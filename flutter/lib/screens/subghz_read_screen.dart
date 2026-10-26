import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/subghz_models.dart';
import '../services/device_controller.dart';
import '../widgets/subghz_kit.dart';
import 'subghz_common.dart';

/// Read mode: decode known fixed-code protocols from captured bursts.
class SubghzReadScreen extends StatefulWidget {
  const SubghzReadScreen({super.key});

  @override
  State<SubghzReadScreen> createState() => _SubghzReadScreenState();
}

class _SubghzReadScreenState extends State<SubghzReadScreen> {
  DeviceController? _ctrl;
  bool _sessionPrepared = false;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
    if (!_sessionPrepared) {
      _sessionPrepared = true;
      _ctrl!.subghzResetReadSession();
    }
  }

  @override
  void dispose() {
    final ctrl = _ctrl;
    if (ctrl != null) {
      closeSubghzSessionOnExit(
        ctrl.subghzCloseReadSession,
        'Sub-GHz Read',
      );
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final running = ctrl.subghzReadRunning;
    final decoded = ctrl.subghzDecoded;
    // Firmware persists the newest known decode; unknown entries can appear
    // above it in the UI, so use the first known entry for the save action.
    SubghzDecoded? lastKnown;
    for (final d in decoded) {
      if (d.isKnown) {
        lastKnown = d;
        break;
      }
    }

    return Scaffold(
      appBar: AppBar(
        title: const Text('Read · Decode'),
        actions: [
          if (decoded.isNotEmpty)
            IconButton(
              tooltip: 'Clear list',
              icon: const Icon(Icons.clear_all),
              onPressed: () => setState(ctrl.subghzResetReadSession),
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
                  label: running ? 'STOP' : 'START DECODING',
                  icon: running ? Icons.stop : Icons.play_arrow,
                  danger: running,
                  busy: false,
                  onPressed: () =>
                      running ? ctrl.subghzStop() : ctrl.subghzReadStart(),
                ),
                const SizedBox(height: 10),
                SgButton(
                  label: lastKnown == null
                      ? 'SAVE LAST DECODE'
                      : 'SAVE: ${lastKnown.proto} ${lastKnown.keyHex}',
                  icon: Icons.save_alt,
                  filled: false,
                  onPressed: lastKnown == null ? null : () => _saveLast(ctrl),
                ),
                const SizedBox(height: 18),
                Row(
                  children: [
                    const SgSectionLabel('DECODED SIGNALS'),
                    const Spacer(),
                    if (running)
                      Row(
                        children: [
                          Container(
                            width: 8,
                            height: 8,
                            decoration: const BoxDecoration(
                              color: kSgAmber,
                              shape: BoxShape.circle,
                            ),
                          ),
                          const SizedBox(width: 6),
                          const Text(
                            'LISTENING',
                            style: TextStyle(
                              color: kSgAmber,
                              fontSize: 10,
                              fontWeight: FontWeight.bold,
                              letterSpacing: 1,
                            ),
                          ),
                        ],
                      ),
                  ],
                ),
                const SizedBox(height: 4),
                if (decoded.isEmpty)
                  _Empty(running: running)
                else
                  for (final d in decoded) _DecodedCard(d: d),
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
            running ? Icons.hourglass_empty : Icons.graphic_eq,
            color: Colors.white24,
            size: 42,
          ),
          const SizedBox(height: 12),
          Text(
            running
                ? 'Waiting for a known signal…\nPress a remote near the antenna.'
                : 'Start decoding, then trigger a remote nearby.',
            textAlign: TextAlign.center,
            style: const TextStyle(color: Colors.white38, fontSize: 13, height: 1.5),
          ),
        ],
      ),
    );
  }
}

class _DecodedCard extends StatelessWidget {
  const _DecodedCard({required this.d});
  final SubghzDecoded d;

  @override
  Widget build(BuildContext context) {
    final known = d.isKnown;
    final color = known ? kSgAmber : Colors.white38;
    return Container(
      margin: const EdgeInsets.only(bottom: 10),
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.03),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: color.withValues(alpha: 0.3)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(
                known ? Icons.verified : Icons.help_outline,
                color: color,
                size: 18,
              ),
              const SizedBox(width: 8),
              Text(
                known ? d.proto : 'Unknown',
                style: TextStyle(
                  color: known ? Colors.white : Colors.white54,
                  fontSize: 15,
                  fontWeight: FontWeight.bold,
                ),
              ),
              if (known) ...[
                const SizedBox(width: 8),
                Text(
                  '${d.bits} bit',
                  style: const TextStyle(color: Colors.white38, fontSize: 11),
                ),
              ],
              const Spacer(),
              Text(
                timeAgo(d.at),
                style: const TextStyle(color: Colors.white38, fontSize: 11),
              ),
            ],
          ),
          if (known && d.keyHex.isNotEmpty) ...[
            const SizedBox(height: 10),
            Container(
              width: double.infinity,
              padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
              decoration: BoxDecoration(
                color: kSgInk,
                borderRadius: BorderRadius.circular(8),
              ),
              child: Text(
                d.keyHex,
                style: const TextStyle(
                  color: kSgAmber,
                  fontSize: 15,
                  letterSpacing: 1,
                  fontFamily: 'monospace',
                  fontWeight: FontWeight.bold,
                ),
              ),
            ),
          ],
          const SizedBox(height: 10),
          Row(
            children: [
              Expanded(
                child: SgStat(
                  label: 'TUNED FREQ',
                  value: '${sgMhz(d.freqHz)} MHz',
                ),
              ),
              Expanded(
                child: SgStat(
                  label: 'RSSI',
                  value: '${d.rssi} dBm',
                  color: sgRssiColor(d.rssi),
                ),
              ),
              if (d.te > 0)
                Expanded(child: SgStat(label: 'Te', value: '${d.te} µs')),
            ],
          ),
        ],
      ),
    );
  }
}
