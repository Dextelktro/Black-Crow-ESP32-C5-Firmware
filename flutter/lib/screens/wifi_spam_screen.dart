import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';
import 'wifi_spam_mode_screen.dart';

const _kAccent = Color(0xFFFF6A00);

/// A WiFi-spam mode descriptor shown in the grid.
class WspamMode {
  const WspamMode(this.id, this.label, this.subtitle, this.icon, this.accent);
  final String id;
  final String label;
  final String subtitle;
  final IconData icon;
  final Color accent;
}

const List<WspamMode> kWspamModes = [
  WspamMode('rickroll', 'Rickroll', 'Never gonna give you up',
      Icons.music_note, Color(0xFFFF2D7A)),
  WspamMode('random', 'Random', 'Random SSID + MAC', Icons.shuffle,
      Color(0xFF00E5FF)),
  WspamMode('funny', 'Funny', 'Meme network names',
      Icons.sentiment_very_satisfied, Color(0xFFFFC107)),
  WspamMode('all', 'All', 'Everything mixed', Icons.all_inclusive,
      Color(0xFF39FF14)),
  WspamMode('custom', 'Custom', 'Your own SSID list', Icons.edit_note,
      Color(0xFF9B5CFF)),
];

class WifiSpamScreen extends StatelessWidget {
  const WifiSpamScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final active = ctrl.wspamRunning;

    return Scaffold(
      appBar: AppBar(
        iconTheme: const IconThemeData(color: _kAccent),
        title: const Text(
          'WIFI SPAM',
          style: TextStyle(
            color: _kAccent,
            fontWeight: FontWeight.bold,
            letterSpacing: 2,
            fontSize: 16,
          ),
        ),
      ),
      body: Column(
        children: [
          DeviceIssueBanner(controller: ctrl),
          if (active) _activeBanner(context, ctrl),
          Expanded(
            child: GridView.builder(
              padding: const EdgeInsets.all(12),
              gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                crossAxisCount: 2,
                mainAxisSpacing: 10,
                crossAxisSpacing: 10,
                childAspectRatio: 1.05,
              ),
              itemCount: kWspamModes.length,
              itemBuilder: (_, i) {
                final m = kWspamModes[i];
                final isActiveMode = active && ctrl.wspamMode == m.id;
                return _ModeCard(
                  mode: m,
                  disabled: active && !isActiveMode,
                  running: isActiveMode,
                  onTap: () => Navigator.of(context).push(
                    MaterialPageRoute(
                      builder: (_) => ChangeNotifierProvider.value(
                        value: ctrl,
                        child: WifiSpamModeScreen(mode: m),
                      ),
                    ),
                  ),
                );
              },
            ),
          ),
        ],
      ),
    );
  }

  Widget _activeBanner(BuildContext context, DeviceController ctrl) {
    return Container(
      width: double.infinity,
      color: _kAccent.withValues(alpha: 0.15),
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      child: Row(
        children: [
          Container(
            width: 8,
            height: 8,
            decoration: BoxDecoration(
              color: _kAccent,
              shape: BoxShape.circle,
              boxShadow: [BoxShadow(color: _kAccent, blurRadius: 8)],
            ),
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              'RUNNING: ${ctrl.wspamMode} · ${ctrl.wspamTotal} beacons',
              style: const TextStyle(
                color: _kAccent,
                fontSize: 12,
                fontFamily: 'monospace',
                fontWeight: FontWeight.bold,
              ),
            ),
          ),
          TextButton(
            onPressed: () => unawaited(ctrl.wspamStop().catchError((Object _) {})),
            child: const Text('STOP',
                style: TextStyle(color: Color(0xFFFF4040))),
          ),
        ],
      ),
    );
  }
}

class _ModeCard extends StatelessWidget {
  const _ModeCard({
    required this.mode,
    required this.disabled,
    required this.running,
    required this.onTap,
  });
  final WspamMode mode;
  final bool disabled;
  final bool running;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        borderRadius: BorderRadius.circular(14),
        onTap: disabled ? null : onTap,
        child: Opacity(
          opacity: disabled ? 0.4 : 1.0,
          child: Container(
            padding: const EdgeInsets.all(14),
            decoration: BoxDecoration(
              color: Colors.black.withValues(alpha: 0.4),
              borderRadius: BorderRadius.circular(14),
              border: Border.all(
                color: mode.accent.withValues(alpha: running ? 0.9 : 0.35),
                width: running ? 1.8 : 1.2,
              ),
              boxShadow: [
                BoxShadow(
                    color: mode.accent.withValues(alpha: running ? 0.25 : 0.08),
                    blurRadius: running ? 18 : 12),
              ],
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Icon(mode.icon, color: mode.accent, size: 26),
                    const Spacer(),
                    if (running)
                      Container(
                        width: 8,
                        height: 8,
                        decoration: BoxDecoration(
                          color: mode.accent,
                          shape: BoxShape.circle,
                          boxShadow: [
                            BoxShadow(color: mode.accent, blurRadius: 8)
                          ],
                        ),
                      ),
                  ],
                ),
                const Spacer(),
                Text(
                  mode.label,
                  style: TextStyle(
                    color: mode.accent,
                    fontWeight: FontWeight.bold,
                    fontSize: 16,
                    fontFamily: 'monospace',
                  ),
                ),
                const SizedBox(height: 3),
                Text(
                  mode.subtitle,
                  style: TextStyle(
                    color: Colors.white.withValues(alpha: 0.5),
                    fontSize: 11,
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
