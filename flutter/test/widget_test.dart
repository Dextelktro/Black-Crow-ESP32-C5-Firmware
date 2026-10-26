import 'package:deauther/main.dart';
import 'package:deauther/models/ble_spam_type.dart';
import 'package:deauther/services/api_server.dart';
import 'package:deauther/services/settings.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('BLE spam type IDs match firmware command tokens', () {
    expect(BleSpamType.sourApple.id, 'sour');
    expect(BleSpamType.fromId('sourapple'), BleSpamType.sourApple);
  });

  testWidgets('app boots into scan screen', (WidgetTester tester) async {
    final settings = Settings();
    // Dispose the API server up front so its HttpServer never binds a real
    // socket — and never starts the 120s idle-timeout timer — during the test.
    // _applySettings() bails at its `if (_disposed) return;` guard, so the
    // pending-timer invariant check stays clean. (The BLE scan path throws an
    // "unsupported platform" error that ScanScreen catches; it starts no timer.)
    final api = ApiServer(settings)..dispose();
    await tester.pumpWidget(DeautherApp(settings: settings, api: api));
    await tester.pump();
    expect(find.text('Black Crow'), findsOneWidget);
  });
}
