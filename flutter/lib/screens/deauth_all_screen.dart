import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';

const _kAccent = Color(0xFFFF4800);

class DeauthAllScreen extends StatelessWidget {
  const DeauthAllScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Deauth All')),
      body: const DeauthAllBody(),
    );
  }
}

/// Deauth-All screen body. The Home route provides the shared controller.
class DeauthAllBody extends StatefulWidget {
  const DeauthAllBody({super.key});

  @override
  State<DeauthAllBody> createState() => _DeauthAllBodyState();
}

class _DeauthAllBodyState extends State<DeauthAllBody> {
  final _durCtrl = TextEditingController(text: '60');
  Timer? _ticker;

  String _mode = 'skip'; // 'skip' or 'unicast'
  bool _autoRescan = false;
  int _rescanInterval = 15; // seconds (5..60)
  Timer? _rescanTimer;

  @override
  void dispose() {
    _ticker?.cancel();
    _ticker = null;
    _rescanTimer?.cancel();
    _rescanTimer = null;
    _durCtrl.dispose();
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

  void _syncAutoRescan(bool running) {
    final shouldRun = running && _autoRescan;
    if (shouldRun && _rescanTimer == null) {
      _rescanTimer = Timer.periodic(
        Duration(seconds: _rescanInterval),
        _autoRescanTick,
      );
    } else if (!shouldRun && _rescanTimer != null) {
      _rescanTimer!.cancel();
      _rescanTimer = null;
    }
  }

  void _restartAutoRescanTimer() {
    _rescanTimer?.cancel();
    _rescanTimer = null;
    final ctrl = context.read<DeviceController>();
    final running = ctrl.attacking && ctrl.attackMode == 'deauthall';
    if (running && _autoRescan) {
      _rescanTimer = Timer.periodic(
        Duration(seconds: _rescanInterval),
        _autoRescanTick,
      );
    }
  }

  void _autoRescanTick(Timer _) {
    if (!mounted) return;
    final ctrl = context.read<DeviceController>();
    final running = ctrl.attacking && ctrl.attackMode == 'deauthall';
    if (!running || !_autoRescan || ctrl.deauthAllScanning) return;
    _runAction(ctrl.deauthAllRescan());
  }

  Future<void> _start() async {
    HapticFeedback.mediumImpact();
    final secs = int.tryParse(_durCtrl.text) ?? 60;
    final ctrl = context.read<DeviceController>();
    await ctrl.deauthAll(secs.clamp(1, 3600), mode: _mode);
  }

  Future<void> _stop() async {
    HapticFeedback.mediumImpact();
    await context.read<DeviceController>().deauthAllStop();
  }

  Future<void> _rescan() async {
    HapticFeedback.lightImpact();
    await context.read<DeviceController>().deauthAllRescan();
  }

  void _runAction(Future<void> action) {
    unawaited(
      action.catchError((Object error, StackTrace stackTrace) {
        debugPrint('Deauth-All action failed: $error\n$stackTrace');
        if (!mounted) return;
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Deauth-All action failed: $error')),
        );
      }),
    );
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final running = ctrl.attacking && ctrl.attackMode == 'deauthall';
    final scanning = ctrl.deauthAllScanning;

    _syncTicker(running);
    _syncAutoRescan(running);

    final elapsed = (ctrl.deauthAllStartedAt != null && running)
        ? DateTime.now().difference(ctrl.deauthAllStartedAt!)
        : Duration.zero;

    return Container(
      color: const Color(0xFF0A0300),
      child: Column(
        children: [
          DeviceIssueBanner(controller: ctrl),

          // ── Title ──────────────────────────────────────────────
          Padding(
            padding: const EdgeInsets.fromLTRB(14, 12, 14, 4),
            child: Row(
              children: [
                Container(
                  width: 10,
                  height: 10,
                  decoration: BoxDecoration(
                    color: _kAccent,
                    shape: BoxShape.circle,
                    boxShadow: running
                        ? const [BoxShadow(color: _kAccent, blurRadius: 10)]
                        : null,
                  ),
                ),
                const SizedBox(width: 10),
                const Text(
                  'DEAUTH ALL',
                  style: TextStyle(
                    color: _kAccent,
                    fontSize: 20,
                    fontWeight: FontWeight.w800,
                    letterSpacing: 2.5,
                    fontFamily: 'monospace',
                  ),
                ),
                const Spacer(),
                if (running) ...[
                  Container(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 8,
                      vertical: 3,
                    ),
                    decoration: BoxDecoration(
                      color: _kAccent.withValues(alpha: 0.15),
                      borderRadius: BorderRadius.circular(20),
                      border: Border.all(
                        color: _kAccent.withValues(alpha: 0.4),
                      ),
                    ),
                    child: Text(
                      ctrl.deauthAllMode.toUpperCase(),
                      style: const TextStyle(
                        color: _kAccent,
                        fontSize: 10,
                        fontWeight: FontWeight.bold,
                        letterSpacing: 1.2,
                        fontFamily: 'monospace',
                      ),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Text(
                    '${elapsed.inSeconds}s',
                    style: const TextStyle(
                      color: Color(0xFFFF9040),
                      fontFamily: 'monospace',
                      fontSize: 13,
                    ),
                  ),
                ],
              ],
            ),
          ),

          // ── Stats: CH · FRAMES · FPS · CYCLES ──────────────────
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 6, 12, 8),
            child: Row(
              children: [
                Expanded(
                  child: _StatBox(
                    label: 'CH',
                    value: running ? '${ctrl.deauthAllCurrentChannel}' : '—',
                    accent: _kAccent,
                  ),
                ),
                const SizedBox(width: 6),
                Expanded(
                  child: _StatBox(
                    label: 'FRAMES',
                    value: '${ctrl.deauthAllPkts}',
                    accent: _kAccent,
                  ),
                ),
                const SizedBox(width: 6),
                Expanded(
                  child: _StatBox(
                    label: 'FPS',
                    value: running ? '${ctrl.deauthAllPps}' : '0',
                    accent: _kAccent,
                  ),
                ),
                const SizedBox(width: 6),
                Expanded(
                  child: _StatBox(
                    label: 'CYCLES',
                    value: '${ctrl.deauthAllCycles}',
                    accent: _kAccent,
                  ),
                ),
              ],
            ),
          ),

          // ── Channel hopper (perpindahan channel) ───────────────
          Container(
            margin: const EdgeInsets.symmetric(horizontal: 12),
            padding: const EdgeInsets.symmetric(vertical: 10, horizontal: 8),
            decoration: BoxDecoration(
              color: Colors.black.withValues(alpha: 0.5),
              borderRadius: BorderRadius.circular(8),
              border: Border.all(color: _kAccent.withValues(alpha: 0.3)),
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Text(
                  'CHANNEL HOPPER',
                  style: TextStyle(
                    color: Color(0xFFFF9040),
                    fontSize: 9,
                    fontWeight: FontWeight.bold,
                    letterSpacing: 1.4,
                  ),
                ),
                const SizedBox(height: 6),
                _ChannelRow(currentChannel: ctrl.deauthAllCurrentChannel),
              ],
            ),
          ),

          const SizedBox(height: 8),

          // ── Live frames ────────────────────────────────────────
          Expanded(
            child: Container(
              margin: const EdgeInsets.symmetric(horizontal: 12),
              padding: const EdgeInsets.all(8),
              decoration: BoxDecoration(
                color: Colors.black.withValues(alpha: 0.5),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: _kAccent.withValues(alpha: 0.2)),
              ),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'LIVE FRAMES',
                    style: TextStyle(
                      color: Color(0xFFFF9040),
                      fontSize: 9,
                      fontWeight: FontWeight.bold,
                      letterSpacing: 1.4,
                    ),
                  ),
                  const SizedBox(height: 4),
                  Expanded(
                    child: ctrl.deauthAllFrames.isEmpty
                        ? const Center(
                            child: Text(
                              '(no frames yet)',
                              style: TextStyle(
                                color: Colors.white24,
                                fontFamily: 'monospace',
                                fontSize: 11,
                              ),
                            ),
                          )
                        : ListView.builder(
                            itemCount: ctrl.deauthAllFrames.length,
                            itemBuilder: (_, i) {
                              final f = ctrl.deauthAllFrames[i];
                              final opacity =
                                  (1.0 - i / ctrl.deauthAllFrames.length).clamp(
                                    0.15,
                                    1.0,
                                  );
                              return Padding(
                                padding: const EdgeInsets.symmetric(
                                  vertical: 1,
                                ),
                                child: Text(
                                  'ch=${f.channel.toString().padLeft(3)} '
                                  'dst=${f.dst} '
                                  'src=${f.src} '
                                  'r=0x${f.reason.toRadixString(16).padLeft(2, '0')}',
                                  style: TextStyle(
                                    color: const Color(
                                      0xFFFFA080,
                                    ).withValues(alpha: opacity),
                                    fontFamily: 'monospace',
                                    fontSize: 10,
                                  ),
                                ),
                              );
                            },
                          ),
                  ),
                ],
              ),
            ),
          ),

          // ── Controls ───────────────────────────────────────────
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 8, 12, 12),
            child: Column(
              children: [
                if (!running) _modeSelector(),
                if (!running) const SizedBox(height: 8),
                if (!running)
                  Row(
                    children: [
                      SizedBox(
                        width: 90,
                        child: TextField(
                          controller: _durCtrl,
                          keyboardType: TextInputType.number,
                          style: const TextStyle(color: Colors.white),
                          decoration: const InputDecoration(
                            labelText: 'Secs',
                            labelStyle: TextStyle(color: Colors.white54),
                            isDense: true,
                            border: OutlineInputBorder(),
                          ),
                          inputFormatters: [
                            FilteringTextInputFormatter.digitsOnly,
                          ],
                        ),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: SizedBox(
                          height: 48,
                          child: ElevatedButton.icon(
                            style: ElevatedButton.styleFrom(
                              backgroundColor: const Color(0xFFCC3300),
                              foregroundColor: Colors.white,
                              shape: RoundedRectangleBorder(
                                borderRadius: BorderRadius.circular(8),
                              ),
                            ),
                            onPressed: () => _runAction(_start()),
                            icon: const Icon(Icons.bolt),
                            label: const Text(
                              'START',
                              style: TextStyle(
                                fontWeight: FontWeight.bold,
                                letterSpacing: 1.4,
                              ),
                            ),
                          ),
                        ),
                      ),
                    ],
                  )
                else
                  SizedBox(
                    width: double.infinity,
                    height: 48,
                    child: ElevatedButton.icon(
                      style: ElevatedButton.styleFrom(
                        backgroundColor: const Color(0xFF8B0000),
                        foregroundColor: Colors.white,
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(8),
                        ),
                      ),
                      onPressed: () => _runAction(_stop()),
                      icon: const Icon(Icons.stop),
                      label: Text(
                        'STOP (${elapsed.inSeconds}s)',
                        style: const TextStyle(
                          fontWeight: FontWeight.bold,
                          letterSpacing: 1.4,
                        ),
                      ),
                    ),
                  ),
                const SizedBox(height: 8),
                // ── Manual rescan ─────────────────────────────
                SizedBox(
                  width: double.infinity,
                  height: 40,
                  child: OutlinedButton.icon(
                    style: OutlinedButton.styleFrom(
                      side: BorderSide(
                        color: running
                            ? const Color(0xFFFFB000)
                            : Colors.white24,
                      ),
                      foregroundColor: running
                          ? const Color(0xFFFFB000)
                          : Colors.white38,
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(8),
                      ),
                    ),
                    onPressed: (running && !scanning)
                        ? () => _runAction(_rescan())
                        : null,
                    icon: scanning
                        ? const SizedBox(
                            width: 14,
                            height: 14,
                            child: CircularProgressIndicator(
                              strokeWidth: 2,
                              color: Color(0xFFFFB000),
                            ),
                          )
                        : const Icon(Icons.refresh, size: 16),
                    label: Text(
                      scanning ? 'SCANNING…' : 'RESCAN (MANUAL)',
                      style: const TextStyle(
                        fontWeight: FontWeight.bold,
                        letterSpacing: 1.4,
                        fontSize: 12,
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 4),
                // ── Auto rescan toggle + interval slider ──────
                _autoRescanControl(running),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _modeSelector() {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: _kAccent.withValues(alpha: 0.3)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            'MODE',
            style: TextStyle(
              color: Color(0xFFFF9040),
              fontSize: 9,
              fontWeight: FontWeight.bold,
              letterSpacing: 1.4,
            ),
          ),
          const SizedBox(height: 6),
          Row(
            children: [
              _modeChip('skip', 'SKIP'),
              const SizedBox(width: 8),
              _modeChip('unicast', 'UNICAST'),
            ],
          ),
          const SizedBox(height: 4),
          Text(
            _mode == 'unicast'
                ? 'Downgrades to per-STA unicast on APs with a whitelisted '
                      'client — run a sniff first or it falls back to broadcast.'
                : 'Skips any AP that has a whitelisted client associated.',
            style: const TextStyle(color: Colors.white38, fontSize: 10),
          ),
        ],
      ),
    );
  }

  Widget _modeChip(String id, String label) {
    final sel = _mode == id;
    return Expanded(
      child: InkWell(
        onTap: () => setState(() => _mode = id),
        borderRadius: BorderRadius.circular(8),
        child: Container(
          padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 8),
          decoration: BoxDecoration(
            color: sel ? _kAccent.withValues(alpha: 0.18) : Colors.transparent,
            borderRadius: BorderRadius.circular(8),
            border: Border.all(
              color: sel ? _kAccent : Colors.white24,
              width: sel ? 1.5 : 1,
            ),
          ),
          child: Row(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Icon(
                sel ? Icons.radio_button_checked : Icons.radio_button_off,
                size: 14,
                color: sel ? _kAccent : Colors.white38,
              ),
              const SizedBox(width: 6),
              Text(
                label,
                style: TextStyle(
                  color: sel ? _kAccent : Colors.white54,
                  fontSize: 12,
                  fontWeight: FontWeight.bold,
                  letterSpacing: 1,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _autoRescanControl(bool running) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(
          color: _autoRescan
              ? const Color(0xFFFFB000).withValues(alpha: 0.5)
              : Colors.white12,
        ),
      ),
      child: Column(
        children: [
          Row(
            children: [
              Icon(
                Icons.autorenew,
                size: 18,
                color: _autoRescan ? const Color(0xFFFFB000) : Colors.white38,
              ),
              const SizedBox(width: 8),
              const Expanded(
                child: Text(
                  'AUTO RESCAN',
                  style: TextStyle(
                    color: Colors.white70,
                    fontSize: 12,
                    fontWeight: FontWeight.bold,
                    letterSpacing: 1.2,
                  ),
                ),
              ),
              Text(
                '${_rescanInterval}s',
                style: const TextStyle(
                  color: Color(0xFFFFB000),
                  fontFamily: 'monospace',
                  fontWeight: FontWeight.bold,
                ),
              ),
              const SizedBox(width: 6),
              Switch(
                value: _autoRescan,
                activeThumbColor: const Color(0xFFFFB000),
                onChanged: (v) {
                  setState(() => _autoRescan = v);
                  _restartAutoRescanTimer();
                },
              ),
            ],
          ),
          if (_autoRescan)
            SliderTheme(
              data: SliderTheme.of(context).copyWith(
                trackHeight: 3,
                overlayShape: const RoundSliderOverlayShape(overlayRadius: 14),
              ),
              child: Slider(
                value: _rescanInterval.toDouble(),
                min: 5,
                max: 60,
                divisions: 11,
                activeColor: const Color(0xFFFFB000),
                label: '${_rescanInterval}s',
                onChanged: (v) => setState(() => _rescanInterval = v.round()),
                onChangeEnd: (_) => _restartAutoRescanTimer(),
              ),
            ),
        ],
      ),
    );
  }
}

class _StatBox extends StatelessWidget {
  const _StatBox({
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
      padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 4),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.5),
        borderRadius: BorderRadius.circular(6),
        border: Border.all(color: accent.withValues(alpha: 0.3)),
      ),
      child: Column(
        children: [
          FittedBox(
            fit: BoxFit.scaleDown,
            child: Text(
              value,
              style: TextStyle(
                color: accent,
                fontSize: 16,
                fontWeight: FontWeight.bold,
                fontFamily: 'monospace',
              ),
            ),
          ),
          Text(
            label,
            style: TextStyle(
              color: Colors.white.withValues(alpha: 0.4),
              fontSize: 8,
              letterSpacing: 1,
            ),
          ),
        ],
      ),
    );
  }
}

class _ChannelRow extends StatelessWidget {
  const _ChannelRow({required this.currentChannel});
  final int currentChannel;

  static const _channels = [
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    36,
    40,
    44,
    48,
    52,
    56,
    60,
    64,
    100,
    104,
    108,
    112,
    116,
    120,
    124,
    128,
    132,
    136,
    140,
    144,
    149,
    153,
    157,
    161,
    165,
  ];

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      scrollDirection: Axis.horizontal,
      child: Row(
        children: _channels.map((ch) {
          final isActive = ch == currentChannel;
          final is5ghz = ch > 14;
          final color = isActive
              ? _kAccent
              : (is5ghz
                    ? Colors.purpleAccent.withValues(alpha: 0.3)
                    : Colors.tealAccent.withValues(alpha: 0.3));
          return Container(
            margin: const EdgeInsets.symmetric(horizontal: 1.5),
            padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 2),
            decoration: BoxDecoration(
              color: isActive
                  ? color.withValues(alpha: 0.8)
                  : color.withValues(alpha: 0.1),
              borderRadius: BorderRadius.circular(3),
              boxShadow: isActive
                  ? [
                      BoxShadow(
                        color: color.withValues(alpha: 0.6),
                        blurRadius: 6,
                      ),
                    ]
                  : null,
            ),
            child: Text(
              '$ch',
              style: TextStyle(
                color: isActive ? Colors.white : color,
                fontSize: 8,
                fontWeight: isActive ? FontWeight.bold : FontWeight.normal,
                fontFamily: 'monospace',
              ),
            ),
          );
        }).toList(),
      ),
    );
  }
}
