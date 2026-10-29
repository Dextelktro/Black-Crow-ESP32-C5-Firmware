import 'package:flutter/foundation.dart';

/// App-wide flag that marks when a route transition animation is in flight.
///
/// While [isAnimating] is true, high-frequency state sources (notably
/// [DeviceController]'s ~20 Hz BLE telemetry) coalesce their notifications
/// instead of rebuilding listeners every tick. That keeps the UI thread free
/// for the push/pop animation — the only way a Flutter app can approach the
/// smoothness of an iOS transition, where Core Animation runs the animation on
/// a separate render server off the main thread. Here build/layout/paint and
/// the animation share one UI thread, so a burst of full-screen rebuilds landing
/// inside the ~220 ms transition window is exactly what makes open/close stutter.
///
/// Transitions can overlap (a pop still settling as a new push begins, a popup
/// opening over a page), so [begin]/[end] use a depth counter and idle listeners
/// fire only on the active → idle edge, i.e. once the UI is quiet again.
class UiTransitionGate {
  UiTransitionGate._();

  /// The single shared instance. A plain singleton (not an InheritedWidget) so
  /// both the root [NavigatorObserver] and the model — which live outside the
  /// widget tree that would carry a provider — can reach it directly.
  static final UiTransitionGate instance = UiTransitionGate._();

  int _depth = 0;
  final Set<VoidCallback> _idleListeners = <VoidCallback>{};

  /// True while at least one route transition is animating.
  bool get isAnimating => _depth > 0;

  /// Register a callback fired when the UI settles (the last in-flight
  /// transition finished), so coalesced work can flush exactly once.
  void addIdleListener(VoidCallback listener) => _idleListeners.add(listener);
  void removeIdleListener(VoidCallback listener) =>
      _idleListeners.remove(listener);

  /// Mark one transition as started.
  void begin() => _depth++;

  /// Mark one transition as finished. When the last one ends, notify idle
  /// listeners. Guarded so an extra [end] (e.g. a safety timer firing after the
  /// status listener already released) can never drive the depth negative.
  void end() {
    if (_depth == 0) return;
    _depth--;
    if (_depth == 0) _notifyIdle();
  }

  void _notifyIdle() {
    // Copy first: a listener may add or remove listeners while flushing.
    for (final listener in _idleListeners.toList()) {
      listener();
    }
  }

  /// Test-only: force the gate back to idle and flush, regardless of depth.
  @visibleForTesting
  void resetForTest() {
    _depth = 0;
    _notifyIdle();
  }
}
