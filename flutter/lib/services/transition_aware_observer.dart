import 'dart:async';

import 'package:flutter/widgets.dart';

import 'ui_transition_gate.dart';

/// A [NavigatorObserver] that opens [UiTransitionGate] while route transitions
/// animate, so high-frequency models can hold their notifications until the
/// push/pop (or dialog/popup) has settled.
///
/// Each observed route is tracked by its [TransitionRoute.animation]: the gate
/// is held open from the first animating frame until the animation reaches
/// `completed` or `dismissed`. A per-transition safety timer force-releases the
/// gate if a status callback is ever missed (e.g. an interrupted back-gesture),
/// so the gate can never get stuck and freeze live updates.
class TransitionAwareObserver extends NavigatorObserver {
  TransitionAwareObserver({
    this.safetyTimeout = const Duration(milliseconds: 700),
  });

  /// Backstop: release the gate after this long even if the animation never
  /// reports completion. Comfortably longer than the app's 220 ms transition.
  final Duration safetyTimeout;

  final Map<Route<dynamic>, _RouteTracker> _trackers =
      <Route<dynamic>, _RouteTracker>{};

  void _track(Route<dynamic>? route) {
    if (route is! TransitionRoute<dynamic>) return;
    if (_trackers.containsKey(route)) return;
    final animation = route.animation;
    if (animation == null) return;
    final status = animation.status;
    // Already settled (e.g. a route configured with no transition): nothing to
    // wait for, so don't hold the gate.
    if (status == AnimationStatus.completed ||
        status == AnimationStatus.dismissed) {
      return;
    }

    UiTransitionGate.instance.begin();
    final tracker = _RouteTracker(animation);
    _trackers[route] = tracker;

    void release() {
      // Idempotent: whichever of the status listener / safety timer fires first
      // releases; the other becomes a no-op.
      if (_trackers.remove(route) == null) return;
      tracker.dispose();
      UiTransitionGate.instance.end();
    }

    tracker.statusListener = (AnimationStatus s) {
      if (s == AnimationStatus.completed || s == AnimationStatus.dismissed) {
        release();
      }
    };
    animation.addStatusListener(tracker.statusListener!);
    tracker.timer = Timer(safetyTimeout, release);
  }

  @override
  void didPush(Route<dynamic> route, Route<dynamic>? previousRoute) {
    _track(route);
    _track(previousRoute);
    super.didPush(route, previousRoute);
  }

  @override
  void didPop(Route<dynamic> route, Route<dynamic>? previousRoute) {
    _track(route);
    _track(previousRoute);
    super.didPop(route, previousRoute);
  }

  @override
  void didReplace({Route<dynamic>? newRoute, Route<dynamic>? oldRoute}) {
    _track(newRoute);
    super.didReplace(newRoute: newRoute, oldRoute: oldRoute);
  }

  @override
  void didStartUserGesture(
    Route<dynamic> route,
    Route<dynamic>? previousRoute,
  ) {
    _track(route);
    _track(previousRoute);
    super.didStartUserGesture(route, previousRoute);
  }
}

class _RouteTracker {
  _RouteTracker(this.animation);

  final Animation<double> animation;
  void Function(AnimationStatus)? statusListener;
  Timer? timer;

  void dispose() {
    timer?.cancel();
    timer = null;
    final listener = statusListener;
    if (listener != null) {
      animation.removeStatusListener(listener);
      statusListener = null;
    }
  }
}
