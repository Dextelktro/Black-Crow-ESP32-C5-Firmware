import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/ble_spam_type.dart';
import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';
import 'ble_spam_type_screen.dart';

class BleSpamScreen extends StatelessWidget {
  const BleSpamScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('BLE Spam')),
      body: const BleSpamBody(),
    );
  }
}

/// BLE Spam screen body. The Home route provides the shared controller.
class BleSpamBody extends StatelessWidget {
  const BleSpamBody({super.key});

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final active = ctrl.bleSpamRunning;

    return Container(
      color: const Color(0xFF010D01),
      child: Column(
        children: [
          DeviceIssueBanner(controller: ctrl),
          if (active) _ActiveBanner(ctrl: ctrl),
          Expanded(
            child: GridView.builder(
              padding: const EdgeInsets.all(12),
              gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                crossAxisCount: 2,
                mainAxisSpacing: 10,
                crossAxisSpacing: 10,
                childAspectRatio: 1.05,
              ),
              itemCount: BleSpamType.values.length,
              itemBuilder: (_, i) => _TypeCard(
                type: BleSpamType.values[i],
                disabled: active && ctrl.bleSpamType != BleSpamType.values[i],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _ActiveBanner extends StatelessWidget {
  const _ActiveBanner({required this.ctrl});
  final DeviceController ctrl;

  @override
  Widget build(BuildContext context) {
    final t = ctrl.bleSpamType;
    final accent = t?.accent ?? const Color(0xFFFFB000);
    return Container(
      width: double.infinity,
      color: accent.withValues(alpha: 0.15),
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      child: Row(
        children: [
          Container(
            width: 8,
            height: 8,
            decoration: BoxDecoration(
              color: accent,
              shape: BoxShape.circle,
              boxShadow: [
                BoxShadow(
                  color: accent.withValues(alpha: 0.6),
                  blurRadius: 8,
                ),
              ],
            ),
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              'RUNNING: ${t?.label ?? "?"} · ${ctrl.bleSpamFrames} frames',
              style: TextStyle(
                color: accent,
                fontSize: 12,
                fontFamily: 'monospace',
                fontWeight: FontWeight.bold,
              ),
            ),
          ),
          TextButton(
            onPressed: () {
              unawaited(
                ctrl.bleSpamStop().catchError((Object e) {
                  debugPrint('bleSpamStop err: $e');
                  if (!context.mounted) return;
                  ScaffoldMessenger.of(context).showSnackBar(
                    SnackBar(content: Text('Unable to stop BLE task: $e')),
                  );
                }),
              );
            },
            child: const Text(
              'STOP',
              style: TextStyle(color: Color(0xFFFF4040)),
            ),
          ),
        ],
      ),
    );
  }
}

class _TypeCard extends StatelessWidget {
  const _TypeCard({required this.type, required this.disabled});
  final BleSpamType type;
  final bool disabled;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: disabled ? null : () => _onTap(context),
        child: Opacity(
          opacity: disabled ? 0.4 : 1.0,
          child: Container(
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(
              color: Colors.black.withValues(alpha: 0.4),
              borderRadius: BorderRadius.circular(12),
              border: Border.all(
                color: type.accent.withValues(alpha: 0.35),
                width: 1.2,
              ),
              boxShadow: [
                BoxShadow(
                  color: type.accent.withValues(alpha: 0.08),
                  blurRadius: 12,
                ),
              ],
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Icon(type.icon, color: type.accent, size: 22),
                    const Spacer(),
                    if (type.warnText != null)
                      Icon(
                        Icons.info_outline,
                        color: type.accent,
                        size: 14,
                      ),
                  ],
                ),
                const SizedBox(height: 8),
                Text(
                  type.label,
                  style: TextStyle(
                    color: type.accent,
                    fontWeight: FontWeight.bold,
                    fontSize: 14,
                    fontFamily: 'monospace',
                  ),
                ),
                const SizedBox(height: 3),
                Text(
                  type.subtitle,
                  style: TextStyle(
                    color: Colors.white.withValues(alpha: 0.5),
                    fontSize: 10,
                    fontFamily: 'monospace',
                  ),
                ),
                const Spacer(),
                Text(
                  '${type.defaultDelay}ms default',
                  style: TextStyle(
                    color: type.accent.withValues(alpha: 0.6),
                    fontSize: 9,
                    fontFamily: 'monospace',
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  void _onTap(BuildContext context) {
    final ctrl = context.read<DeviceController>();
    if (type.warnText == null) {
      _pushTypeScreen(context, ctrl);
      return;
    }
    showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: const Color(0xFF0A1A0A),
        title: Text(
          'Warning: ${type.label}',
          style: const TextStyle(color: Color(0xFFFF4040)),
        ),
        content: Text(
          type.warnText!,
          style: const TextStyle(color: Colors.white70, fontSize: 13),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('CANCEL'),
          ),
          TextButton(
            onPressed: () {
              Navigator.pop(ctx);
              _pushTypeScreen(context, ctrl);
            },
            child: const Text(
              'PROCEED',
              style: TextStyle(color: Color(0xFFFFB000)),
            ),
          ),
        ],
      ),
    );
  }

  void _pushTypeScreen(BuildContext context, DeviceController ctrl) {
    // Wrap in a provider so the pushed route (a child of the root Navigator)
    // can resolve the DeviceController — otherwise the detail screen renders
    // blank (ProviderNotFoundException).
    Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) => ChangeNotifierProvider.value(
          value: ctrl,
          child: BleSpamTypeScreen(controller: ctrl, type: type),
        ),
      ),
    );
  }
}
