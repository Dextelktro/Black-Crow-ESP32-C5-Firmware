import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/ir_models.dart';
import '../services/device_controller.dart';
import 'ir_hub_screen.dart';

/// Saved IR signal library (LittleFS). Pick a signal and start/stop emulating
/// (replaying) it.
class IrSavedScreen extends StatefulWidget {
  const IrSavedScreen({super.key});

  @override
  State<IrSavedScreen> createState() => _IrSavedScreenState();
}

class _IrSavedScreenState extends State<IrSavedScreen> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      context.read<DeviceController>().irSavedRefresh();
    });
  }

  Future<void> _guard(Future<void> f) async {
    final messenger = ScaffoldMessenger.of(context);
    try {
      await f;
    } catch (e) {
      messenger.showSnackBar(SnackBar(content: Text('Failed: $e')));
    }
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    return Scaffold(
      appBar: AppBar(
        title: const Text('Infrared Saved'),
        actions: [
          IconButton(
            tooltip: 'Refresh',
            icon: const Icon(Icons.refresh),
            onPressed: () => _guard(ctrl.irSavedRefresh()),
          ),
        ],
      ),
      body: Column(
        children: [
          if (ctrl.irEmulateRunning) _emulateBanner(ctrl),
          Expanded(
            child: ctrl.irSaved.isEmpty
                ? _empty()
                : ListView.separated(
                    padding: const EdgeInsets.all(14),
                    itemCount: ctrl.irSaved.length,
                    separatorBuilder: (_, _) => const SizedBox(height: 10),
                    itemBuilder: (_, i) => _tile(ctrl, ctrl.irSaved[i]),
                  ),
          ),
        ],
      ),
    );
  }

  Widget _empty() {
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(
            Icons.bookmark_border,
            size: 48,
            color: Colors.white.withValues(alpha: 0.15),
          ),
          const SizedBox(height: 12),
          const Text(
            'No saved signals',
            style: TextStyle(color: Colors.white54, fontSize: 14),
          ),
          const SizedBox(height: 6),
          const Text(
            'Capture one in the Receiver, then Save it',
            style: TextStyle(color: Colors.white38, fontSize: 12),
          ),
        ],
      ),
    );
  }

  Widget _emulateBanner(DeviceController ctrl) {
    return Container(
      width: double.infinity,
      margin: const EdgeInsets.fromLTRB(14, 14, 14, 0),
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      decoration: BoxDecoration(
        gradient: LinearGradient(
          colors: [
            kIrAccent.withValues(alpha: 0.25),
            kIrAccent2.withValues(alpha: 0.12),
          ],
        ),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: kIrAccent.withValues(alpha: 0.5)),
      ),
      child: Row(
        children: [
          const _PulseDot(),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  ctrl.irEmulateStopping
                      ? 'STOPPING AFTER SIGNAL'
                      : 'EMULATING',
                  style: TextStyle(
                    color: kIrAccent,
                    fontSize: 11,
                    letterSpacing: 2,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  ctrl.irEmulateName,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(color: Colors.white, fontSize: 14),
                ),
              ],
            ),
          ),
          ElevatedButton.icon(
            style: ElevatedButton.styleFrom(
              backgroundColor: const Color(0xFF8A1F1F),
            ),
            icon: const Icon(Icons.stop, size: 18),
            label: Text(ctrl.irEmulateStopping ? 'Stopping…' : 'Stop'),
            onPressed: ctrl.irEmulateStopping
                ? null
                : () => _guard(ctrl.irEmulateStop()),
          ),
        ],
      ),
    );
  }

  Widget _tile(DeviceController ctrl, IrSavedEntry e) {
    final isEmulating = ctrl.irEmulateRunning && ctrl.irEmulateName == e.name;
    final busy = ctrl.irEmulateRunning && !isEmulating;
    final color = isEmulating ? kIrAccent : const Color(0xFF2EE6C5);
    return Container(
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.03),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(
          color: color.withValues(alpha: isEmulating ? 0.6 : 0.3),
          width: isEmulating ? 1.5 : 1,
        ),
      ),
      padding: const EdgeInsets.all(12),
      child: Row(
        children: [
          Container(
            width: 44,
            height: 44,
            decoration: BoxDecoration(
              color: color.withValues(alpha: 0.14),
              borderRadius: BorderRadius.circular(12),
            ),
            child: Icon(Icons.graphic_eq, color: color, size: 22),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  e.name,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 15,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  e.subtitle,
                  style: const TextStyle(color: Colors.white54, fontSize: 11),
                ),
              ],
            ),
          ),
          IconButton(
            tooltip: 'Delete',
            icon: const Icon(Icons.delete_outline, size: 20),
            color: Colors.white38,
            onPressed: isEmulating ? null : () => _confirmDelete(ctrl, e.name),
          ),
          SizedBox(
            height: 40,
            child: ElevatedButton(
              style: ElevatedButton.styleFrom(
                backgroundColor: isEmulating ? const Color(0xFF8A1F1F) : color,
                foregroundColor: isEmulating ? Colors.white : Colors.black,
                padding: const EdgeInsets.symmetric(horizontal: 14),
              ),
              onPressed: busy || (isEmulating && ctrl.irEmulateStopping)
                  ? null
                  : () => _guard(
                      isEmulating
                          ? ctrl.irEmulateStop()
                          : ctrl.irEmulateStart(e.name),
                    ),
              child: Icon(isEmulating ? Icons.stop : Icons.play_arrow),
            ),
          ),
        ],
      ),
    );
  }

  Future<void> _confirmDelete(DeviceController ctrl, String name) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (dctx) => AlertDialog(
        title: const Text('Delete signal?'),
        content: Text('Remove "$name" from storage?'),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(dctx, false),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () => Navigator.pop(dctx, true),
            child: const Text('Delete'),
          ),
        ],
      ),
    );
    if (ok == true) {
      await _guard(ctrl.irSavedDelete(name));
    }
  }
}

class _PulseDot extends StatefulWidget {
  const _PulseDot();
  @override
  State<_PulseDot> createState() => _PulseDotState();
}

class _PulseDotState extends State<_PulseDot>
    with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 700),
  )..repeat(reverse: true);

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _c,
      builder: (_, _) => Container(
        width: 12,
        height: 12,
        decoration: BoxDecoration(
          color: kIrAccent,
          shape: BoxShape.circle,
          boxShadow: [
            BoxShadow(
              color: kIrAccent.withValues(alpha: 0.4 + 0.6 * _c.value),
              blurRadius: 4 + 8 * _c.value,
            ),
          ],
        ),
      ),
    );
  }
}
