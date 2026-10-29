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

const appRouteTransitionDuration = Duration(milliseconds: 220);
const appPopupMenuAnimationStyle = AnimationStyle(
  duration: Duration(milliseconds: 180),
  curve: Curves.easeOutCubic,
  reverseCurve: Curves.easeInCubic,
);

class AppPageRoute<T> extends MaterialPageRoute<T> {
  AppPageRoute({required super.builder, super.settings});

  @override
  Duration get transitionDuration => appRouteTransitionDuration;

  @override
  Duration get reverseTransitionDuration => appRouteTransitionDuration;
}

/// Page transition used app-wide: a Material **fade-through**.
///
/// The outgoing page's content fades out over roughly the first half of the
/// animation while the incoming page fades in — with a gentle zoom from
/// 0.96→1.0 — starting at ~35%. The two fades overlap slightly, but because
/// every page carries the same opaque [AppBackground] they only ever
/// cross-dissolve over that identical backdrop, never beside each other, so you
/// still never see two headers, two card stacks, or two pages side by side.
/// The overlap is deliberate: a zero-overlap fade-through briefly showed the
/// bare canvas with no content at all, which read as an almost-black flash
/// mid-transition. Letting the outgoing content linger until the incoming has
/// begun keeps the dimmest frame at roughly half content opacity instead.
///
/// Why this replaced the earlier horizontal iOS-style slide: a slide, by
/// construction, parks the outgoing page *beside* the incoming one for the
/// whole animation. Because every screen here shares the same left-aligned
/// header + dark-card layout, that read as doubled titles / stacked cards / a
/// glitch — exactly the overlap this fixes. Making each page opaque stopped the
/// two *bleeding through* each other but not *sitting next to* each other; only
/// removing the horizontal travel does that, and a fade-through has none.
///
/// Each page carries its own opaque [AppBackground] inside a [RepaintBoundary]:
/// the outgoing *content* fades out over that backdrop (so the hand-off reveals
/// the app canvas, never the route underneath or black), and the whole incoming
/// page composites in as one cached layer. Live telemetry is withheld while the
/// route animates (see UiTransitionGate), so the page rasterises once and the
/// fade just composites that layer — which keeps it smooth despite the opacity
/// pass. Forward and back run the same curves in reverse, so push and pop match.
class _FadeThroughPageTransitionsBuilder extends PageTransitionsBuilder {
  const _FadeThroughPageTransitionsBuilder();

  @override
  Widget buildTransitions<T>(
    PageRoute<T> route,
    BuildContext context,
    Animation<double> animation,
    Animation<double> secondaryAnimation,
    Widget child,
  ) {
    // Incoming (this route being pushed / popped): fade in over the last ~65%
    // so it only appears once the outgoing page has cleared, with a light zoom.
    final incoming = CurvedAnimation(
      parent: animation,
      curve: const Interval(0.35, 1.0, curve: Curves.easeOut),
      reverseCurve: const Interval(0.35, 1.0, curve: Curves.easeIn),
    );
    final zoom = Tween<double>(begin: 0.96, end: 1.0).animate(
      CurvedAnimation(
        parent: animation,
        curve: Curves.easeOutCubic,
        reverseCurve: Curves.easeInCubic,
      ),
    );

    // Outgoing (this route being covered / revealed): fade the *content* out
    // over the first ~50%, leaving its opaque backdrop so the hand-off shows the
    // app canvas rather than whatever route sits below. Ending at 0.5 (rather
    // than 0.35) overlaps the incoming fade-in that starts at 0.35, so the
    // content never fully disappears mid-transition — that empty window was the
    // "almost black" flash. The shared opaque backdrop means the overlap reads
    // as a cross-dissolve of co-located content, not two pages at once.
    final outgoing = Tween<double>(begin: 1.0, end: 0.0).animate(
      CurvedAnimation(
        parent: secondaryAnimation,
        curve: const Interval(0.0, 0.5, curve: Curves.easeIn),
        reverseCurve: const Interval(0.0, 0.5, curve: Curves.easeOut),
      ),
    );

    // RepaintBoundary wraps the (for the incoming route, static) opaque page so
    // the fade/zoom composite a cached raster instead of re-rastering per frame.
    return FadeTransition(
      opacity: incoming,
      child: ScaleTransition(
        scale: zoom,
        child: RepaintBoundary(
          child: AppBackground(
            child: FadeTransition(opacity: outgoing, child: child),
          ),
        ),
      ),
    );
  }
}

const _appFadeThroughTransition = _FadeThroughPageTransitionsBuilder();
const _appPageTransitions = PageTransitionsTheme(
  builders: <TargetPlatform, PageTransitionsBuilder>{
    TargetPlatform.android: _appFadeThroughTransition,
    TargetPlatform.iOS: _appFadeThroughTransition,
    TargetPlatform.linux: _appFadeThroughTransition,
    TargetPlatform.macOS: _appFadeThroughTransition,
    TargetPlatform.windows: _appFadeThroughTransition,
    TargetPlatform.fuchsia: _appFadeThroughTransition,
  },
);

/// Ambient background painted behind each route (wired through the page
/// transition above, so every page is self-contained and opaque). Static
/// gradients only — the translucent [GlassCard]s reveal this as their colour.
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

/// Builds the dark theme. Scaffolds are transparent so each route's own
/// [AppBackground] (wired through the page transition) shows through.
ThemeData buildDarkTheme() {
  final scheme =
      ColorScheme.fromSeed(
        seedColor: AppColors.accent,
        brightness: Brightness.dark,
      ).copyWith(
        surface: AppColors.bg,
        // Pin every M3 surface-container role to the palette. Left at the seed
        // defaults these are mint-tinted dark greens, which is what made menus,
        // dropdowns and popups flash a different (greenish) background.
        surfaceContainerLowest: AppColors.bg,
        surfaceContainerLow: AppColors.surface,
        surfaceContainer: AppColors.surface,
        surfaceContainerHigh: AppColors.surfaceHigh,
        surfaceContainerHighest: AppColors.surfaceHigh,
        surfaceBright: AppColors.surfaceHigh,
        surfaceDim: AppColors.bg,
        onSurface: AppColors.text,
        onSurfaceVariant: AppColors.textDim,
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
    pageTransitionsTheme: _appPageTransitions,
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
    bottomSheetTheme: const BottomSheetThemeData(
      backgroundColor: AppColors.surface,
      modalBackgroundColor: AppColors.surface,
      surfaceTintColor: Colors.transparent,
    ),
    // Keep every pop-out surface on the palette so opening a menu/dropdown does
    // not flash the seed-tinted M3 surface.
    popupMenuTheme: const PopupMenuThemeData(
      color: AppColors.surface,
      surfaceTintColor: Colors.transparent,
    ),
    dropdownMenuTheme: const DropdownMenuThemeData(
      menuStyle: MenuStyle(
        backgroundColor: WidgetStatePropertyAll(AppColors.surface),
        surfaceTintColor: WidgetStatePropertyAll(Colors.transparent),
      ),
    ),
    menuTheme: const MenuThemeData(
      style: MenuStyle(
        backgroundColor: WidgetStatePropertyAll(AppColors.surface),
        surfaceTintColor: WidgetStatePropertyAll(Colors.transparent),
      ),
    ),
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
    colorScheme:
        ColorScheme.fromSeed(
          seedColor: AppColors.accent,
          brightness: Brightness.light,
        ).copyWith(
          outline: const Color(0xFF65736E),
          outlineVariant: AppColors.border,
        ),
    pageTransitionsTheme: _appPageTransitions,
  );
}
