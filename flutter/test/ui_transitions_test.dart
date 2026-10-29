import 'package:deauther/services/ui_transition_gate.dart';
import 'package:deauther/theme/app_theme.dart';
import 'package:deauther/widgets/app_dropdown_field.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

class _PopupRouteObserver extends NavigatorObserver {
  PopupRoute<dynamic>? route;

  @override
  void didPush(Route<dynamic> route, Route<dynamic>? previousRoute) {
    if (route is PopupRoute<dynamic>) this.route = route;
    super.didPush(route, previousRoute);
  }
}

void main() {
  group('UiTransitionGate', () {
    final gate = UiTransitionGate.instance;
    tearDown(gate.resetForTest);

    test('fires idle listeners once on the active -> idle edge only', () {
      var idle = 0;
      void onIdle() => idle++;
      gate.addIdleListener(onIdle);
      addTearDown(() => gate.removeIdleListener(onIdle));

      expect(gate.isAnimating, isFalse);
      gate.begin();
      gate.begin(); // overlapping transitions
      expect(gate.isAnimating, isTrue);
      gate.end();
      expect(gate.isAnimating, isTrue); // still one in flight
      expect(idle, 0);
      gate.end();
      expect(gate.isAnimating, isFalse);
      expect(idle, 1); // exactly once, on the final settle
    });

    test('an extra end() while idle is a harmless no-op', () {
      var idle = 0;
      void onIdle() => idle++;
      gate.addIdleListener(onIdle);
      addTearDown(() => gate.removeIdleListener(onIdle));

      gate.end(); // depth already 0
      expect(gate.isAnimating, isFalse);
      expect(idle, 0);
    });

    test('a telemetry-style source coalesces to one flush per transition', () {
      // Mirrors DeviceController._notifyListeners / _flushPendingNotify.
      var rebuilds = 0;
      var pending = false;
      void notify() {
        if (gate.isAnimating) {
          pending = true;
          return;
        }
        rebuilds++;
      }

      void flush() {
        if (pending) {
          pending = false;
          rebuilds++;
        }
      }

      gate.addIdleListener(flush);
      addTearDown(() => gate.removeIdleListener(flush));

      notify(); // idle -> immediate
      expect(rebuilds, 1);

      gate.begin(); // transition starts
      notify();
      notify();
      notify(); // three ~20 Hz telemetry ticks during the 220 ms animation
      expect(rebuilds, 1); // all withheld — the UI thread stays free
      gate.end(); // settle
      expect(rebuilds, 2); // flushed exactly once
    });
  });

  test('app page routes use a 220 ms transition in both directions', () {
    final route = AppPageRoute<void>(builder: (_) => const SizedBox.shrink());

    expect(route.transitionDuration, const Duration(milliseconds: 220));
    expect(route.reverseTransitionDuration, const Duration(milliseconds: 220));
  });

  testWidgets('popup menu opens and closes within 200 ms', (tester) async {
    final observer = _PopupRouteObserver();
    await tester.pumpWidget(
      MaterialApp(
        theme: buildDarkTheme(),
        navigatorObservers: [observer],
        home: Scaffold(
          appBar: AppBar(
            actions: [
              PopupMenuButton<String>(
                popUpAnimationStyle: appPopupMenuAnimationStyle,
                itemBuilder: (_) => const [
                  PopupMenuItem(value: 'settings', child: Text('Settings')),
                ],
              ),
            ],
          ),
        ),
      ),
    );

    await tester.tap(find.byType(PopupMenuButton<String>));
    final openStartedAt = tester.binding.clock.now();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 180));
    final openDuration = tester.binding.clock.now().difference(openStartedAt);

    expect(openDuration, lessThanOrEqualTo(const Duration(milliseconds: 200)));
    expect(
      observer.route?.transitionDuration,
      const Duration(milliseconds: 180),
    );
    expect(observer.route?.animation?.value, 1.0);
    expect(find.text('Settings'), findsOneWidget);

    await tester.tap(find.text('Settings'));
    final closeStartedAt = tester.binding.clock.now();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 180));
    final closeDuration = tester.binding.clock.now().difference(closeStartedAt);

    expect(closeDuration, lessThanOrEqualTo(const Duration(milliseconds: 200)));
    expect(observer.route?.animation?.value, 0.0);
    await tester.pump(const Duration(milliseconds: 1));
    expect(find.text('Settings'), findsNothing);
  });

  testWidgets('dropdown fields use the shared popup timing', (tester) async {
    String? selected;
    await tester.pumpWidget(
      MaterialApp(
        theme: buildDarkTheme(),
        home: Scaffold(
          body: Center(
            child: SizedBox(
              width: 420,
              child: AppDropdownField<String>(
                label: 'Target chip',
                value: 'auto',
                valueLabel: 'Detect automatically',
                itemBuilder: (_) => const [
                  PopupMenuItem(
                    value: 'auto',
                    child: Text('Detect automatically'),
                  ),
                  PopupMenuItem(value: 'esp32c5', child: Text('esp32c5')),
                ],
                onSelected: (value) => selected = value,
              ),
            ),
          ),
        ),
      ),
    );

    final menu = tester.widget<PopupMenuButton<String>>(
      find.byType(PopupMenuButton<String>),
    );
    expect(
      menu.popUpAnimationStyle?.duration,
      const Duration(milliseconds: 180),
    );

    await tester.tap(find.byType(PopupMenuButton<String>));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 180));
    await tester.tap(find.text('esp32c5'));
    expect(selected, 'esp32c5');
  });

  testWidgets('feature pages fade through instead of sliding (no overlap)', (
    tester,
  ) async {
    final navKey = GlobalKey<NavigatorState>();
    await tester.pumpWidget(
      MaterialApp(
        theme: buildDarkTheme(),
        navigatorKey: navKey,
        home: const Scaffold(body: Center(child: Text('HOME'))),
      ),
    );

    navKey.currentState!.push(
      AppPageRoute<void>(
        builder: (_) => const Scaffold(body: Center(child: Text('FEATURE'))),
      ),
    );
    await tester.pump(); // first transition frame
    await tester.pump(const Duration(milliseconds: 70)); // ~32% through 220 ms

    // The horizontal slide was the root cause of two pages sitting side by
    // side; the fade-through uses none.
    expect(find.byType(SlideTransition), findsNothing);
    expect(find.byType(FadeTransition), findsWidgets);

    // Hand-off is staggered: at ~32% the incoming page has not begun fading in
    // (opacity still 0), so it cannot be on screen beside the outgoing one — a
    // slide or a flat cross-fade would both fail this.
    final featureOpacities = tester
        .widgetList<FadeTransition>(
          find.ancestor(
            of: find.text('FEATURE'),
            matching: find.byType(FadeTransition),
          ),
        )
        .map((f) => f.opacity.value)
        .toList();
    expect(featureOpacities, isNotEmpty);
    expect(featureOpacities.any((v) => v < 0.01), isTrue);

    await tester.pumpAndSettle();
    expect(find.text('FEATURE'), findsOneWidget);
  });

  testWidgets('forward then back leaves only the origin page in the stack', (
    tester,
  ) async {
    final navKey = GlobalKey<NavigatorState>();
    await tester.pumpWidget(
      MaterialApp(
        theme: buildDarkTheme(),
        navigatorKey: navKey,
        home: const Scaffold(body: Center(child: Text('HOME'))),
      ),
    );

    navKey.currentState!.push(
      AppPageRoute<void>(
        builder: (_) => const Scaffold(body: Center(child: Text('FEATURE'))),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('FEATURE'), findsOneWidget);

    navKey.currentState!.pop();
    await tester.pumpAndSettle();

    // The popped route is fully gone (no leftover in the stack) and the origin
    // page is back.
    expect(find.text('FEATURE'), findsNothing);
    expect(find.text('HOME'), findsOneWidget);
  });
}
