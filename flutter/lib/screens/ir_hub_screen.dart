import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/glass_surface.dart';
import 'ir_receiver_screen.dart';
import 'ir_saved_screen.dart';
import 'ir_universal_screen.dart';

const kIrAccent = Color(0xFFFF5A52);
const kIrAccent2 = Color(0xFFFF9E40);

/// Push a route that re-wraps the shared [DeviceController] provider, so the
/// pushed IR screen keeps observing the same device (see flutter-provider
/// route pattern in project memory).
void pushIr(BuildContext context, DeviceController ctrl, Widget screen) {
  Navigator.of(context).push(
    MaterialPageRoute(
      builder: (_) => ChangeNotifierProvider.value(value: ctrl, child: screen),
    ),
  );
}

/// Top-level Infrared menu: Universal remote, Receiver, Saved library.
class IrHubScreen extends StatefulWidget {
  const IrHubScreen({super.key});

  @override
  State<IrHubScreen> createState() => _IrHubScreenState();
}

class _IrHubScreenState extends State<IrHubScreen> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final ctrl = context.read<DeviceController>();
      ctrl.irStatus();
      ctrl.irSavedRefresh();
    });
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    return Scaffold(
      appBar: AppBar(title: const Text('Infrared')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _IrHero(ready: ctrl.irReady, known: ctrl.irModuleKnown),
          const SizedBox(height: 18),
          _IrMenuCard(
            icon: Icons.settings_remote,
            color: kIrAccent,
            title: 'Universal Remote',
            subtitle: 'TV · A/C · Projector · LED · Fan · Audio',
            onTap: () => pushIr(context, ctrl, const IrUniversalScreen()),
          ),
          const SizedBox(height: 12),
          _IrMenuCard(
            icon: Icons.sensors,
            color: const Color(0xFF4FA8FF),
            title: 'Infrared Receiver',
            subtitle: 'Capture · live spectrum · decode · save',
            onTap: () => pushIr(context, ctrl, const IrReceiverScreen()),
          ),
          const SizedBox(height: 12),
          _IrMenuCard(
            icon: Icons.bookmarks,
            color: const Color(0xFF2EE6C5),
            title: 'Saved Signals',
            subtitle: ctrl.irSaved.isEmpty
                ? 'Replay captured signals from storage'
                : '${ctrl.irSaved.length} saved · tap to emulate',
            onTap: () => pushIr(context, ctrl, const IrSavedScreen()),
          ),
          const SizedBox(height: 18),
          if (ctrl.irFsTotal > 0) _storageBar(ctrl),
        ],
      ),
    );
  }

  Widget _storageBar(DeviceController ctrl) {
    final frac = ctrl.irFsTotal == 0 ? 0.0 : ctrl.irFsUsed / ctrl.irFsTotal;
    String kb(int b) => '${(b / 1024).toStringAsFixed(0)} KB';
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            const Icon(Icons.sd_storage, size: 15, color: Colors.white38),
            const SizedBox(width: 6),
            Text('Storage  ${kb(ctrl.irFsUsed)} / ${kb(ctrl.irFsTotal)}',
                style: const TextStyle(color: Colors.white38, fontSize: 11)),
          ],
        ),
        const SizedBox(height: 6),
        ClipRRect(
          borderRadius: BorderRadius.circular(6),
          child: LinearProgressIndicator(
            value: frac.clamp(0.0, 1.0),
            minHeight: 6,
            backgroundColor: Colors.white.withValues(alpha: 0.06),
            valueColor: const AlwaysStoppedAnimation(kIrAccent),
          ),
        ),
      ],
    );
  }
}

class _IrHero extends StatelessWidget {
  const _IrHero({required this.ready, required this.known});
  final bool ready;
  final bool known;

  @override
  Widget build(BuildContext context) {
    final statusText = !known
        ? 'Checking…'
        : ready
            ? 'READY · TX GPIO4 · RX GPIO5'
            : 'IR MODULE UNAVAILABLE';
    final statusColor = ready || !known ? kIrAccent : const Color(0xFFFF5050);
    return Container(
      padding: const EdgeInsets.all(20),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(20),
        gradient: const LinearGradient(
          colors: [Color(0xFF2A0A08), Color(0xFF130605)],
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
        ),
        border: Border.all(color: glassNeutralBorder),
      ),
      child: Row(
        children: [
          Container(
            width: 54,
            height: 54,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              gradient: const LinearGradient(
                colors: [kIrAccent, kIrAccent2],
              ),
              boxShadow: [
                BoxShadow(color: kIrAccent.withValues(alpha: 0.5), blurRadius: 16),
              ],
            ),
            child: const Icon(Icons.wifi_tethering, color: Colors.white, size: 28),
          ),
          const SizedBox(width: 16),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Text('INFRARED',
                    style: TextStyle(
                        color: Colors.white,
                        fontSize: 22,
                        fontWeight: FontWeight.w800,
                        letterSpacing: 3)),
                const SizedBox(height: 4),
                Text(statusText,
                    style: TextStyle(
                        color: statusColor,
                        fontSize: 11,
                        letterSpacing: 1.2,
                        fontFamily: 'monospace')),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _IrMenuCard extends StatelessWidget {
  const _IrMenuCard({
    required this.icon,
    required this.color,
    required this.title,
    required this.subtitle,
    required this.onTap,
  });

  final IconData icon;
  final Color color;
  final String title;
  final String subtitle;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(16),
        child: Container(
          padding: const EdgeInsets.all(16),
          decoration: glassSurfaceDecoration(
            context,
            borderRadius: BorderRadius.circular(16),
          ),
          child: Row(
            children: [
              Container(
                width: 50,
                height: 50,
                decoration: BoxDecoration(
                  color: color.withValues(alpha: 0.12),
                  borderRadius: BorderRadius.circular(14),
                ),
                child: Icon(icon, color: color, size: 26),
              ),
              const SizedBox(width: 16),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(title,
                        style: const TextStyle(
                            color: Colors.white,
                            fontSize: 16,
                            fontWeight: FontWeight.bold)),
                    const SizedBox(height: 3),
                    Text(subtitle,
                        style: const TextStyle(
                            color: Colors.white54, fontSize: 12)),
                  ],
                ),
              ),
              Icon(Icons.chevron_right, color: color.withValues(alpha: 0.6)),
            ],
          ),
        ),
      ),
    );
  }
}
