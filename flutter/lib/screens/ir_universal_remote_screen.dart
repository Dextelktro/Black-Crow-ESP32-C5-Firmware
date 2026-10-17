import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/ir_models.dart';
import '../services/device_controller.dart';

/// A Flipper-style universal remote for one category: a grid of big buttons
/// that sweeps every built-in code for the selected function.
class IrUniversalRemoteScreen extends StatelessWidget {
  const IrUniversalRemoteScreen({super.key, required this.category});
  final IrCategory category;

  Future<void> _press(BuildContext context, IrButton b) async {
    final ctrl = context.read<DeviceController>();
    final messenger = ScaffoldMessenger.of(context);
    try {
      await ctrl.irUniversalSend(category.id, b.token);
      messenger.showSnackBar(
        SnackBar(
          duration: const Duration(milliseconds: 1200),
          content: Text('Sending ${b.label} …'),
        ),
      );
    } catch (e) {
      messenger.showSnackBar(SnackBar(content: Text('Send failed: $e')));
    }
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final color = category.color;
    final running = ctrl.irUnivRunning;

    return Scaffold(
      appBar: AppBar(title: Text(category.label)),
      body: Column(
        children: [
          _statusBar(context, ctrl, color, running),
          if (!category.builtIn) _captureHint(color),
          Expanded(
            child: GridView.count(
              padding: const EdgeInsets.all(18),
              crossAxisCount: 2,
              crossAxisSpacing: 16,
              mainAxisSpacing: 16,
              childAspectRatio: 1.15,
              children: [
                for (final b in category.buttons)
                  _RemoteButton(
                    button: b,
                    color: color,
                    enabled: !ctrl.irUnivRunning && !ctrl.irEmulateRunning,
                    onTap: () => _press(context, b),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _statusBar(
    BuildContext context,
    DeviceController ctrl,
    Color color,
    bool running,
  ) {
    return Container(
      width: double.infinity,
      margin: const EdgeInsets.fromLTRB(16, 14, 16, 0),
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: color.withValues(alpha: 0.35)),
      ),
      child: Row(
        children: [
          Container(
            width: 10,
            height: 10,
            decoration: BoxDecoration(
              color: running ? color : Colors.white24,
              shape: BoxShape.circle,
              boxShadow: running
                  ? [BoxShadow(color: color, blurRadius: 10)]
                  : null,
            ),
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              running
                  ? ctrl.irUnivStopping
                        ? 'Stopping after current signal…'
                        : 'Sweeping ${ctrl.irUnivCategory} / ${ctrl.irUnivButton}  '
                              '${ctrl.irUnivSent}/${ctrl.irUnivTotal}'
                              '${ctrl.irUnivBrand.isNotEmpty ? "  ·  ${ctrl.irUnivBrand}" : ""}'
                  : ctrl.irUnivCategory == category.id &&
                        ctrl.irUnivButton.isNotEmpty
                  ? '${ctrl.irUnivButton}: ${ctrl.irUnivSent} sent'
                        '${ctrl.irUnivFailed > 0 ? ", ${ctrl.irUnivFailed} failed" : ""}'
                        '${ctrl.irUnivError != null ? " · ${ctrl.irUnivError}" : ""}'
                        '${ctrl.irUnivStopped ? " (stopped)" : ""}'
                  : 'Tap a button — matching device codes are sent in turn',
              style: TextStyle(
                color: running ? color : Colors.white54,
                fontSize: 12,
                fontFamily: running ? 'monospace' : null,
              ),
            ),
          ),
          if (running && !ctrl.irUnivStopping)
            TextButton(
              onPressed: () async {
                try {
                  await ctrl.irUniversalStop();
                } catch (e) {
                  if (context.mounted) {
                    ScaffoldMessenger.of(context).showSnackBar(
                      SnackBar(content: Text('Stop failed: $e')),
                    );
                  }
                }
              },
              child: const Text('STOP'),
            ),
        ],
      ),
    );
  }

  Widget _captureHint(Color color) {
    return Container(
      margin: const EdgeInsets.fromLTRB(16, 10, 16, 0),
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.08),
        borderRadius: BorderRadius.circular(12),
      ),
      child: Row(
        children: [
          Icon(Icons.info_outline, color: color, size: 18),
          const SizedBox(width: 10),
          const Expanded(
            child: Text(
              'No built-in codes for this type yet. Use the Receiver to '
              'capture your remote, then replay it from Saved.',
              style: TextStyle(color: Colors.white70, fontSize: 11.5),
            ),
          ),
        ],
      ),
    );
  }
}

class _RemoteButton extends StatelessWidget {
  const _RemoteButton({
    required this.button,
    required this.color,
    required this.enabled,
    required this.onTap,
  });

  final IrButton button;
  final Color color;
  final bool enabled;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final isPower = button.token.startsWith('power');
    final base = isPower ? const Color(0xFFFF5050) : color;
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: enabled ? onTap : null,
        borderRadius: BorderRadius.circular(20),
        child: Container(
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(20),
            gradient: LinearGradient(
              colors: [
                base.withValues(alpha: enabled ? 0.20 : 0.06),
                Colors.white.withValues(alpha: 0.02),
              ],
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
            ),
            border: Border.all(
              color: base.withValues(alpha: enabled ? 0.5 : 0.15),
              width: 1.4,
            ),
            boxShadow: enabled
                ? [
                    BoxShadow(
                      color: base.withValues(alpha: 0.12),
                      blurRadius: 14,
                    ),
                  ]
                : null,
          ),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Icon(
                button.icon,
                color: enabled ? base : Colors.white24,
                size: 40,
              ),
              const SizedBox(height: 10),
              Text(
                button.label,
                style: TextStyle(
                  color: enabled ? Colors.white : Colors.white30,
                  fontSize: 15,
                  fontWeight: FontWeight.w600,
                  letterSpacing: 0.5,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
