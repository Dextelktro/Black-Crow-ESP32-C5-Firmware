import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/subghz_kit.dart';
import 'subghz_common.dart';

/// Brute Force: iterate a small fixed-code protocol's code space and transmit
/// each code. OOK-only (CAME / Nice FLO / Linear), ≤ 20 bits, on the current
/// frequency. Dual-use — gated behind an explicit authorisation confirm.
class SubghzBruteScreen extends StatefulWidget {
  const SubghzBruteScreen({super.key});

  @override
  State<SubghzBruteScreen> createState() => _SubghzBruteScreenState();
}

class _BruteProto {
  const _BruteProto(this.id, this.label, this.bits, this.defaultBits);
  final String id; // firmware protocol name
  final String label;
  final List<int> bits; // selectable bit counts (<= 20)
  final int defaultBits;
}

const _protos = [
  _BruteProto('CAME', 'CAME', [12, 18], 12),
  _BruteProto('NiceFlo', 'Nice FLO', [12, 18, 20], 12),
  _BruteProto('Linear', 'Linear', [10], 10),
];

class _SubghzBruteScreenState extends State<SubghzBruteScreen> {
  DeviceController? _ctrl;
  Timer? _ticker;

  _BruteProto _proto = _protos[0];
  int _bits = 12;
  int _repeats = 2;

  // Local rate/ETA estimate from the idx telemetry.
  int _lastIdx = 0;
  DateTime? _lastSample;
  double _rate = 0; // codes/sec (EMA)

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      context.read<DeviceController>().subghzRefreshStatus();
    });
    _ticker = Timer.periodic(const Duration(seconds: 1), (_) {
      if (!mounted) return;
      final ctrl = _ctrl;
      if (ctrl == null || !ctrl.subghzBruteRunning) return;
      final now = DateTime.now();
      if (_lastSample != null) {
        final dt = now.difference(_lastSample!).inMilliseconds / 1000.0;
        if (dt > 0) {
          final inst = (ctrl.subghzBruteIdx - _lastIdx) / dt;
          _rate = _rate == 0 ? inst : _rate * 0.6 + inst * 0.4;
        }
      }
      _lastIdx = ctrl.subghzBruteIdx;
      _lastSample = now;
      setState(() {});
    });
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
  }

  @override
  void dispose() {
    _ticker?.cancel();
    final ctrl = _ctrl;
    if (ctrl != null) {
      closeSubghzSessionOnExit(
        ctrl.subghzCloseBruteSession,
        'Sub-GHz Brute Force',
      );
    }
    super.dispose();
  }

  int get _codeSpace => 1 << _bits;

  String _fmtDuration(double seconds) {
    if (!seconds.isFinite || seconds <= 0) return '—';
    final s = seconds.round();
    if (s < 60) return '${s}s';
    final m = s ~/ 60;
    if (m < 60) return '${m}m ${s % 60}s';
    final h = m ~/ 60;
    return '${h}h ${m % 60}m';
  }

  Future<void> _start(DeviceController ctrl) async {
    final ok = await _confirm(ctrl);
    if (!ok || !mounted) return;
    _rate = 0;
    _lastIdx = 0;
    _lastSample = DateTime.now();
    await ctrl.subghzBruteStart(_proto.id, _bits, repeats: _repeats);
  }

  Future<bool> _confirm(DeviceController ctrl) async {
    final r = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: const Text('Confirm transmission'),
        content: Text(
          'This will transmit $_codeSpace ${_proto.label} codes on '
          '${sgMhz(ctrl.subghzFreqHz)} MHz (OOK).\n\n'
          'Only do this on a receiver you own or are explicitly authorised to '
          'test. Transmitting to open or interfere with equipment you do not '
          'own may be illegal.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(c).pop(false),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () => Navigator.of(c).pop(true),
            child: const Text('I am authorised — start'),
          ),
        ],
      ),
    );
    return r ?? false;
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final running = ctrl.subghzBruteRunning;
    final total = ctrl.subghzBruteTotal;
    final idx = ctrl.subghzBruteIdx;
    final canResume = !running && idx > 0 && total > 0 && idx < total;

    return Scaffold(
      appBar: AppBar(title: const Text('Brute Force')),
      body: Column(
        children: [
          SubghzNoticeBanner(ctrl: ctrl),
          Expanded(
            child: ListView(
              padding: const EdgeInsets.all(16),
              children: [
                _AuthBanner(),
                const SizedBox(height: 14),
                SubghzConfigStrip(ctrl: ctrl),
                const SizedBox(height: 14),
                if (running || canResume)
                  _Progress(
                    idx: idx,
                    total: total,
                    proto: ctrl.subghzBruteProto.isEmpty
                        ? _proto.label
                        : ctrl.subghzBruteProto,
                    running: running,
                    rate: _rate,
                    etaText: _fmtDuration(
                      _rate > 0 ? (total - idx) / _rate : double.infinity,
                    ),
                  )
                else
                  _Config(
                    proto: _proto,
                    bits: _bits,
                    repeats: _repeats,
                    codeSpace: _codeSpace,
                    onProto: (p) => setState(() {
                      _proto = p;
                      if (!p.bits.contains(_bits)) _bits = p.defaultBits;
                    }),
                    onBits: (b) => setState(() => _bits = b),
                    onRepeats: (r) => setState(() => _repeats = r),
                  ),
                const SizedBox(height: 16),
                if (running)
                  SgButton(
                    label: 'STOP',
                    icon: Icons.stop,
                    danger: true,
                    onPressed: () => ctrl.subghzStop(),
                  )
                else ...[
                  SgButton(
                    label: 'START BRUTE FORCE',
                    icon: Icons.play_arrow,
                    onPressed: ctrl.subghzBusy ? null : () => _start(ctrl),
                  ),
                  if (canResume) ...[
                    const SizedBox(height: 10),
                    SgButton(
                      label: 'RESUME FROM #$idx',
                      icon: Icons.fast_forward,
                      filled: false,
                      onPressed: ctrl.subghzBusy
                          ? null
                          : () => ctrl.subghzBruteResume(),
                    ),
                  ],
                ],
                const SizedBox(height: 14),
                const Text(
                  'Brute force sends every code of a small fixed-code protocol in '
                  'order. It is slow (a full space can take minutes to hours) and '
                  'rolling-code remotes are immune. Transmit only on equipment you '
                  'own or are authorised to test; keep the device RF-isolated when '
                  'experimenting.',
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

class _AuthBanner extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: kSgDanger.withValues(alpha: 0.10),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: kSgDanger.withValues(alpha: 0.5)),
      ),
      child: Row(
        children: const [
          Icon(Icons.gpp_maybe, color: kSgDanger, size: 22),
          SizedBox(width: 12),
          Expanded(
            child: Text(
              'Authorised testing only. Transmitting codes to equipment you do '
              'not own may be illegal in your jurisdiction.',
              style: TextStyle(color: kSgDanger, fontSize: 12, height: 1.35),
            ),
          ),
        ],
      ),
    );
  }
}

class _Config extends StatelessWidget {
  const _Config({
    required this.proto,
    required this.bits,
    required this.repeats,
    required this.codeSpace,
    required this.onProto,
    required this.onBits,
    required this.onRepeats,
  });

  final _BruteProto proto;
  final int bits;
  final int repeats;
  final int codeSpace;
  final ValueChanged<_BruteProto> onProto;
  final ValueChanged<int> onBits;
  final ValueChanged<int> onRepeats;

  @override
  Widget build(BuildContext context) {
    return SgPanel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            'PROTOCOL',
            style: TextStyle(
              color: Colors.white38,
              fontSize: 10,
              letterSpacing: 1.2,
              fontWeight: FontWeight.bold,
            ),
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 8,
            children: [
              for (final p in _protos)
                _Pill(
                  label: p.label,
                  selected: p.id == proto.id,
                  onTap: () => onProto(p),
                ),
            ],
          ),
          const Divider(height: 24, color: Colors.white10),
          const Text(
            'BITS',
            style: TextStyle(
              color: Colors.white38,
              fontSize: 10,
              letterSpacing: 1.2,
              fontWeight: FontWeight.bold,
            ),
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 8,
            children: [
              for (final b in proto.bits)
                _Pill(label: '$b', selected: b == bits, onTap: () => onBits(b)),
            ],
          ),
          const SizedBox(height: 10),
          Row(
            children: [
              const Icon(Icons.tag, color: Colors.white38, size: 15),
              const SizedBox(width: 6),
              Text(
                'Code space: $codeSpace codes',
                style: const TextStyle(
                  color: Colors.white54,
                  fontSize: 12,
                  fontFamily: 'monospace',
                ),
              ),
            ],
          ),
          const Divider(height: 24, color: Colors.white10),
          Row(
            children: [
              const Text(
                'Repeats per code',
                style: TextStyle(color: Colors.white70, fontSize: 13),
              ),
              const Spacer(),
              _MiniStepper(
                value: repeats,
                min: 1,
                max: 10,
                onChanged: onRepeats,
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _Progress extends StatelessWidget {
  const _Progress({
    required this.idx,
    required this.total,
    required this.proto,
    required this.running,
    required this.rate,
    required this.etaText,
  });

  final int idx;
  final int total;
  final String proto;
  final bool running;
  final double rate;
  final String etaText;

  @override
  Widget build(BuildContext context) {
    final frac = total > 0 ? (idx / total).clamp(0.0, 1.0) : 0.0;
    return Container(
      padding: const EdgeInsets.all(18),
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
              Text(
                proto,
                style: const TextStyle(
                  color: Colors.white,
                  fontSize: 16,
                  fontWeight: FontWeight.bold,
                ),
              ),
              const Spacer(),
              Text(
                running ? '● TRANSMITTING' : 'PAUSED',
                style: TextStyle(
                  color: running ? kSgAmber : Colors.white38,
                  fontSize: 11,
                  fontWeight: FontWeight.bold,
                  letterSpacing: 1,
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          Row(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              Text(
                '${(frac * 100).toStringAsFixed(1)}%',
                style: const TextStyle(
                  color: kSgAmber,
                  fontSize: 36,
                  height: 1,
                  fontWeight: FontWeight.w800,
                  fontFamily: 'monospace',
                ),
              ),
              const Spacer(),
              Text(
                '$idx / $total',
                style: const TextStyle(
                  color: Colors.white54,
                  fontSize: 13,
                  fontFamily: 'monospace',
                ),
              ),
            ],
          ),
          const SizedBox(height: 10),
          ClipRRect(
            borderRadius: BorderRadius.circular(5),
            child: LinearProgressIndicator(
              value: frac,
              minHeight: 8,
              backgroundColor: Colors.white.withValues(alpha: 0.08),
              valueColor: const AlwaysStoppedAnimation(kSgAmber),
            ),
          ),
          const SizedBox(height: 14),
          Row(
            children: [
              Expanded(
                child: SgStat(
                  label: 'RATE',
                  value: rate > 0 ? '${rate.round()} code/s' : '—',
                ),
              ),
              Expanded(
                child: SgStat(label: 'ETA', value: running ? etaText : '—'),
              ),
              Expanded(
                child: SgStat(
                  label: 'CURRENT',
                  value: '0x${idx.toRadixString(16).toUpperCase()}',
                  color: kSgAmber,
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _Pill extends StatelessWidget {
  const _Pill({
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
            color: selected
                ? kSgAmber.withValues(alpha: 0.18)
                : Colors.transparent,
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

class _MiniStepper extends StatelessWidget {
  const _MiniStepper({
    required this.value,
    required this.min,
    required this.max,
    required this.onChanged,
  });
  final int value;
  final int min;
  final int max;
  final ValueChanged<int> onChanged;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        _btn(Icons.remove, value > min ? () => onChanged(value - 1) : null),
        Container(
          width: 36,
          alignment: Alignment.center,
          child: Text(
            '$value',
            style: const TextStyle(
              color: Colors.white,
              fontSize: 16,
              fontWeight: FontWeight.bold,
              fontFamily: 'monospace',
            ),
          ),
        ),
        _btn(Icons.add, value < max ? () => onChanged(value + 1) : null),
      ],
    );
  }

  Widget _btn(IconData icon, VoidCallback? onTap) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(8),
        child: Container(
          width: 32,
          height: 32,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(8),
            border: Border.all(
              color: kSgAmber.withValues(alpha: onTap == null ? 0.2 : 0.5),
            ),
          ),
          child: Icon(
            icon,
            size: 17,
            color: onTap == null ? Colors.white24 : kSgAmber,
          ),
        ),
      ),
    );
  }
}
