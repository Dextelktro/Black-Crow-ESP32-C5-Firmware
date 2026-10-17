import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';
import 'nrf24_jammer_screen.dart';
import 'nrf24_mousejack_screen.dart';
import 'nrf24_scanner_screen.dart';
import 'nrf24_sniffer_screen.dart';

const _kBg = Color(0xFF12060C);
const _kAccent = Color(0xFFFF2D78);

/// Hub for the nRF24 2.4 GHz radio toolkit. Lists the four features; each
/// opens its own full-screen sub-view. Shows live module + radio status.
class Nrf24HubScreen extends StatefulWidget {
  const Nrf24HubScreen({super.key});

  @override
  State<Nrf24HubScreen> createState() => _Nrf24HubScreenState();
}

class _Nrf24HubScreenState extends State<Nrf24HubScreen> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      // Refresh presence + busy state when the hub opens.
      unawaited(
          context.read<DeviceController>().nrfStatus().catchError((Object _) {}));
    });
  }

  void _push(BuildContext context, DeviceController ctrl, Widget screen) {
    Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) =>
            ChangeNotifierProvider.value(value: ctrl, child: screen),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();

    final bool known = ctrl.nrfModuleKnown;
    final bool present = ctrl.nrfPresent;
    final Color statusColor = !known
        ? Colors.white38
        : present
            ? const Color(0xFF2EE6C5)
            : const Color(0xFFFF5050);
    final String statusText = !known
        ? 'PROBING…'
        : present
            ? 'DETECTED'
            : 'NOT DETECTED';

    String busyLabel = 'IDLE';
    if (ctrl.nrfJamRunning) {
      busyLabel = 'JAMMER';
    } else if (ctrl.nrfSniffRunning) {
      busyLabel = 'SNIFFER';
    } else if (ctrl.nrfMjRunning) {
      busyLabel = 'MOUSEJACK';
    } else if (ctrl.nrfScanRunning) {
      busyLabel = 'SCANNER';
    } else if (ctrl.nrfReplayActive) {
      busyLabel = 'REPLAY';
    }

    return Scaffold(
      backgroundColor: _kBg,
      appBar: AppBar(
        backgroundColor: Colors.black,
        iconTheme: const IconThemeData(color: _kAccent),
        title: const Text(
          'nRF24 TOOLKIT',
          style: TextStyle(
              color: _kAccent,
              fontWeight: FontWeight.bold,
              letterSpacing: 2,
              fontSize: 16),
        ),
      ),
      body: ListView(
        padding: const EdgeInsets.all(14),
        children: [
          DeviceIssueBanner(controller: ctrl),

          // ── Module status hero ──────────────────────────────
          Container(
            padding: const EdgeInsets.all(18),
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(18),
              gradient: const LinearGradient(
                colors: [Color(0xFF26091A), Color(0xFF12060C)],
                begin: Alignment.topLeft,
                end: Alignment.bottomRight,
              ),
              border: Border.all(color: _kAccent.withValues(alpha: 0.35)),
              boxShadow: [
                BoxShadow(
                    color: _kAccent.withValues(alpha: 0.10), blurRadius: 18),
              ],
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    const Icon(Icons.settings_input_antenna,
                        color: _kAccent, size: 20),
                    const SizedBox(width: 8),
                    const Expanded(
                      child: Text(
                        'nRF24L01+ · 2.4 GHz',
                        style: TextStyle(
                            color: Colors.white,
                            fontSize: 15,
                            fontWeight: FontWeight.bold),
                      ),
                    ),
                    Container(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 10, vertical: 4),
                      decoration: BoxDecoration(
                        color: statusColor.withValues(alpha: 0.15),
                        borderRadius: BorderRadius.circular(20),
                        border:
                            Border.all(color: statusColor.withValues(alpha: 0.5)),
                      ),
                      child: Text(
                        statusText,
                        style: TextStyle(
                          color: statusColor,
                          fontSize: 10,
                          fontWeight: FontWeight.bold,
                          letterSpacing: 1.2,
                        ),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 14),
                Row(
                  children: [
                    _stat('RADIO', ctrl.nrfRadioBusy ? 'BUSY' : 'FREE',
                        ctrl.nrfRadioBusy ? const Color(0xFFFFB000) : _kAccent),
                    const SizedBox(width: 10),
                    _stat('ACTIVE', busyLabel, _kAccent),
                    const SizedBox(width: 10),
                    _stat('SPI', 'CSN9·CE8', Colors.white38),
                  ],
                ),
                if (known && !present) ...[
                  const SizedBox(height: 14),
                  Text(
                    'No nRF24 module responded on SPI2 (SCK6 MISO2 MOSI7 '
                    'CSN9 CE8). Check wiring and 3V3 power, then reopen.',
                    style: TextStyle(
                        color: Colors.white.withValues(alpha: 0.45),
                        fontSize: 11,
                        height: 1.4),
                  ),
                ],
              ],
            ),
          ),
          const SizedBox(height: 16),

          const Padding(
            padding: EdgeInsets.only(left: 4, bottom: 8),
            child: Text(
              'FEATURES',
              style: TextStyle(
                color: Colors.white38,
                fontSize: 11,
                letterSpacing: 2,
                fontWeight: FontWeight.bold,
              ),
            ),
          ),

          _NrfTool(
            icon: Icons.radar,
            color: const Color(0xFF00E0B8),
            title: 'Spectrum Scanner',
            subtitle: 'RPD energy across all 126 channels + peak finder',
            running: ctrl.nrfScanRunning,
            onTap: () => _push(context, ctrl, const Nrf24ScannerScreen()),
          ),
          const SizedBox(height: 10),
          _NrfTool(
            icon: Icons.flash_on,
            color: const Color(0xFFFF3B30),
            title: 'Jammer',
            subtitle: 'CW / flood / reactive — WiFi, BLE, Zigbee, drone…',
            running: ctrl.nrfJamRunning,
            onTap: () => _push(context, ctrl, const Nrf24JammerScreen()),
          ),
          const SizedBox(height: 10),
          _NrfTool(
            icon: Icons.sensors,
            color: const Color(0xFFFFB000),
            title: 'Sniffer',
            subtitle: 'Pseudo-promiscuous capture + packet replay',
            running: ctrl.nrfSniffRunning || ctrl.nrfReplayActive,
            onTap: () => _push(context, ctrl, const Nrf24SnifferScreen()),
          ),
          const SizedBox(height: 10),
          _NrfTool(
            icon: Icons.mouse,
            color: const Color(0xFFB06CFF),
            title: 'MouseJack',
            subtitle: 'Find wireless keyboards/mice + keystroke inject',
            running: ctrl.nrfMjRunning || ctrl.nrfMjInjecting,
            onTap: () => _push(context, ctrl, const Nrf24MousejackScreen()),
          ),
        ],
      ),
    );
  }

  Widget _stat(String label, String value, Color c) => Expanded(
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 10),
          decoration: BoxDecoration(
            color: Colors.black.withValues(alpha: 0.35),
            borderRadius: BorderRadius.circular(10),
            border: Border.all(color: c.withValues(alpha: 0.25)),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(label,
                  style: const TextStyle(
                      color: Colors.white38,
                      fontSize: 9,
                      letterSpacing: 1.2,
                      fontWeight: FontWeight.bold)),
              const SizedBox(height: 3),
              FittedBox(
                fit: BoxFit.scaleDown,
                alignment: Alignment.centerLeft,
                child: Text(value,
                    style: TextStyle(
                        color: c,
                        fontSize: 13,
                        fontWeight: FontWeight.bold,
                        fontFamily: 'monospace')),
              ),
            ],
          ),
        ),
      );
}

class _NrfTool extends StatelessWidget {
  const _NrfTool({
    required this.icon,
    required this.color,
    required this.title,
    required this.subtitle,
    required this.running,
    required this.onTap,
  });

  final IconData icon;
  final Color color;
  final String title;
  final String subtitle;
  final bool running;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(14),
        child: Container(
          padding: const EdgeInsets.all(14),
          decoration: BoxDecoration(
            color: Colors.white.withValues(alpha: 0.03),
            borderRadius: BorderRadius.circular(14),
            border: Border.all(
                color: color.withValues(alpha: running ? 0.7 : 0.3)),
          ),
          child: Row(
            children: [
              Container(
                width: 44,
                height: 44,
                decoration: BoxDecoration(
                  color: color.withValues(alpha: 0.12),
                  borderRadius: BorderRadius.circular(12),
                ),
                child: Icon(icon, color: color, size: 24),
              ),
              const SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Text(title,
                            style: const TextStyle(
                                color: Colors.white,
                                fontSize: 15,
                                fontWeight: FontWeight.bold)),
                        const SizedBox(width: 8),
                        if (running)
                          Container(
                            padding: const EdgeInsets.symmetric(
                                horizontal: 6, vertical: 2),
                            decoration: BoxDecoration(
                              color: color.withValues(alpha: 0.18),
                              borderRadius: BorderRadius.circular(6),
                            ),
                            child: Text('LIVE',
                                style: TextStyle(
                                    color: color,
                                    fontSize: 8,
                                    fontWeight: FontWeight.bold,
                                    letterSpacing: 1)),
                          ),
                      ],
                    ),
                    const SizedBox(height: 2),
                    Text(subtitle,
                        style: const TextStyle(
                            color: Colors.white54, fontSize: 11)),
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
