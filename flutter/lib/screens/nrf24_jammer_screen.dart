import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';
import '../widgets/nrf_kit.dart';

const _kAccent = Color(0xFFFF3B30);

const _targets = <(String, String)>[
  ('full', 'Full band'),
  ('wifi', 'WiFi 2.4'),
  ('ble_adv', 'BLE adv'),
  ('ble_data', 'BLE data'),
  ('bt_classic', 'BT classic'),
  ('zigbee', 'Zigbee'),
  ('drone', 'Drone'),
  ('usb_dongle', 'USB dongle'),
];
const _strategies = <(String, String)>[
  ('cw', 'Carrier'),
  ('flood', 'Flood'),
  ('reactive', 'Reactive'),
];
const _rates = <(String, String)>[('250k', '250k'), ('1m', '1M'), ('2m', '2M')];
const _pas = <(String, String)>[
  ('min', 'Min'),
  ('low', 'Low'),
  ('high', 'High'),
  ('max', 'Max'),
];
const _durations = <(int, String)>[
  (0, '∞'),
  (10, '10s'),
  (30, '30s'),
  (60, '1m'),
  (300, '5m'),
];

enum _Mode { auto, manual, waterfall }

class Nrf24JammerScreen extends StatefulWidget {
  const Nrf24JammerScreen({super.key});

  @override
  State<Nrf24JammerScreen> createState() => _Nrf24JammerScreenState();
}

class _Nrf24JammerScreenState extends State<Nrf24JammerScreen> {
  DeviceController? _ctrl;

  _Mode _mode = _Mode.auto;
  String _target = 'full';
  String _strategy = 'cw';
  String _rate = '2m';
  String _pa = 'max';
  double _dwell = 400;
  int _burst = 1;
  int _duration = 0;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
  }

  @override
  void dispose() {
    // Safety: never leave the jammer running after leaving the screen.
    unawaited(_ctrl?.nrfJamStop().catchError((Object _) {}));
    super.dispose();
  }

  Future<void> _start() async {
    final ctrl = context.read<DeviceController>();
    try {
      switch (_mode) {
        case _Mode.auto:
          await ctrl.nrfJamAuto(_target, _duration);
        case _Mode.manual:
          await ctrl.nrfJamManual(
            target: _target,
            strategy: _strategy,
            rate: _rate,
            pa: _pa,
            dwellUs: _dwell.round(),
            burst: _burst,
            durationSec: _duration,
          );
        case _Mode.waterfall:
          await ctrl.nrfJamWaterfallStart(_duration);
      }
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context)
          .showSnackBar(SnackBar(content: Text('Jam failed: $e')));
    }
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final running = ctrl.nrfJamRunning;

    return Scaffold(
      appBar: AppBar(
        iconTheme: const IconThemeData(color: _kAccent),
        title: const Text(
          'nRF JAMMER',
          style: TextStyle(
              color: _kAccent,
              fontWeight: FontWeight.bold,
              letterSpacing: 2,
              fontSize: 16),
        ),
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(14, 12, 14, 24),
        children: [
          DeviceIssueBanner(controller: ctrl),

          if (running) _telemetry(ctrl) else _config(ctrl),
        ],
      ),
    );
  }

  // ── Live telemetry (running) ──────────────────────────────
  Widget _telemetry(DeviceController ctrl) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Container(
          padding: const EdgeInsets.all(20),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(18),
            gradient: const LinearGradient(
              colors: [Color(0xFF3A0B09), Color(0xFF140404)],
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
            ),
            border: Border.all(color: _kAccent.withValues(alpha: 0.5)),
            boxShadow: [
              BoxShadow(color: _kAccent.withValues(alpha: 0.18), blurRadius: 22),
            ],
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  const Icon(Icons.flash_on, color: _kAccent, size: 20),
                  const SizedBox(width: 8),
                  Text(
                    ctrl.nrfJamWaterfall
                        ? 'WATERFALL JAM'
                        : '${ctrl.nrfJamTarget.toUpperCase()} · '
                            '${ctrl.nrfJamStrategy.toUpperCase()}',
                    style: const TextStyle(
                        color: Colors.white,
                        fontSize: 14,
                        fontWeight: FontWeight.bold,
                        letterSpacing: 1),
                  ),
                  const Spacer(),
                  const NrfPulseDot(color: _kAccent, size: 9),
                ],
              ),
              const SizedBox(height: 18),
              Row(
                crossAxisAlignment: CrossAxisAlignment.end,
                children: [
                  Text(
                    '${ctrl.nrfJamFrames}',
                    style: const TextStyle(
                        color: _kAccent,
                        fontSize: 46,
                        height: 1,
                        fontWeight: FontWeight.w800,
                        fontFamily: 'monospace'),
                  ),
                  const SizedBox(width: 6),
                  const Padding(
                    padding: EdgeInsets.only(bottom: 6),
                    child: Text('frames',
                        style: TextStyle(color: Colors.white54, fontSize: 13)),
                  ),
                ],
              ),
              const SizedBox(height: 14),
              Row(
                children: [
                  _miniStat('ELAPSED', '${ctrl.nrfJamElapsed}s'),
                  _miniStat('CHANNEL', '${ctrl.nrfJamCurrentCh}'),
                  _miniStat(
                      ctrl.nrfJamWaterfall ? 'WF PEAK' : 'DUR',
                      ctrl.nrfJamWaterfall
                          ? (ctrl.nrfJamWfPeak < 0
                              ? '—'
                              : '${ctrl.nrfJamWfPeak}')
                          : (ctrl.nrfJamDurationSecs == 0
                              ? '∞'
                              : '${ctrl.nrfJamDurationSecs}s')),
                ],
              ),
            ],
          ),
        ),
        const SizedBox(height: 18),
        _bigButton('STOP JAMMER', Icons.stop, _kAccent, () {
          unawaited(ctrl.nrfJamStop().catchError((Object _) {}));
        }),
      ],
    );
  }

  Widget _miniStat(String label, String value) => Expanded(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(label,
                style: const TextStyle(
                    color: Colors.white38,
                    fontSize: 9,
                    letterSpacing: 1.2,
                    fontWeight: FontWeight.bold)),
            const SizedBox(height: 2),
            Text(value,
                style: const TextStyle(
                    color: Colors.white,
                    fontSize: 16,
                    fontWeight: FontWeight.bold,
                    fontFamily: 'monospace')),
          ],
        ),
      );

  // ── Config (idle) ─────────────────────────────────────────
  Widget _config(DeviceController ctrl) {
    final canStart = ctrl.nrfPresent;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        _warnBanner(),
        const SizedBox(height: 16),

        _label('MODE'),
        _chips<_Mode>(
          _Mode.values.map((m) => (m, _modeLabel(m))).toList(),
          _mode,
          (m) => setState(() => _mode = m),
        ),
        const SizedBox(height: 18),

        if (_mode != _Mode.waterfall) ...[
          _label('TARGET'),
          _chips<String>(
            _targets,
            _target,
            (v) => setState(() => _target = v),
          ),
          const SizedBox(height: 18),
        ],

        if (_mode == _Mode.manual) ...[
          _label('STRATEGY'),
          _chips<String>(
            _strategies,
            _strategy,
            (v) => setState(() => _strategy = v),
          ),
          const SizedBox(height: 18),
          _label('DATA RATE'),
          _chips<String>(_rates, _rate, (v) => setState(() => _rate = v)),
          const SizedBox(height: 18),
          _label('TX POWER'),
          _chips<String>(_pas, _pa, (v) => setState(() => _pa = v)),
          const SizedBox(height: 18),
          _label('DWELL ${_dwell.round()} µs'),
          Slider(
            value: _dwell,
            min: 50,
            max: 2000,
            divisions: 39,
            activeColor: _kAccent,
            label: '${_dwell.round()}µs',
            onChanged: (v) => setState(() => _dwell = v),
          ),
          const SizedBox(height: 4),
          _label('BURST $_burst'),
          Row(
            children: [
              _stepBtn(Icons.remove, () {
                if (_burst > 1) setState(() => _burst--);
              }),
              Expanded(
                child: Center(
                  child: Text('$_burst packet${_burst == 1 ? '' : 's'}/hop',
                      style: const TextStyle(
                          color: Colors.white,
                          fontSize: 14,
                          fontFamily: 'monospace')),
                ),
              ),
              _stepBtn(Icons.add, () {
                if (_burst < 20) setState(() => _burst++);
              }),
            ],
          ),
          const SizedBox(height: 18),
        ],

        _label('DURATION'),
        _chips<int>(_durations, _duration, (v) => setState(() => _duration = v)),
        const SizedBox(height: 24),

        _bigButton(
          canStart ? 'START JAMMER' : 'nRF24 NOT DETECTED',
          Icons.flash_on,
          _kAccent,
          canStart ? _start : null,
        ),
      ],
    );
  }

  String _modeLabel(_Mode m) => switch (m) {
        _Mode.auto => 'Auto',
        _Mode.manual => 'Manual',
        _Mode.waterfall => 'Waterfall',
      };

  Widget _warnBanner() => Container(
        padding: const EdgeInsets.all(12),
        decoration: BoxDecoration(
          color: _kAccent.withValues(alpha: 0.08),
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: _kAccent.withValues(alpha: 0.3)),
        ),
        child: const Row(
          children: [
            Icon(Icons.warning_amber, color: _kAccent, size: 18),
            SizedBox(width: 10),
            Expanded(
              child: Text(
                'Active RF jamming is disruptive and illegal in most regions '
                'outside a shielded lab. Use only on hardware you own.',
                style: TextStyle(
                    color: Colors.white60, fontSize: 11, height: 1.35),
              ),
            ),
          ],
        ),
      );

  Widget _label(String t) => Padding(
        padding: const EdgeInsets.only(left: 2, bottom: 8),
        child: Text(t,
            style: const TextStyle(
                color: Colors.white38,
                fontSize: 11,
                letterSpacing: 1.5,
                fontWeight: FontWeight.bold)),
      );

  Widget _chips<T>(
    List<(T, String)> options,
    T selected,
    void Function(T) onSelect,
  ) {
    return Wrap(
      spacing: 8,
      runSpacing: 8,
      children: [
        for (final (value, text) in options)
          GestureDetector(
            onTap: () => onSelect(value),
            child: AnimatedContainer(
              duration: const Duration(milliseconds: 120),
              padding:
                  const EdgeInsets.symmetric(horizontal: 14, vertical: 9),
              decoration: BoxDecoration(
                color: value == selected
                    ? _kAccent.withValues(alpha: 0.18)
                    : Colors.white.withValues(alpha: 0.03),
                borderRadius: BorderRadius.circular(10),
                border: Border.all(
                    color: value == selected
                        ? _kAccent
                        : Colors.white.withValues(alpha: 0.12)),
              ),
              child: Text(text,
                  style: TextStyle(
                      color: value == selected ? _kAccent : Colors.white70,
                      fontSize: 12,
                      fontWeight: FontWeight.bold)),
            ),
          ),
      ],
    );
  }

  Widget _stepBtn(IconData icon, VoidCallback onTap) => Material(
        color: Colors.white.withValues(alpha: 0.05),
        borderRadius: BorderRadius.circular(10),
        child: InkWell(
          onTap: onTap,
          borderRadius: BorderRadius.circular(10),
          child: Padding(
            padding: const EdgeInsets.all(10),
            child: Icon(icon, color: _kAccent, size: 20),
          ),
        ),
      );

  Widget _bigButton(
      String text, IconData icon, Color color, VoidCallback? onTap) {
    final enabled = onTap != null;
    return SizedBox(
      height: 54,
      child: Material(
        color: enabled ? color.withValues(alpha: 0.16) : Colors.white10,
        borderRadius: BorderRadius.circular(14),
        child: InkWell(
          onTap: onTap,
          borderRadius: BorderRadius.circular(14),
          child: Container(
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(14),
              border: Border.all(
                  color: enabled
                      ? color.withValues(alpha: 0.6)
                      : Colors.white24),
            ),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Icon(icon,
                    color: enabled ? color : Colors.white38, size: 20),
                const SizedBox(width: 10),
                Text(text,
                    style: TextStyle(
                        color: enabled ? color : Colors.white38,
                        fontSize: 14,
                        fontWeight: FontWeight.bold,
                        letterSpacing: 1.2)),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
