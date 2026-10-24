import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';

/// Shared visual kit for the Sub-GHz (CC1101) toolkit. Its identity is a warm
/// "analog tuning dial": amber/gold on a deep ember-black, a horizontal
/// frequency tuner with lit band regions, and a pulse-train scope — distinct
/// from the teal/cyan/pink of the other RF tools so the feature reads as its
/// own instrument.

const kSgAmber = Color(0xFFFFB020);
const kSgOrange = Color(0xFFFF7A1A);
const kSgInk = Color(0xFF140B02);
const kSgInk2 = Color(0xFF241506);
const kSgDanger = Color(0xFFFF5050);

/// Tuner frequency range (Hz) — spans the firmware's 300–348 / 387–464 /
/// 779–928 MHz band groups with a little headroom.
const int kSgMinHz = 280000000;
const int kSgMaxHz = 940000000;

/// The three valid RX/TX band groups, as (loHz, hiHz).
const List<List<int>> kSgBands = [
  [300000000, 348000000],
  [387000000, 464000000],
  [779000000, 928000000],
];

/// Common quick-tune spot frequencies (Hz) with a short label.
const List<(String, int)> kSgSpots = [
  ('315', 315000000),
  ('390', 390000000),
  ('433.92', 433920000),
  ('868.35', 868350000),
  ('915', 915000000),
];

bool sgFreqInBand(int hz) {
  for (final b in kSgBands) {
    if (hz >= b[0] && hz <= b[1]) return true;
  }
  return false;
}

String sgMhz(int hz, {int decimals = 2}) =>
    (hz / 1e6).toStringAsFixed(decimals);

/// Colour ramp for an RSSI reading (dBm): weak grey → amber → hot orange/red.
Color sgRssiColor(int dbm) {
  if (dbm <= -110) return Colors.white24;
  if (dbm >= -45) return kSgDanger;
  final t = ((dbm + 110) / 65).clamp(0.0, 1.0);
  return Color.lerp(const Color(0xFF6B7280), kSgAmber, t)!;
}

double sgRssiFraction(int dbm) => ((dbm + 110) / 80.0).clamp(0.0, 1.0);

/// Push a route that re-wraps the shared [DeviceController] provider so the
/// pushed Sub-GHz screen keeps observing the same device (project
/// flutter-provider route pattern).
void pushSubghz(BuildContext context, DeviceController ctrl, Widget screen) {
  Navigator.of(context).push(
    MaterialPageRoute(
      builder: (_) => ChangeNotifierProvider.value(value: ctrl, child: screen),
    ),
  );
}

/// Small uppercase section caption.
class SgSectionLabel extends StatelessWidget {
  const SgSectionLabel(this.text, {super.key, this.trailing});
  final String text;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(left: 2, bottom: 8, top: 2),
      child: Row(
        children: [
          Text(
            text,
            style: const TextStyle(
              color: Colors.white38,
              fontSize: 11,
              letterSpacing: 2,
              fontWeight: FontWeight.bold,
            ),
          ),
          const Spacer(),
          ?trailing,
        ],
      ),
    );
  }
}

/// Bordered amber panel used to group controls.
class SgPanel extends StatelessWidget {
  const SgPanel({
    super.key,
    required this.child,
    this.accent = kSgAmber,
    this.padding = const EdgeInsets.all(16),
  });
  final Widget child;
  final Color accent;
  final EdgeInsets padding;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: padding,
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.03),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: accent.withValues(alpha: 0.28)),
      ),
      child: child,
    );
  }
}

/// A label + value stat, monospace value.
class SgStat extends StatelessWidget {
  const SgStat({
    super.key,
    required this.label,
    required this.value,
    this.color = Colors.white,
  });
  final String label;
  final String value;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        Text(
          label,
          style: const TextStyle(
            color: Colors.white38,
            fontSize: 9,
            letterSpacing: 1.1,
            fontWeight: FontWeight.bold,
          ),
        ),
        const SizedBox(height: 3),
        Text(
          value,
          style: TextStyle(
            color: color,
            fontSize: 16,
            fontWeight: FontWeight.w800,
            fontFamily: 'monospace',
          ),
        ),
      ],
    );
  }
}

/// A full-width gradient action button with an optional busy spinner.
class SgButton extends StatelessWidget {
  const SgButton({
    super.key,
    required this.label,
    required this.icon,
    required this.onPressed,
    this.busy = false,
    this.danger = false,
    this.filled = true,
  });

  final String label;
  final IconData icon;
  final VoidCallback? onPressed;
  final bool busy;
  final bool danger;
  final bool filled;

  @override
  Widget build(BuildContext context) {
    final enabled = onPressed != null && !busy;
    final base = danger ? kSgDanger : kSgAmber;
    return Opacity(
      opacity: enabled ? 1 : 0.5,
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          onTap: enabled ? onPressed : null,
          borderRadius: BorderRadius.circular(14),
          child: Container(
            height: 52,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(14),
              gradient: filled
                  ? LinearGradient(
                      colors: danger
                          ? [kSgDanger, const Color(0xFFB01818)]
                          : [kSgAmber, kSgOrange],
                    )
                  : null,
              color: filled ? null : base.withValues(alpha: 0.10),
              border: filled
                  ? null
                  : Border.all(color: base.withValues(alpha: 0.5)),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                if (busy)
                  SizedBox(
                    width: 18,
                    height: 18,
                    child: CircularProgressIndicator(
                      strokeWidth: 2.4,
                      valueColor: AlwaysStoppedAnimation(
                        filled ? Colors.black : base,
                      ),
                    ),
                  )
                else
                  Icon(icon, size: 19, color: filled ? Colors.black : base),
                const SizedBox(width: 9),
                Text(
                  label,
                  style: TextStyle(
                    color: filled ? Colors.black : base,
                    fontSize: 14,
                    fontWeight: FontWeight.w800,
                    letterSpacing: 0.6,
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

/// Big monospace frequency readout in MHz.
class SgFreqReadout extends StatelessWidget {
  const SgFreqReadout({super.key, required this.hz, this.inBand = true});
  final int hz;
  final bool inBand;

  @override
  Widget build(BuildContext context) {
    final color = inBand ? kSgAmber : kSgDanger;
    return Row(
      crossAxisAlignment: CrossAxisAlignment.end,
      mainAxisSize: MainAxisSize.min,
      children: [
        Text(
          sgMhz(hz, decimals: 3),
          style: TextStyle(
            color: color,
            fontSize: 40,
            height: 1,
            fontWeight: FontWeight.w800,
            fontFamily: 'monospace',
            shadows: [BoxShadow(color: color.withValues(alpha: 0.5), blurRadius: 16)],
          ),
        ),
        const Padding(
          padding: EdgeInsets.only(bottom: 4, left: 4),
          child: Text(
            'MHz',
            style: TextStyle(
              color: Colors.white54,
              fontSize: 13,
              fontWeight: FontWeight.bold,
            ),
          ),
        ),
      ],
    );
  }
}

/// Horizontal analog tuning dial. Drag to sweep across [kSgMinHz]..[kSgMaxHz];
/// the valid band groups glow amber, a needle marks the current frequency.
/// [onChanged] fires live during a drag (update the readout cheaply); [onCommit]
/// fires once the drag ends (send to the device then).
class FrequencyTuner extends StatelessWidget {
  const FrequencyTuner({
    super.key,
    required this.hz,
    required this.onChanged,
    required this.onCommit,
    this.height = 92,
  });

  final int hz;
  final ValueChanged<int> onChanged;
  final ValueChanged<int> onCommit;
  final double height;

  int _hzAt(double dx, double width) {
    final frac = (dx / width).clamp(0.0, 1.0);
    final v = (kSgMinHz + frac * (kSgMaxHz - kSgMinHz)).round();
    // Snap to 10 kHz so dragging yields clean values.
    return (v ~/ 10000) * 10000;
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, c) {
        final w = c.maxWidth;
        return GestureDetector(
          behavior: HitTestBehavior.opaque,
          onTapDown: (d) => onCommit(_hzAt(d.localPosition.dx, w)),
          onHorizontalDragUpdate: (d) =>
              onChanged(_hzAt(d.localPosition.dx, w)),
          onHorizontalDragEnd: (_) => onCommit(hz),
          child: CustomPaint(
            size: Size(w, height),
            painter: _TunerPainter(hz),
          ),
        );
      },
    );
  }
}

class _TunerPainter extends CustomPainter {
  _TunerPainter(this.hz);
  final int hz;

  double _x(int f, double w) =>
      ((f - kSgMinHz) / (kSgMaxHz - kSgMinHz)).clamp(0.0, 1.0) * w;

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height;
    canvas.drawRRect(
      RRect.fromRectAndRadius(Offset.zero & size, const Radius.circular(12)),
      Paint()..color = kSgInk,
    );

    // Lit band regions.
    for (final b in kSgBands) {
      final x0 = _x(b[0], w), x1 = _x(b[1], w);
      canvas.drawRect(
        Rect.fromLTRB(x0, 8, x1, h - 8),
        Paint()
          ..shader = LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: [
              kSgAmber.withValues(alpha: 0.22),
              kSgOrange.withValues(alpha: 0.06),
            ],
          ).createShader(Rect.fromLTRB(x0, 8, x1 <= x0 ? x0 + 1 : x1, h - 8)),
      );
      canvas.drawRect(
        Rect.fromLTRB(x0, 8, x1, h - 8),
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1
          ..color = kSgAmber.withValues(alpha: 0.35),
      );
    }

    // Ticks every 50 MHz, labelled every 100 MHz.
    final tickPaint = Paint()
      ..color = Colors.white.withValues(alpha: 0.18)
      ..strokeWidth = 1;
    for (var f = 300000000; f <= kSgMaxHz; f += 50000000) {
      final x = _x(f, w);
      final major = (f % 100000000) == 0;
      canvas.drawLine(
        Offset(x, h - 8),
        Offset(x, h - (major ? 20 : 14)),
        tickPaint,
      );
      if (major) {
        final tp = TextPainter(
          text: TextSpan(
            text: '${f ~/ 1000000}',
            style: const TextStyle(
              color: Colors.white38,
              fontSize: 8,
              fontFamily: 'monospace',
            ),
          ),
          textDirection: TextDirection.ltr,
        )..layout();
        tp.paint(canvas, Offset(x - tp.width / 2, 4));
      }
    }

    // Needle at the current frequency.
    final nx = _x(hz, w);
    final inBand = sgFreqInBand(hz);
    final nc = inBand ? kSgAmber : kSgDanger;
    canvas.drawLine(
      Offset(nx, 2),
      Offset(nx, h - 2),
      Paint()
        ..color = nc
        ..strokeWidth = 2
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 2),
    );
    canvas.drawLine(
      Offset(nx, 2),
      Offset(nx, h - 2),
      Paint()
        ..color = nc
        ..strokeWidth = 1.2,
    );
    canvas.drawCircle(Offset(nx, h - 2), 4, Paint()..color = nc);
  }

  @override
  bool shouldRepaint(_TunerPainter old) => old.hz != hz;
}

/// A pulse-train scope for a raw OOK burst: positive = carrier/high (amber),
/// negative = off/low (dark). Renders the full burst scaled to width.
class PulseTrain extends StatelessWidget {
  const PulseTrain({super.key, required this.durations, this.height = 64});
  final List<int> durations;
  final double height;

  @override
  Widget build(BuildContext context) {
    return ClipRRect(
      borderRadius: BorderRadius.circular(10),
      child: Container(
        height: height,
        color: kSgInk,
        child: CustomPaint(
          size: Size.infinite,
          painter: _PulsePainter(durations),
        ),
      ),
    );
  }
}

class _PulsePainter extends CustomPainter {
  _PulsePainter(this.durations);
  final List<int> durations;

  @override
  void paint(Canvas canvas, Size size) {
    final n = durations.length;
    if (n == 0) return;
    final w = size.width, h = size.height;
    final hi = h * 0.28, lo = h * 0.72;

    var total = 0;
    for (final d in durations) {
      total += d.abs();
    }
    if (total <= 0) return;

    final line = Path();
    var x = 0.0;
    var level = durations.first >= 0;
    line.moveTo(0, level ? hi : lo);
    for (var i = 0; i < n; i++) {
      final seg = durations[i].abs() / total * w;
      final lvl = durations[i] >= 0;
      if (lvl != level) {
        line.lineTo(x, lvl ? hi : lo); // vertical edge
        level = lvl;
      }
      x += seg;
      line.lineTo(x, level ? hi : lo);
    }

    // Fill the carrier (high) areas faintly.
    final fill = Paint()..color = kSgAmber.withValues(alpha: 0.12);
    x = 0.0;
    for (var i = 0; i < n; i++) {
      final seg = durations[i].abs() / total * w;
      if (durations[i] >= 0) {
        canvas.drawRect(Rect.fromLTRB(x, hi, x + seg, lo), fill);
      }
      x += seg;
    }

    canvas.drawPath(
      line,
      Paint()
        ..color = kSgAmber
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.6
        ..strokeJoin = StrokeJoin.round,
    );
  }

  @override
  bool shouldRepaint(_PulsePainter old) =>
      !identical(old.durations, durations) ||
      old.durations.length != durations.length;
}
