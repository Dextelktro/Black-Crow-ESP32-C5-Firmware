import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../models/ble_spam_type.dart';
import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';

class BleSpamTypeScreen extends StatefulWidget {
  const BleSpamTypeScreen({
    super.key,
    required this.controller,
    required this.type,
  });

  final DeviceController controller;
  final BleSpamType type;

  @override
  State<BleSpamTypeScreen> createState() => _BleSpamTypeScreenState();
}

class _BleSpamTypeScreenState extends State<BleSpamTypeScreen> {
  late int _delayMs;
  Timer? _ticker;
  bool _initialSyncDone = false;

  @override
  void initState() {
    super.initState();
    // Start from default delay; sync with firmware state on first build
    // if spam is already running.
    _delayMs = widget.type.defaultDelay;
  }

  @override
  void dispose() {
    _ticker?.cancel();
    _ticker = null;
    super.dispose();
  }

  void _syncTicker(bool running) {
    if (running && _ticker == null) {
      _ticker = Timer.periodic(const Duration(seconds: 1), (_) {
        if (mounted) setState(() {});
      });
    } else if (!running && _ticker != null) {
      _ticker!.cancel();
      _ticker = null;
    }
  }

  void _syncFromControllerIfNeeded(DeviceController ctrl, bool running) {
    // Only sync once per screen lifetime when spam is running for this type.
    if (_initialSyncDone) return;
    if (!running) return;
    if (ctrl.bleSpamType != widget.type) return;
    _delayMs = ctrl.bleSpamDelayMs.clamp(20, 500);
    _initialSyncDone = true;
  }

  Future<void> _start(DeviceController ctrl) async {
    HapticFeedback.mediumImpact();
    // Duration: default 300s (5 min). Can be extended later.
    await ctrl.bleSpamStart(widget.type, _delayMs, 300);
  }

  Future<void> _stop(DeviceController ctrl) async {
    HapticFeedback.mediumImpact();
    await ctrl.bleSpamStop();
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final t = widget.type;
    final running = ctrl.bleSpamRunning && ctrl.bleSpamType == t;

    _syncTicker(running);
    _syncFromControllerIfNeeded(ctrl, running);

    final elapsed = (ctrl.bleSpamStartedAt != null && running)
        ? DateTime.now().difference(ctrl.bleSpamStartedAt!)
        : Duration.zero;

    return Scaffold(
      backgroundColor: const Color(0xFF010D01),
      appBar: AppBar(
        backgroundColor: Colors.black,
        title: Text(
          '${t.label.toUpperCase()} SPAM',
          style: TextStyle(
            color: t.accent,
            fontWeight: FontWeight.bold,
            letterSpacing: 2,
            fontSize: 15,
          ),
        ),
        iconTheme: IconThemeData(color: t.accent),
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          DeviceIssueBanner(controller: ctrl),
          Row(
            children: [
              Expanded(
                child: _StatCard(
                  label: 'FRAMES',
                  value: running ? '${ctrl.bleSpamFrames}' : '0',
                  accent: t.accent,
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: _StatCard(
                  label: 'FPS',
                  value: running ? '${ctrl.bleSpamFps}' : '0',
                  accent: t.accent,
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: _StatCard(
                  label: 'UPTIME',
                  value: running ? _fmt(elapsed) : '00:00',
                  accent: t.accent,
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),

          _SectionTitle('CURRENT FRAME', t.accent),
          const SizedBox(height: 6),
          Container(
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(
              color: Colors.black,
              borderRadius: BorderRadius.circular(8),
              border: Border.all(color: t.accent.withValues(alpha: 0.3)),
            ),
            child: Text(
              _hexPreview(t),
              style: const TextStyle(
                color: Color(0xFF8FFF8F),
                fontFamily: 'monospace',
                fontSize: 11,
                height: 1.5,
              ),
            ),
          ),
          const SizedBox(height: 20),

          _SectionTitle('DELAY', t.accent),
          const SizedBox(height: 6),
          Row(
            children: [
              Expanded(
                child: Slider(
                  value: _delayMs.toDouble().clamp(20, 500),
                  min: 20,
                  max: 500,
                  divisions: 24,
                  activeColor: t.accent,
                  label: '${_delayMs}ms',
                  onChanged: (v) => setState(() => _delayMs = v.round()),
                  onChangeEnd: (v) {
                    final newMs = v.round();
                    if (running) {
                      unawaited(
                        ctrl.bleSpamSetDelay(newMs).catchError(
                          (Object e, StackTrace s) {
                            debugPrint('setDelay err: $e\n$s');
                            if (!mounted) return;
                            setState(
                                () => _delayMs = ctrl.bleSpamDelayMs);
                            ScaffoldMessenger.of(context).showSnackBar(
                              SnackBar(
                                  content: Text('Unable to change delay: $e')),
                            );
                          },
                        ),
                      );
                    }
                  },
                ),
              ),
              SizedBox(
                width: 72,
                child: Text(
                  '${_delayMs}ms',
                  textAlign: TextAlign.right,
                  style: TextStyle(
                    color: t.accent,
                    fontFamily: 'monospace',
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 24),

          SizedBox(
            height: 52,
            child: ElevatedButton.icon(
              style: ElevatedButton.styleFrom(
                backgroundColor: running
                    ? const Color(0xFF8B0000)
                    : t.accent.withValues(alpha: 0.9),
                foregroundColor: running ? Colors.white : Colors.black,
                shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(10),
                ),
              ),
              onPressed: () {
                unawaited(
                  (running ? _stop(ctrl) : _start(ctrl)).catchError(
                    (Object e) {
                      if (!mounted) return;
                      ScaffoldMessenger.of(context).showSnackBar(
                        SnackBar(content: Text('Error: $e')),
                      );
                    },
                  ),
                );
              },
              icon: Icon(running ? Icons.stop : Icons.play_arrow),
              label: Text(
                running ? 'STOP SPAM' : 'START SPAM',
                style: const TextStyle(
                  fontWeight: FontWeight.bold,
                  letterSpacing: 1.5,
                ),
              ),
            ),
          ),

          if (t.warnText != null) ...[
            const SizedBox(height: 16),
            Container(
              padding: const EdgeInsets.all(10),
              decoration: BoxDecoration(
                color: const Color(0xFF2A0000),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: const Color(0xFFFF4040)),
              ),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Icon(
                    Icons.warning_amber,
                    color: Color(0xFFFF4040),
                    size: 18,
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      t.warnText!,
                      style: const TextStyle(
                        color: Color(0xFFFF8080),
                        fontSize: 11,
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ],
        ],
      ),
    );
  }

  String _fmt(Duration d) {
    final m = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final s = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    return '$m:$s';
  }

  String _hexPreview(BleSpamType t) {
    final Map<String, String> samples = {
      'apple': '1e ff 4c 00 07 19 07 02 20 75 aa 30 01 00 00 45',
      'sour': '10 ff 4c 00 01 00 00 00 00 00 00 00 00 00 00 00',
      'samsung': '42 09 81 02 14 15 03 21 01 09 ab 0c 01 46 06 3c',
      'google': '03 03 2c fe 06 16 2c fe 00 00 02 0a 00 00',
      'windows': '0a ff 06 00 01 00 00 00 00 00 00 00 00',
      'xiaomi': '15 16 95 fe 70 20 5b 04 fc 00 00 00 00 00 00',
      'android': '42 09 81 02 ... (sam) / 03 03 2c fe ... (goo)',
      'all': '(rotating all types)',
    };
    return samples[t.id] ?? '--';
  }
}

class _SectionTitle extends StatelessWidget {
  const _SectionTitle(this.text, this.accent);
  final String text;
  final Color accent;

  @override
  Widget build(BuildContext context) {
    return Text(
      text,
      style: TextStyle(
        color: accent.withValues(alpha: 0.7),
        fontSize: 10,
        fontWeight: FontWeight.bold,
        letterSpacing: 1.5,
      ),
    );
  }
}

class _StatCard extends StatelessWidget {
  const _StatCard({
    required this.label,
    required this.value,
    required this.accent,
  });
  final String label;
  final String value;
  final Color accent;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 12, horizontal: 8),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: accent.withValues(alpha: 0.3)),
      ),
      child: Column(
        children: [
          Text(
            value,
            style: TextStyle(
              color: accent,
              fontSize: 18,
              fontWeight: FontWeight.bold,
              fontFamily: 'monospace',
            ),
          ),
          const SizedBox(height: 2),
          Text(
            label,
            style: TextStyle(
              color: Colors.white.withValues(alpha: 0.5),
              fontSize: 9,
              fontFamily: 'monospace',
              letterSpacing: 1,
            ),
          ),
        ],
      ),
    );
  }
}
