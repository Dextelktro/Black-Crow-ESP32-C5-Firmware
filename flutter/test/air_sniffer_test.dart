import 'package:deauther/services/device_controller.dart';
import 'package:flutter_test/flutter_test.dart';

/// Covers the air-sniffer flood-detection hold (see
/// [DeviceController.airThreatHoldNext]). The passive sniffer channel-hops, so
/// a flood on one channel is only heard for ~one dwell per ~1 s window and the
/// raw per-window alert blinks on and off as the scanner hops away and back.
/// The latch must keep the detection asserted across those gaps.
void main() {
  group('airThreatHoldNext', () {
    test('an alerting window latches the hold to the full window count', () {
      expect(DeviceController.airThreatHoldNext(0, true, holdWindows: 6), 6);
      // Re-alerting mid-decay re-latches to the top.
      expect(DeviceController.airThreatHoldNext(2, true, holdWindows: 6), 6);
    });

    test('quiet windows count the hold down but never below zero', () {
      expect(DeviceController.airThreatHoldNext(6, false), 5);
      expect(DeviceController.airThreatHoldNext(1, false), 0);
      expect(DeviceController.airThreatHoldNext(0, false), 0);
    });

    test('detection stays asserted across a multi-window hop gap', () {
      // One alerting window, then a run of quiet windows (scanner hopped away).
      var hold = DeviceController.airThreatHoldNext(0, true, holdWindows: 6);
      final active = <bool>[];
      for (var i = 0; i < 8; i++) {
        active.add(hold > 0);
        hold = DeviceController.airThreatHoldNext(hold, false, holdWindows: 6);
      }
      // Held true for the whole ~6-window gap (longer than one 2.4+5 GHz
      // channel cycle), then releases — no blinking in between.
      expect(active, [true, true, true, true, true, true, false, false]);
    });

    test('a flood that re-triggers each cycle never drops out', () {
      // The attacked channel is revisited roughly every few windows; each visit
      // re-latches long before the hold decays, so the UI never flickers off.
      var hold = 0;
      for (var window = 0; window < 20; window++) {
        final alerting = window % 3 == 0; // caught once every 3 windows
        hold = DeviceController.airThreatHoldNext(hold, alerting, holdWindows: 6);
        if (window >= 3) expect(hold > 0, isTrue, reason: 'window $window');
      }
    });
  });
}
