import 'package:flutter/material.dart';

/// Shared visual identity for the nRF24 toolkit. One hot colormap, one live
/// "pulse" signature, one chip and one glow-panel style used across every nRF
/// screen so the whole toolkit reads as a single, distinctive instrument.

/// Maps a 0..1 energy value to the nRF "hot" colormap:
/// deep teal → teal → lime → amber → white-hot.
Color nrfHot(double t) {
  t = t.isNaN ? 0.0 : t.clamp(0.0, 1.0);
  const stops = <(double, Color)>[
    (0.00, Color(0xFF041A16)),
    (0.18, Color(0xFF0B5A50)),
    (0.42, Color(0xFF00E0B8)),
    (0.68, Color(0xFF8BE04B)),
    (0.85, Color(0xFFFFC53D)),
    (1.00, Color(0xFFFFF4E0)),
  ];
  for (var i = 0; i < stops.length - 1; i++) {
    final (a, ca) = stops[i];
    final (b, cb) = stops[i + 1];
    if (t <= b) return Color.lerp(ca, cb, (t - a) / (b - a))!;
  }
  return stops.last.$2;
}

/// Translucent monospace status chip — the toolkit's label unit.
Widget nrfChip(String text, Color c, {bool mono = true}) => Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
      decoration: BoxDecoration(
        color: c.withValues(alpha: 0.12),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: c.withValues(alpha: 0.4)),
      ),
      child: Text(text,
          style: TextStyle(
              color: c,
              fontSize: 11,
              fontWeight: FontWeight.bold,
              fontFamily: mono ? 'monospace' : null)),
    );

/// Glowing panel decoration used by hero cards.
BoxDecoration nrfPanel(Color c, {double glow = 0.10, double radius = 16}) =>
    BoxDecoration(
      color: Colors.white.withValues(alpha: 0.02),
      borderRadius: BorderRadius.circular(radius),
      border: Border.all(color: c.withValues(alpha: 0.3)),
      boxShadow: [BoxShadow(color: c.withValues(alpha: glow), blurRadius: 16)],
    );

/// A soft pulsing dot — the nRF "live" signature, used in place of the generic
/// Material spinner so the toolkit has its own heartbeat.
class NrfPulseDot extends StatefulWidget {
  const NrfPulseDot({super.key, required this.color, this.size = 10});
  final Color color;
  final double size;

  @override
  State<NrfPulseDot> createState() => _NrfPulseDotState();
}

class _NrfPulseDotState extends State<NrfPulseDot>
    with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 1100),
  )..repeat();

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _c,
      builder: (context, _) {
        final t = _c.value;
        final ring = 1.0 - t;
        return SizedBox(
          width: widget.size * 2.4,
          height: widget.size * 2.4,
          child: Center(
            child: Stack(
              alignment: Alignment.center,
              children: [
                Container(
                  width: widget.size + widget.size * 1.5 * t,
                  height: widget.size + widget.size * 1.5 * t,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: widget.color.withValues(alpha: 0.35 * ring),
                  ),
                ),
                Container(
                  width: widget.size,
                  height: widget.size,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: widget.color,
                    boxShadow: [
                      BoxShadow(
                          color: widget.color.withValues(alpha: 0.6),
                          blurRadius: 6),
                    ],
                  ),
                ),
              ],
            ),
          ),
        );
      },
    );
  }
}
