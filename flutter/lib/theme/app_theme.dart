import 'package:flutter/material.dart';

/// Radio C5 design tokens.
///
/// One identity: near-black canvas + mint-cyan accent. Red/orange are reserved
/// for transmit / destructive / warning states; category hues survive only as
/// small icon tints, never as whole competing surfaces.
class AppColors {
  AppColors._();

  static const bg = Color(0xFF07110F); // canvas / scaffold
  static const surface = Color(0xFF111D1A); // card base
  static const surfaceHigh = Color(0xFF17251F); // elevated / pressed
  static const border = Color(0xFF2B403A); // hairline
  static const accent = Color(0xFF42D9C3); // primary mint-cyan identity
  static const accentDim = Color(0xFF2C8A7C); // muted accent

  static const text = Color(0xFFF1F5F3); // primary text
  static const textDim = Color(0xFFA6B8B2); // secondary text
  static const textFaint = Color(0xFF6E817B); // tertiary / meta

  static const danger = Color(0xFFFF5A52); // TX / destructive / errors
  static const warning = Color(0xFFFFB020); // caution

  /// Soft hairline used on glass edges (accent-neutral).
  static const glassBorder = Color(0x552B403A);
}

/// Shared corner radii so every surface shares one rounding language.
class AppRadius {
  AppRadius._();
  static const card = 16.0;
  static const tile = 14.0;
  static const chip = 20.0;
  static const hero = 20.0;
}

/// Builds the dark theme. Scaffold is transparent so the global
/// [AppBackground] (ambient layer) shows through every page.
ThemeData buildDarkTheme() {
  final scheme = ColorScheme.fromSeed(
    seedColor: AppColors.accent,
    brightness: Brightness.dark,
  ).copyWith(
    surface: AppColors.bg,
    primary: AppColors.accent,
    outline: AppColors.textFaint,
    outlineVariant: AppColors.border,
    error: AppColors.danger,
  );

  return ThemeData(
    useMaterial3: true,
    colorScheme: scheme,
    scaffoldBackgroundColor: Colors.transparent,
    canvasColor: AppColors.bg,
    splashColor: AppColors.accent.withValues(alpha: 0.08),
    highlightColor: AppColors.accent.withValues(alpha: 0.05),
    appBarTheme: const AppBarTheme(
      backgroundColor: Colors.transparent,
      surfaceTintColor: Colors.transparent,
      foregroundColor: AppColors.text,
      elevation: 0,
      centerTitle: false,
      titleTextStyle: TextStyle(
        color: AppColors.text,
        fontSize: 18,
        fontWeight: FontWeight.w700,
        letterSpacing: 0.3,
      ),
    ),
    sliderTheme: SliderThemeData(
      activeTrackColor: AppColors.accent,
      inactiveTrackColor: AppColors.border,
      thumbColor: AppColors.accent,
      overlayColor: AppColors.accent.withValues(alpha: 0.14),
    ),
    switchTheme: SwitchThemeData(
      thumbColor: WidgetStateProperty.resolveWith(
        (s) => s.contains(WidgetState.selected)
            ? AppColors.accent
            : AppColors.textFaint,
      ),
      trackColor: WidgetStateProperty.resolveWith(
        (s) => s.contains(WidgetState.selected)
            ? AppColors.accent.withValues(alpha: 0.35)
            : AppColors.border,
      ),
    ),
    dividerColor: AppColors.border,
    dialogTheme: const DialogThemeData(backgroundColor: AppColors.surface),
    snackBarTheme: const SnackBarThemeData(
      backgroundColor: AppColors.surfaceHigh,
      contentTextStyle: TextStyle(color: AppColors.text),
    ),
  );
}

/// Light theme is kept functional (app is dark-first).
ThemeData buildLightTheme() {
  return ThemeData(
    useMaterial3: true,
    colorScheme: ColorScheme.fromSeed(
      seedColor: AppColors.accent,
      brightness: Brightness.light,
    ).copyWith(
      outline: const Color(0xFF65736E),
      outlineVariant: AppColors.border,
    ),
  );
}
