import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';
import 'wifi_spam_screen.dart';

class WifiSpamModeScreen extends StatefulWidget {
  const WifiSpamModeScreen({super.key, required this.mode});
  final WspamMode mode;

  @override
  State<WifiSpamModeScreen> createState() => _WifiSpamModeScreenState();
}

class _WifiSpamModeScreenState extends State<WifiSpamModeScreen> {
  String _rate = 'aggressive';
  bool _dual = true;
  final _customCtrl = TextEditingController();
  Timer? _ticker;

  bool get _isCustom => widget.mode.id == 'custom';

  @override
  void initState() {
    super.initState();
    _ticker = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
    if (_isCustom) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted) return;
        unawaited(context
            .read<DeviceController>()
            .wspamListCustom()
            .catchError((Object _) {}));
      });
    }
  }

  @override
  void dispose() {
    _ticker?.cancel();
    _customCtrl.dispose();
    super.dispose();
  }

  Color get _accent => widget.mode.accent;

  void _run(Future<void> f) {
    unawaited(f.catchError((Object e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('WiFi spam error: $e')),
      );
    }));
  }

  void _start() {
    HapticFeedback.mediumImpact();
    _run(context.read<DeviceController>().wspamStart(
          mode: widget.mode.id,
          rate: _rate,
          dual: _dual,
        ));
  }

  void _stop() {
    HapticFeedback.mediumImpact();
    _run(context.read<DeviceController>().wspamStop());
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final running = ctrl.wspamRunning && ctrl.wspamMode == widget.mode.id;
    final otherRunning = ctrl.wspamRunning && ctrl.wspamMode != widget.mode.id;
    final customEmpty = _isCustom && ctrl.wspamCustom.isEmpty;

    return Scaffold(
      appBar: AppBar(
        iconTheme: IconThemeData(color: _accent),
        title: Text(
          '${widget.mode.label.toUpperCase()} SPAM',
          style: TextStyle(
            color: _accent,
            fontWeight: FontWeight.bold,
            letterSpacing: 2,
            fontSize: 15,
          ),
        ),
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          DeviceIssueBanner(controller: ctrl),

          // ── Stats ───────────────────────────────────────────
          Row(
            children: [
              Expanded(child: _stat('FRAMES', running ? '${ctrl.wspamTotal}' : '0')),
              const SizedBox(width: 8),
              Expanded(child: _stat('FPS', running ? '${ctrl.wspamFps}' : '0')),
              const SizedBox(width: 8),
              Expanded(
                  child: _stat('CH', running && ctrl.wspamChannel > 0
                      ? '${ctrl.wspamChannel}'
                      : '—')),
            ],
          ),
          const SizedBox(height: 14),

          // ── Current SSID being broadcast ────────────────────
          Container(
            width: double.infinity,
            padding: const EdgeInsets.all(14),
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(12),
              gradient: LinearGradient(
                colors: [_accent.withValues(alpha: 0.18), Colors.transparent],
                begin: Alignment.topLeft,
                end: Alignment.bottomRight,
              ),
              border: Border.all(color: _accent.withValues(alpha: 0.4)),
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Icon(Icons.wifi_tethering, color: _accent, size: 16),
                    const SizedBox(width: 6),
                    Text('BROADCASTING',
                        style: TextStyle(
                          color: _accent,
                          fontSize: 10,
                          letterSpacing: 1.5,
                          fontWeight: FontWeight.bold,
                        )),
                    const Spacer(),
                    if (running && ctrl.wspamChannel > 0)
                      Text('ch ${ctrl.wspamChannel}',
                          style: TextStyle(
                              color: _accent,
                              fontFamily: 'monospace',
                              fontSize: 12)),
                  ],
                ),
                const SizedBox(height: 8),
                Text(
                  running && ctrl.wspamCurrentSsid.isNotEmpty
                      ? ctrl.wspamCurrentSsid
                      : (running ? '…' : 'idle'),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 18,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 14),

          // ── Live SSID feed ──────────────────────────────────
          _label('LIVE FEED'),
          const SizedBox(height: 6),
          Container(
            height: 160,
            padding: const EdgeInsets.all(10),
            decoration: BoxDecoration(
              color: Colors.black.withValues(alpha: 0.5),
              borderRadius: BorderRadius.circular(10),
              border: Border.all(color: _accent.withValues(alpha: 0.25)),
            ),
            child: ctrl.wspamFeed.isEmpty
                ? Center(
                    child: Text(
                      running ? 'Starting…' : '(not running)',
                      style: const TextStyle(
                          color: Colors.white24, fontFamily: 'monospace'),
                    ),
                  )
                : ListView.builder(
                    itemCount: ctrl.wspamFeed.length,
                    itemBuilder: (_, i) {
                      final opacity =
                          (1.0 - i / ctrl.wspamFeed.length).clamp(0.25, 1.0);
                      return Padding(
                        padding: const EdgeInsets.symmetric(vertical: 1.5),
                        child: Row(
                          children: [
                            Icon(Icons.wifi, size: 12,
                                color: _accent.withValues(alpha: opacity)),
                            const SizedBox(width: 6),
                            Expanded(
                              child: Text(
                                ctrl.wspamFeed[i],
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                                style: TextStyle(
                                  color: Colors.white.withValues(alpha: opacity),
                                  fontFamily: 'monospace',
                                  fontSize: 12,
                                ),
                              ),
                            ),
                          ],
                        ),
                      );
                    },
                  ),
          ),
          const SizedBox(height: 16),

          // ── Custom SSID management ──────────────────────────
          if (_isCustom && !running) ...[
            _label('CUSTOM SSIDS (${ctrl.wspamCustom.length})'),
            const SizedBox(height: 6),
            Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _customCtrl,
                    style: const TextStyle(color: Colors.white),
                    decoration: InputDecoration(
                      isDense: true,
                      hintText: 'Add SSID…',
                      hintStyle: const TextStyle(color: Colors.white38),
                      border: const OutlineInputBorder(),
                      enabledBorder: OutlineInputBorder(
                        borderSide:
                            BorderSide(color: _accent.withValues(alpha: 0.4)),
                      ),
                    ),
                    inputFormatters: [LengthLimitingTextInputFormatter(32)],
                    onSubmitted: (_) => _addCustom(ctrl),
                  ),
                ),
                const SizedBox(width: 8),
                IconButton.filled(
                  style: IconButton.styleFrom(backgroundColor: _accent),
                  icon: const Icon(Icons.add, color: Colors.black),
                  onPressed: () => _addCustom(ctrl),
                ),
              ],
            ),
            const SizedBox(height: 8),
            if (ctrl.wspamCustom.isNotEmpty)
              Wrap(
                spacing: 6,
                runSpacing: 6,
                children: [
                  for (final s in ctrl.wspamCustom)
                    Chip(
                      label: Text(s,
                          style: const TextStyle(
                              color: Colors.white, fontSize: 11)),
                      backgroundColor: Colors.white.withValues(alpha: 0.06),
                      side: BorderSide(color: _accent.withValues(alpha: 0.3)),
                    ),
                  ActionChip(
                    avatar: const Icon(Icons.delete_outline,
                        size: 14, color: Color(0xFFFF6B6B)),
                    label: const Text('Clear',
                        style: TextStyle(color: Color(0xFFFF6B6B), fontSize: 11)),
                    backgroundColor: Colors.transparent,
                    side: const BorderSide(color: Color(0xFFFF6B6B)),
                    onPressed: () =>
                        _run(ctrl.wspamClearCustom()),
                  ),
                ],
              ),
            const SizedBox(height: 16),
          ],

          // ── Rate + band (when idle) ─────────────────────────
          if (!running) ...[
            _label('RATE'),
            const SizedBox(height: 6),
            Wrap(
              spacing: 8,
              children: [
                for (final r in const ['aggressive', 'normal', 'stealth'])
                  ChoiceChip(
                    selected: _rate == r,
                    label: Text(r,
                        style: TextStyle(
                            color: _rate == r ? Colors.black : Colors.white70,
                            fontSize: 11,
                            fontWeight: FontWeight.bold)),
                    selectedColor: _accent,
                    backgroundColor: Colors.white.withValues(alpha: 0.05),
                    side: BorderSide(color: _accent.withValues(alpha: 0.3)),
                    onSelected: (_) => setState(() => _rate = r),
                  ),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                _label('BAND'),
                const Spacer(),
                const Text('2.4G',
                    style: TextStyle(color: Colors.white54, fontSize: 12)),
                Switch(
                  value: _dual,
                  activeThumbColor: _accent,
                  onChanged: (v) => setState(() => _dual = v),
                ),
                const Text('DUAL',
                    style: TextStyle(color: Colors.white54, fontSize: 12)),
              ],
            ),
            const SizedBox(height: 16),
          ],

          // ── Start / Stop ────────────────────────────────────
          SizedBox(
            height: 52,
            child: ElevatedButton.icon(
              style: ElevatedButton.styleFrom(
                backgroundColor: running
                    ? const Color(0xFF8B0000)
                    : (customEmpty || otherRunning
                        ? Colors.grey.shade800
                        : _accent),
                foregroundColor: running ? Colors.white : Colors.black,
                shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(12)),
              ),
              onPressed: running
                  ? _stop
                  : (customEmpty || otherRunning ? null : _start),
              icon: Icon(running ? Icons.stop : Icons.wifi_tethering),
              label: Text(
                running
                    ? 'STOP SPAM'
                    : (customEmpty
                        ? 'ADD SSIDS FIRST'
                        : otherRunning
                            ? 'STOP OTHER MODE FIRST'
                            : 'START SPAM'),
                style: const TextStyle(
                    fontWeight: FontWeight.bold, letterSpacing: 1.5),
              ),
            ),
          ),
          const SizedBox(height: 10),
          Center(
            child: Text(
              'Beacons flood fake APs across 2.4${_dual ? '/5' : ''} GHz channels.',
              style: const TextStyle(color: Colors.white38, fontSize: 11),
            ),
          ),
        ],
      ),
    );
  }

  void _addCustom(DeviceController ctrl) {
    final s = _customCtrl.text.trim();
    if (s.isEmpty) return;
    _customCtrl.clear();
    _run(ctrl.wspamAddCustom(s));
  }

  Widget _stat(String label, String value) {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 12, horizontal: 6),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: _accent.withValues(alpha: 0.3)),
      ),
      child: Column(
        children: [
          FittedBox(
            fit: BoxFit.scaleDown,
            child: Text(value,
                style: TextStyle(
                    color: _accent,
                    fontSize: 20,
                    fontWeight: FontWeight.bold,
                    fontFamily: 'monospace')),
          ),
          const SizedBox(height: 2),
          Text(label,
              style: TextStyle(
                  color: Colors.white.withValues(alpha: 0.5),
                  fontSize: 9,
                  letterSpacing: 1,
                  fontFamily: 'monospace')),
        ],
      ),
    );
  }

  Widget _label(String t) => Text(t,
      style: TextStyle(
        color: _accent.withValues(alpha: 0.8),
        fontSize: 11,
        letterSpacing: 1.5,
        fontWeight: FontWeight.bold,
      ));
}
