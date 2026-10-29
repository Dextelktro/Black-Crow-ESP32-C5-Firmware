import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../models/subghz_models.dart';
import '../services/device_controller.dart';
import '../widgets/subghz_kit.dart';
import 'subghz_radio_settings_screen.dart';

/// Human-facing protocol label. The decoder reports (and `.sub` files store)
/// "Princeton" for the whole 24-bit 3:1 learning-code family, but most 433/315
/// remotes are EV1527 — so surface both names to the user. The stored/wire name
/// stays "Princeton" for Flipper interop.
String sgProtoDisplay(String proto) {
  if (proto == 'Princeton') return 'Princeton / EV1527';
  return proto;
}

String timeAgo(DateTime at) {
  final s = DateTime.now().difference(at).inSeconds;
  if (s < 1) return 'now';
  if (s < 60) return '${s}s ago';
  final m = s ~/ 60;
  if (m < 60) return '${m}m ago';
  return '${m ~/ 60}h ago';
}

void closeSubghzSessionOnExit(
  Future<void> Function() closeSession,
  String screenName,
) {
  unawaited(
    closeSession().catchError((Object error, StackTrace stackTrace) {
      debugPrint(
        'Failed to close $screenName cleanly: $error\n$stackTrace',
      );
    }),
  );
}

/// A dismissible one-line result banner driven by [DeviceController.subghzNotice]
/// (SAVED / RM / ERR / NOCAP / WARN). Error-ish text reads red.
class SubghzNoticeBanner extends StatelessWidget {
  const SubghzNoticeBanner({super.key, required this.ctrl});
  final DeviceController ctrl;

  @override
  Widget build(BuildContext context) {
    final notice = ctrl.subghzNotice;
    if (notice == null) return const SizedBox.shrink();
    final bad =
        notice.contains('ERR') ||
        notice.contains('error') ||
        notice.contains('absent') ||
        notice.contains('no-storage') ||
        notice.contains('invalid') ||
        notice.contains('unsupported') ||
        notice.contains('mismatch') ||
        notice.contains('Nothing');
    final color = bad ? kSgDanger : kSgAmber;
    return Container(
      margin: const EdgeInsets.fromLTRB(12, 10, 12, 0),
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.12),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: color.withValues(alpha: 0.5)),
      ),
      child: Row(
        children: [
          Icon(bad ? Icons.error_outline : Icons.check_circle_outline,
              color: color, size: 18),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              notice,
              style: TextStyle(color: color, fontSize: 12.5),
            ),
          ),
          InkWell(
            onTap: ctrl.subghzClearNotice,
            borderRadius: BorderRadius.circular(20),
            child: const Padding(
              padding: EdgeInsets.all(2),
              child: Icon(Icons.close, color: Colors.white54, size: 16),
            ),
          ),
        ],
      ),
    );
  }
}

/// Compact frequency + preset strip with a "Tune" shortcut to Radio Settings.
class SubghzConfigStrip extends StatelessWidget {
  const SubghzConfigStrip({super.key, required this.ctrl});
  final DeviceController ctrl;

  @override
  Widget build(BuildContext context) {
    final preset = SubghzPreset.byId(ctrl.subghzPresetId);
    final inBand = sgFreqInBand(ctrl.subghzFreqHz);
    final modColor = preset.modulation == SubghzModulation.fm
        ? const Color(0xFF9B8CFF)
        : kSgAmber;
    return SgPanel(
      padding: const EdgeInsets.fromLTRB(14, 12, 8, 12),
      child: Row(
        children: [
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                crossAxisAlignment: CrossAxisAlignment.baseline,
                textBaseline: TextBaseline.alphabetic,
                children: [
                  Text(
                    sgMhz(ctrl.subghzFreqHz, decimals: 3),
                    style: TextStyle(
                      color: inBand ? kSgAmber : kSgDanger,
                      fontSize: 24,
                      fontWeight: FontWeight.w800,
                      fontFamily: 'monospace',
                    ),
                  ),
                  const SizedBox(width: 4),
                  const Text('MHz',
                      style: TextStyle(color: Colors.white54, fontSize: 11)),
                ],
              ),
              const SizedBox(height: 2),
              Row(
                children: [
                  Text(
                    preset.label,
                    style: const TextStyle(color: Colors.white54, fontSize: 11),
                  ),
                  const SizedBox(width: 6),
                  Container(
                    padding:
                        const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
                    decoration: BoxDecoration(
                      color: modColor.withValues(alpha: 0.15),
                      borderRadius: BorderRadius.circular(5),
                    ),
                    child: Text(
                      preset.modulation.label,
                      style: TextStyle(
                        color: modColor,
                        fontSize: 9,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                  ),
                ],
              ),
            ],
          ),
          const Spacer(),
          TextButton.icon(
            onPressed: () => pushSubghz(
              context,
              ctrl,
              const SubghzRadioSettingsScreen(),
            ),
            icon: const Icon(Icons.tune, size: 18, color: kSgAmber),
            label: const Text('Tune', style: TextStyle(color: kSgAmber)),
          ),
        ],
      ),
    );
  }
}

bool _validSubghzName(String n) {
  if (n.isEmpty || n.length > 32) return false;
  if (n == '.' || n == '..') return false;
  for (final u in n.codeUnits) {
    if (u < 0x20 || u == 0x2f || u == 0x5c || u == 0x3a || u == 0x22 ||
        u == 0x20) {
      return false; // control, / \ : " space
    }
  }
  return true;
}

/// Ask for a `.sub` file name. First enforces a storage target is chosen
/// (manual, no fallback); offers to open Radio Settings if not. Returns the
/// validated base name (without extension) or null if cancelled.
Future<String?> promptSubghzName(
  BuildContext context,
  DeviceController ctrl,
) async {
  if (ctrl.subghzStorageTarget == SubghzStorageTarget.unset) {
    final go = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: const Text('Choose a save location first'),
        content: const Text(
          'Saving is manual: pick internal flash or SD card in Radio Settings '
          'before saving a capture.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(c).pop(false),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () => Navigator.of(c).pop(true),
            child: const Text('Open Radio Settings'),
          ),
        ],
      ),
    );
    if (go == true && context.mounted) {
      pushSubghz(context, ctrl, const SubghzRadioSettingsScreen());
    }
    return null;
  }

  final controller = TextEditingController();
  if (!context.mounted) return null;
  final name = await showDialog<String>(
    context: context,
    builder: (c) => StatefulBuilder(
      builder: (c, setLocal) {
        final text = controller.text.trim();
        final valid = _validSubghzName(text);
        return AlertDialog(
          title: const Text('Save as'),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              TextField(
                controller: controller,
                autofocus: true,
                inputFormatters: [
                  FilteringTextInputFormatter.deny(RegExp(r'[ /\\:"]')),
                  LengthLimitingTextInputFormatter(32),
                ],
                decoration: const InputDecoration(
                  hintText: 'e.g. garage_gate',
                  suffixText: '.sub',
                ),
                onChanged: (_) => setLocal(() {}),
                onSubmitted: (v) {
                  if (_validSubghzName(v.trim())) Navigator.of(c).pop(v.trim());
                },
              ),
              const SizedBox(height: 8),
              Text(
                'Target: ${ctrl.subghzStorageTarget.label}',
                style: const TextStyle(color: Colors.white38, fontSize: 11),
              ),
            ],
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(c).pop(),
              child: const Text('Cancel'),
            ),
            TextButton(
              onPressed:
                  valid ? () => Navigator.of(c).pop(text) : null,
              child: const Text('Save'),
            ),
          ],
        );
      },
    ),
  );
  return name;
}
