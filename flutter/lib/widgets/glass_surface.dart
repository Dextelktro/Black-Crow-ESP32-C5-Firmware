import 'package:flutter/material.dart';

const glassNeutralBorder = Color(0x55616E68);

BoxDecoration glassSurfaceDecoration(
  BuildContext context, {
  required BorderRadius borderRadius,
  Color? borderColor,
}) {
  final isDark = Theme.of(context).brightness == Brightness.dark;
  final sheen = isDark ? Colors.white : Colors.black;
  return BoxDecoration(
    gradient: LinearGradient(
      begin: Alignment.topLeft,
      end: Alignment.bottomRight,
      colors: [
        sheen.withValues(alpha: isDark ? 0.055 : 0.035),
        sheen.withValues(alpha: isDark ? 0.015 : 0.008),
      ],
    ),
    borderRadius: borderRadius,
    border: Border.all(
      color: borderColor ??
          (isDark ? glassNeutralBorder : const Color(0x55717D78)),
    ),
  );
}

class GlassSurface extends StatelessWidget {
  const GlassSurface({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.all(16),
    this.radius = 16,
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
    final borderRadius = BorderRadius.circular(radius);
    return Material(
      color: Colors.transparent,
      shape: RoundedRectangleBorder(borderRadius: borderRadius),
      child: InkWell(
        onTap: onTap,
        borderRadius: borderRadius,
        child: Ink(
          padding: padding,
          decoration: glassSurfaceDecoration(
            context,
            borderRadius: borderRadius,
            borderColor: borderColor,
          ),
          child: child,
        ),
      ),
    );
  }
}
