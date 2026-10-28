import 'package:flutter/material.dart';

import '../theme/app_theme.dart';

/// Legacy neutral border token (kept for existing callers). Points at the
/// shared design-system border so everything shares one edge colour.
const glassNeutralBorder = AppColors.glassBorder;

/// Translucent card decoration: a semi-transparent surface fill (so the
/// ambient [AppBackground] bleeds through), a hairline edge, and a soft lift
/// shadow. No BackdropFilter — cheap enough for long scrolling lists.
BoxDecoration glassSurfaceDecoration(
  BuildContext context, {
  required BorderRadius borderRadius,
  Color? borderColor,
}) {
  return BoxDecoration(
    gradient: LinearGradient(
      begin: Alignment.topLeft,
      end: Alignment.bottomRight,
      colors: [
        AppColors.surface.withValues(alpha: 0.58),
        AppColors.surface.withValues(alpha: 0.40),
      ],
    ),
    borderRadius: borderRadius,
    border: Border.all(
      color: borderColor ?? AppColors.border.withValues(alpha: 0.9),
    ),
    boxShadow: [
      BoxShadow(
        color: Colors.black.withValues(alpha: 0.28),
        blurRadius: 18,
        offset: const Offset(0, 8),
      ),
    ],
  );
}

/// Top-edge sheen that reads as light catching the glass surface. Painted as a
/// [Container.foregroundDecoration] so it needs no extra widget in the tree.
BoxDecoration _sheenDecoration(BorderRadius borderRadius, double strength) {
  return BoxDecoration(
    borderRadius: borderRadius,
    gradient: LinearGradient(
      begin: Alignment.topCenter,
      end: Alignment.bottomCenter,
      colors: [
        Colors.white.withValues(alpha: 0.10 * strength),
        Colors.white.withValues(alpha: 0.0),
      ],
      stops: const [0.0, 0.42],
    ),
  );
}

/// Simple tappable glass surface (legacy API, now with the shared look).
class GlassSurface extends StatelessWidget {
  const GlassSurface({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.all(16),
    this.radius = AppRadius.card,
    this.onTap,
    this.borderColor,
  });

  final Widget child;
  final EdgeInsetsGeometry padding;
  final double radius;
  final VoidCallback? onTap;
  final Color? borderColor;

  @override
  Widget build(BuildContext context) {
    return GlassCard(
      onTap: onTap,
      padding: padding,
      radius: radius,
      accent: borderColor,
      child: child,
    );
  }
}

/// The premium card: translucent fill, light sheen (lensing), lift shadow
/// (depth) and a touch response that shifts light + scale + depth. Built from
/// transform/opacity only, so it stays compositor-cheap even in long lists.
class GlassCard extends StatefulWidget {
  const GlassCard({
    super.key,
    required this.child,
    this.onTap,
    this.padding = const EdgeInsets.all(16),
    this.radius = AppRadius.card,
    this.accent,
    this.active = false,
    this.disabled = false,
  });

  final Widget child;
  final VoidCallback? onTap;
  final EdgeInsetsGeometry padding;
  final double radius;

  /// Optional hue. When [active] it drives the border + glow; otherwise it is
  /// ignored so cards stay neutral and do not all compete for attention.
  final Color? accent;
  final bool active;
  final bool disabled;

  @override
  State<GlassCard> createState() => _GlassCardState();
}

class _GlassCardState extends State<GlassCard> {
  bool _pressed = false;

  bool get _interactive => widget.onTap != null && !widget.disabled;

  void _setPressed(bool v) {
    if (!_interactive || _pressed == v) return;
    setState(() => _pressed = v);
  }

  @override
  Widget build(BuildContext context) {
    final br = BorderRadius.circular(widget.radius);
    final accent = widget.accent;
    final highlight = widget.active || _pressed;

    final borderColor = widget.active && accent != null
        ? accent.withValues(alpha: 0.75)
        : _pressed
            ? AppColors.accent.withValues(alpha: 0.45)
            : AppColors.border.withValues(alpha: 0.9);

    final shadows = <BoxShadow>[
      BoxShadow(
        color: Colors.black.withValues(alpha: _pressed ? 0.22 : 0.30),
        blurRadius: _pressed ? 10 : 20,
        offset: Offset(0, _pressed ? 3 : 9),
      ),
      if (widget.active && accent != null)
        BoxShadow(
          color: accent.withValues(alpha: 0.18),
          blurRadius: 22,
          offset: const Offset(0, 4),
        ),
    ];

    Widget card = AnimatedContainer(
      duration: const Duration(milliseconds: 130),
      curve: Curves.easeOut,
      padding: widget.padding,
      decoration: BoxDecoration(
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [
            AppColors.surface.withValues(alpha: _pressed ? 0.70 : 0.58),
            AppColors.surface.withValues(alpha: _pressed ? 0.52 : 0.40),
          ],
        ),
        borderRadius: br,
        border: Border.all(color: borderColor),
        boxShadow: shadows,
      ),
      foregroundDecoration: _sheenDecoration(br, highlight ? 1.5 : 1.0),
      child: widget.child,
    );

    if (widget.disabled) {
      card = Opacity(opacity: 0.4, child: card);
    }

    if (!_interactive) return card;

    return GestureDetector(
      onTap: widget.onTap,
      onTapDown: (_) => _setPressed(true),
      onTapUp: (_) => _setPressed(false),
      onTapCancel: () => _setPressed(false),
      child: AnimatedScale(
        scale: _pressed ? 0.975 : 1.0,
        duration: const Duration(milliseconds: 130),
        curve: Curves.easeOut,
        child: card,
      ),
    );
  }
}

/// Ambient background painted once behind every route (via MaterialApp.builder)
/// so the translucent cards have real colour to reveal. Static gradients only.
class AppBackground extends StatelessWidget {
  const AppBackground({super.key, required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    final isDark = Theme.of(context).brightness == Brightness.dark;
    if (!isDark) {
      return ColoredBox(color: const Color(0xFFF4F7F5), child: child);
    }
    return DecoratedBox(
      decoration: const BoxDecoration(
        color: AppColors.bg,
        gradient: RadialGradient(
          center: Alignment(-0.75, -0.95),
          radius: 1.35,
          colors: [Color(0x1842D9C3), Color(0x0042D9C3)],
          stops: [0.0, 0.72],
        ),
      ),
      child: DecoratedBox(
        decoration: const BoxDecoration(
          gradient: RadialGradient(
            center: Alignment(0.95, 0.85),
            radius: 1.25,
            colors: [Color(0x14123A33), Color(0x00000000)],
            stops: [0.0, 0.7],
          ),
        ),
        child: child,
      ),
    );
  }
}

/// Section label used between card groups (e.g. "RF TOOLS").
class SectionHeader extends StatelessWidget {
  const SectionHeader(this.label, {super.key, this.trailing});

  final String label;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(left: 4, bottom: 10, top: 2),
      child: Row(
        children: [
          Text(
            label,
            style: const TextStyle(
              color: AppColors.textDim,
              fontSize: 12,
              letterSpacing: 1.4,
              fontWeight: FontWeight.w700,
            ),
          ),
          const Spacer(),
          ?trailing,
        ],
      ),
    );
  }
}

/// Consistent BLE link indicator — a small pill with a status dot. Reused on
/// every page header so the device state reads the same everywhere (identity).
class LinkStatusChip extends StatelessWidget {
  const LinkStatusChip({
    super.key,
    required this.connected,
    this.label,
  });

  final bool connected;
  final String? label;

  @override
  Widget build(BuildContext context) {
    final color = connected ? AppColors.accent : AppColors.danger;
    final text = label ?? (connected ? 'LINKED' : 'LOST');
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 4),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.14),
        borderRadius: BorderRadius.circular(AppRadius.chip),
        border: Border.all(color: color.withValues(alpha: 0.4)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          StatusDot(color: color, pulsing: connected),
          const SizedBox(width: 6),
          Text(
            text,
            style: TextStyle(
              color: color,
              fontSize: 10,
              fontWeight: FontWeight.bold,
              letterSpacing: 1.3,
            ),
          ),
        ],
      ),
    );
  }
}

/// A small status dot. When [pulsing] it breathes gently — a subtle, functional
/// signal animation (not decorative neon). One short animation, GPU-cheap.
class StatusDot extends StatefulWidget {
  const StatusDot({super.key, required this.color, this.pulsing = false, this.size = 8});

  final Color color;
  final bool pulsing;
  final double size;

  @override
  State<StatusDot> createState() => _StatusDotState();
}

class _StatusDotState extends State<StatusDot>
    with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 1600),
  );

  @override
  void initState() {
    super.initState();
    if (widget.pulsing) _c.repeat(reverse: true);
  }

  @override
  void didUpdateWidget(StatusDot old) {
    super.didUpdateWidget(old);
    if (widget.pulsing && !_c.isAnimating) {
      _c.repeat(reverse: true);
    } else if (!widget.pulsing && _c.isAnimating) {
      _c.stop();
      _c.value = 0;
    }
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (!widget.pulsing) {
      return _dot(1.0);
    }
    return FadeTransition(
      opacity: Tween(begin: 0.45, end: 1.0).animate(
        CurvedAnimation(parent: _c, curve: Curves.easeInOut),
      ),
      child: _dot(1.0),
    );
  }

  Widget _dot(double glow) => Container(
        width: widget.size,
        height: widget.size,
        decoration: BoxDecoration(
          color: widget.color,
          shape: BoxShape.circle,
          boxShadow: [
            BoxShadow(
              color: widget.color.withValues(alpha: 0.6 * glow),
              blurRadius: 7,
            ),
          ],
        ),
      );
}
