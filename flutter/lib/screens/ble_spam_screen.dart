import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/ble_spam_type.dart';
import '../services/device_controller.dart';
import '../theme/app_theme.dart';
import '../widgets/device_issue_banner.dart';
import '../widgets/glass_surface.dart';
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

    return Column(
      children: [
        DeviceIssueBanner(controller: ctrl),
        if (active) _ActiveBanner(ctrl: ctrl),
        Expanded(
          child: GridView.builder(
            padding: const EdgeInsets.fromLTRB(14, 14, 14, 20),
            gridDelegate: const SliverGridDelegateWithMaxCrossAxisExtent(
              maxCrossAxisExtent: 200,
              mainAxisSpacing: 12,
              crossAxisSpacing: 12,
              mainAxisExtent: 142,
            ),
            itemCount: BleSpamType.values.length,
            itemBuilder: (_, i) {
              final type = BleSpamType.values[i];
              return _TypeCard(
                type: type,
                disabled: active && ctrl.bleSpamType != type,
                selected: active && ctrl.bleSpamType == type,
              );
            },
          ),
        ),
      ],
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
  const _TypeCard({
    required this.type,
    required this.disabled,
    this.selected = false,
  });
  final BleSpamType type;
  final bool disabled;
  final bool selected;

  @override
  Widget build(BuildContext context) {
    return GlassCard(
      onTap: disabled ? null : () => _onTap(context),
      disabled: disabled,
      padding: const EdgeInsets.all(13),
      radius: 14,
      accent: type.accent,
      active: selected,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(type.icon, color: type.accent, size: 22),
              const Spacer(),
              if (type.warnText != null)
                const Icon(
                  Icons.info_outline,
                  color: AppColors.warning,
                  size: 14,
                ),
            ],
          ),
          const SizedBox(height: 10),
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
          Expanded(
            child: Text(
              type.subtitle,
              maxLines: 2,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(
                color: AppColors.textDim,
                fontSize: 10,
                fontFamily: 'monospace',
              ),
            ),
          ),
          Text(
            '${type.defaultDelay}ms default',
            style: const TextStyle(
              color: AppColors.textFaint,
              fontSize: 9,
              fontFamily: 'monospace',
            ),
          ),
        ],
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
