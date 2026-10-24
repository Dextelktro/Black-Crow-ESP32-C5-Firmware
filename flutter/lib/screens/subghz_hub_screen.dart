import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/subghz_models.dart';
import '../services/device_controller.dart';
import '../widgets/subghz_kit.dart';
import 'subghz_analyzer_screen.dart';
import 'subghz_brute_screen.dart';
import 'subghz_radio_settings_screen.dart';
import 'subghz_read_raw_screen.dart';
import 'subghz_read_screen.dart';
import 'subghz_saved_screen.dart';
import 'subghz_spectrum_screen.dart';

/// Top-level Sub-GHz (CC1101) menu: Read, Read RAW, Saved, Analyzer, Spectrum
/// and Radio Settings, over a live tuning-dial hero.
class SubghzHubScreen extends StatefulWidget {
  const SubghzHubScreen({super.key});

  @override
  State<SubghzHubScreen> createState() => _SubghzHubScreenState();
}

class _SubghzHubScreenState extends State<SubghzHubScreen> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final ctrl = context.read<DeviceController>();
      ctrl.subghzRefreshStatus();
      ctrl.subghzRefreshStorage();
      ctrl.subghzRefreshBands();
      ctrl.subghzRefreshSaved();
    });
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final preset = SubghzPreset.byId(ctrl.subghzPresetId);
    return Scaffold(
      appBar: AppBar(title: const Text('Sub-GHz')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _Hero(ctrl: ctrl, preset: preset),
          const SizedBox(height: 18),
          _Menu(
            icon: Icons.graphic_eq,
            color: kSgAmber,
            title: 'Read',
            subtitle: 'Decode fixed-code protocols · CAME, Princeton, Nice…',
            running: ctrl.subghzReadRunning,
            onTap: () => pushSubghz(context, ctrl, const SubghzReadScreen()),
          ),
          const SizedBox(height: 12),
          _Menu(
            icon: Icons.waves,
            color: const Color(0xFFFF9E40),
            title: 'Read RAW',
            subtitle: 'Capture any OOK burst · replay & save',
            running: ctrl.subghzReadRawRunning,
            onTap: () => pushSubghz(context, ctrl, const SubghzReadRawScreen()),
          ),
          const SizedBox(height: 12),
          _Menu(
            icon: Icons.bookmarks,
            color: const Color(0xFF2EE6C5),
            title: 'Saved',
            subtitle: ctrl.subghzSaved.isEmpty
                ? 'Stored .sub library · view & emulate'
                : '${ctrl.subghzSaved.length} saved · tap to emulate',
            onTap: () => pushSubghz(context, ctrl, const SubghzSavedScreen()),
          ),
          const SizedBox(height: 12),
          _Menu(
            icon: Icons.align_vertical_bottom,
            color: const Color(0xFF9B8CFF),
            title: 'Frequency Analyzer',
            subtitle: 'Find the strongest frequency across all bands',
            running: ctrl.subghzAnalyzerRunning,
            onTap: () => pushSubghz(context, ctrl, const SubghzAnalyzerScreen()),
          ),
          const SizedBox(height: 12),
          _Menu(
            icon: Icons.blur_on,
            color: const Color(0xFF4FA8FF),
            title: 'Spectrum',
            subtitle: 'Swept-RSSI waterfall · zoom a band',
            running: ctrl.subghzSpectrumRunning,
            onTap: () => pushSubghz(context, ctrl, const SubghzSpectrumScreen()),
          ),
          const SizedBox(height: 12),
          _Menu(
            icon: Icons.tune,
            color: const Color(0xFFFFC53D),
            title: 'Radio Settings',
            subtitle:
                '${sgMhz(ctrl.subghzFreqHz)} MHz · ${preset.label} · storage ${_short(ctrl.subghzStorageTarget)}',
            onTap: () =>
                pushSubghz(context, ctrl, const SubghzRadioSettingsScreen()),
          ),
          const SizedBox(height: 18),
          const SgSectionLabel('ADVANCED'),
          _Menu(
            icon: Icons.password,
            color: kSgDanger,
            title: 'Brute Force',
            subtitle: 'Sweep a fixed-code space · authorised testing only',
            running: ctrl.subghzBruteRunning,
            onTap: () => pushSubghz(context, ctrl, const SubghzBruteScreen()),
          ),
          const SizedBox(height: 18),
          const Text(
            'CC1101 transceiver · OOK & 2-FSK · 300–348 / 387–464 / 779–928 MHz. '
            'Receive and transmit are shared with the IR radio — only one runs at '
            'a time. Use only on equipment you own or are authorised to test.',
            style: TextStyle(color: Colors.white38, fontSize: 11, height: 1.4),
          ),
        ],
      ),
    );
  }

  String _short(SubghzStorageTarget t) => switch (t) {
    SubghzStorageTarget.littlefs => 'flash',
    SubghzStorageTarget.sd => 'SD',
    SubghzStorageTarget.unset => 'unset',
  };
}

class _Hero extends StatelessWidget {
  const _Hero({required this.ctrl, required this.preset});
  final DeviceController ctrl;
  final SubghzPreset preset;

  @override
  Widget build(BuildContext context) {
    final known = ctrl.subghzModuleKnown;
    final present = ctrl.subghzPresent;
    final status = ctrl.subghzStatus;
    final fresh =
        status != null &&
        DateTime.now().difference(status.at) < const Duration(seconds: 6);
    final stateColor = !known
        ? kSgAmber
        : present
        ? kSgAmber
        : kSgDanger;
    final stateText = !known
        ? 'DETECTING…'
        : present
        ? 'CC1101 ONLINE'
        : 'CC1101 NOT DETECTED';

    return Container(
      padding: const EdgeInsets.all(20),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(20),
        gradient: const LinearGradient(
          colors: [kSgInk2, kSgInk],
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
        ),
        border: Border.all(color: stateColor.withValues(alpha: 0.4)),
        boxShadow: [
          BoxShadow(color: kSgAmber.withValues(alpha: 0.10), blurRadius: 22),
        ],
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Icon(Icons.settings_input_antenna, color: kSgAmber, size: 20),
              const SizedBox(width: 8),
              Text(
                stateText,
                style: TextStyle(
                  color: stateColor,
                  fontSize: 12,
                  fontWeight: FontWeight.bold,
                  letterSpacing: 1.4,
                ),
              ),
              const Spacer(),
              _Chip(
                text: preset.modulation.label,
                color: preset.modulation == SubghzModulation.fm
                    ? const Color(0xFF9B8CFF)
                    : kSgAmber,
              ),
            ],
          ),
          const SizedBox(height: 14),
          SgFreqReadout(
            hz: ctrl.subghzFreqHz,
            inBand: sgFreqInBand(ctrl.subghzFreqHz),
          ),
          const SizedBox(height: 4),
          Text(
            preset.label,
            style: const TextStyle(color: Colors.white54, fontSize: 12),
          ),
          const SizedBox(height: 14),
          Row(
            children: [
              Expanded(
                child: SgStat(
                  label: 'RSSI',
                  value: fresh ? '${status.rssi} dBm' : '-- dBm',
                  color: fresh ? sgRssiColor(status.rssi) : Colors.white38,
                ),
              ),
              Expanded(
                child: SgStat(
                  label: 'CHIP VER',
                  value: fresh
                      ? '0x${status.version.toRadixString(16).toUpperCase().padLeft(2, '0')}'
                      : '--',
                ),
              ),
              Expanded(
                child: SgStat(
                  label: 'ACTIVITY',
                  value: ctrl.subghzBusy ? 'RUNNING' : 'IDLE',
                  color: ctrl.subghzBusy ? kSgAmber : Colors.white54,
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _Chip extends StatelessWidget {
  const _Chip({required this.text, required this.color});
  final String text;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 4),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.15),
        borderRadius: BorderRadius.circular(20),
      ),
      child: Text(
        text,
        style: TextStyle(
          color: color,
          fontSize: 10,
          fontWeight: FontWeight.bold,
          letterSpacing: 1.2,
        ),
      ),
    );
  }
}

class _Menu extends StatelessWidget {
  const _Menu({
    required this.icon,
    required this.color,
    required this.title,
    required this.subtitle,
    required this.onTap,
    this.running = false,
  });

  final IconData icon;
  final Color color;
  final String title;
  final String subtitle;
  final VoidCallback onTap;
  final bool running;

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
              color: color.withValues(alpha: running ? 0.7 : 0.3),
            ),
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
                        Text(
                          title,
                          style: const TextStyle(
                            color: Colors.white,
                            fontSize: 15,
                            fontWeight: FontWeight.bold,
                          ),
                        ),
                        if (running) ...[
                          const SizedBox(width: 8),
                          Container(
                            padding: const EdgeInsets.symmetric(
                              horizontal: 6,
                              vertical: 2,
                            ),
                            decoration: BoxDecoration(
                              color: color.withValues(alpha: 0.18),
                              borderRadius: BorderRadius.circular(6),
                            ),
                            child: Text(
                              'LIVE',
                              style: TextStyle(
                                color: color,
                                fontSize: 8,
                                fontWeight: FontWeight.bold,
                                letterSpacing: 1,
                              ),
                            ),
                          ),
                        ],
                      ],
                    ),
                    const SizedBox(height: 2),
                    Text(
                      subtitle,
                      style: const TextStyle(color: Colors.white54, fontSize: 11),
                    ),
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
