import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../models/subghz_models.dart';
import '../services/device_controller.dart';
import '../widgets/subghz_kit.dart';

/// Radio Settings: frequency tuner, modulation preset, RSSI squelch and the
/// manual storage target for saved `.sub` files.
class SubghzRadioSettingsScreen extends StatefulWidget {
  const SubghzRadioSettingsScreen({super.key});

  @override
  State<SubghzRadioSettingsScreen> createState() =>
      _SubghzRadioSettingsScreenState();
}

class _SubghzRadioSettingsScreenState
    extends State<SubghzRadioSettingsScreen> {
  int _hz = 433920000;
  bool _seeded = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final ctrl = context.read<DeviceController>();
      ctrl.subghzRefreshStatus();
      ctrl.subghzRefreshStorage();
    });
  }

  void _commit(DeviceController ctrl, int hz) {
    final clamped = hz.clamp(kSgMinHz, kSgMaxHz);
    setState(() => _hz = clamped);
    ctrl.subghzSetFrequency(clamped);
  }

  Future<void> _typeFrequency(DeviceController ctrl) async {
    final controller = TextEditingController(text: sgMhz(_hz, decimals: 3));
    final mhz = await showDialog<double>(
      context: context,
      builder: (dialogContext) => AlertDialog(
        title: const Text('Set frequency (MHz)'),
        content: TextField(
          controller: controller,
          autofocus: true,
          keyboardType: const TextInputType.numberWithOptions(decimal: true),
          inputFormatters: [
            FilteringTextInputFormatter.allow(RegExp(r'[0-9.]')),
          ],
          decoration: const InputDecoration(
            suffixText: 'MHz',
            hintText: 'e.g. 433.92',
          ),
          onSubmitted: (v) =>
              Navigator.of(dialogContext).pop(double.tryParse(v)),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(dialogContext).pop(),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () => Navigator.of(dialogContext)
                .pop(double.tryParse(controller.text)),
            child: const Text('Set'),
          ),
        ],
      ),
    );
    if (mhz != null && mhz.isFinite) {
      _commit(ctrl, (mhz * 1e6).round());
    }
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    if (!_seeded) {
      _hz = ctrl.subghzFreqHz;
      _seeded = true;
    }
    final preset = SubghzPreset.byId(ctrl.subghzPresetId);
    final inBand = sgFreqInBand(_hz);

    return Scaffold(
      appBar: AppBar(title: const Text('Radio Settings')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          // ── Frequency ──
          SgPanel(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    SgFreqReadout(hz: _hz, inBand: inBand),
                    const Spacer(),
                    IconButton(
                      onPressed: () => _typeFrequency(ctrl),
                      icon: const Icon(Icons.keyboard, color: kSgAmber),
                      tooltip: 'Type exact frequency',
                    ),
                  ],
                ),
                if (!inBand)
                  const Padding(
                    padding: EdgeInsets.only(bottom: 6),
                    child: Text(
                      'Outside the CC1101 bands — transmit/receive will be refused.',
                      style: TextStyle(color: kSgDanger, fontSize: 11),
                    ),
                  ),
                const SizedBox(height: 10),
                FrequencyTuner(
                  hz: _hz,
                  onChanged: (v) => setState(() => _hz = v),
                  onCommit: (v) => _commit(ctrl, v),
                ),
                const SizedBox(height: 12),
                Row(
                  children: [
                    _StepButton(
                      label: '−1 MHz',
                      onTap: () => _commit(ctrl, _hz - 1000000),
                    ),
                    const SizedBox(width: 6),
                    _StepButton(
                      label: '−10 kHz',
                      onTap: () => _commit(ctrl, _hz - 10000),
                    ),
                    const SizedBox(width: 6),
                    _StepButton(
                      label: '+10 kHz',
                      onTap: () => _commit(ctrl, _hz + 10000),
                    ),
                    const SizedBox(width: 6),
                    _StepButton(
                      label: '+1 MHz',
                      onTap: () => _commit(ctrl, _hz + 1000000),
                    ),
                  ],
                ),
                const SizedBox(height: 12),
                Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  children: [
                    for (final spot in kSgSpots)
                      _SpotChip(
                        label: '${spot.$1} MHz',
                        active: (_hz - spot.$2).abs() < 5000,
                        onTap: () => _commit(ctrl, spot.$2),
                      ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 14),

          // ── Modulation preset ──
          const SgSectionLabel('MODULATION PRESET'),
          SgPanel(
            child: Column(
              children: [
                for (final p in SubghzPreset.all)
                  _PresetRow(
                    preset: p,
                    selected: p.id == preset.id,
                    onTap: () => ctrl.subghzSetPreset(p.id),
                  ),
              ],
            ),
          ),
          const SizedBox(height: 14),

          // ── RSSI squelch ──
          const SgSectionLabel('RSSI SQUELCH'),
          _SquelchPanel(ctrl: ctrl),
          const SizedBox(height: 14),

          // ── Storage target ──
          const SgSectionLabel('SAVE LOCATION'),
          _StoragePanel(ctrl: ctrl),
          const SizedBox(height: 10),
          const Text(
            'Storage is manual: saving is refused until you pick a target, and '
            'choosing SD when no card is present is an explicit error (no silent '
            'fallback).',
            style: TextStyle(color: Colors.white38, fontSize: 11, height: 1.4),
          ),
        ],
      ),
    );
  }
}

class _StepButton extends StatelessWidget {
  const _StepButton({required this.label, required this.onTap});
  final String label;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Expanded(
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          onTap: onTap,
          borderRadius: BorderRadius.circular(10),
          child: Container(
            height: 38,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(10),
              border: Border.all(color: kSgAmber.withValues(alpha: 0.4)),
            ),
            child: Text(
              label,
              style: const TextStyle(
                color: kSgAmber,
                fontSize: 11,
                fontWeight: FontWeight.bold,
                fontFamily: 'monospace',
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _SpotChip extends StatelessWidget {
  const _SpotChip({
    required this.label,
    required this.active,
    required this.onTap,
  });
  final String label;
  final bool active;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(20),
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 7),
          decoration: BoxDecoration(
            color: active ? kSgAmber.withValues(alpha: 0.18) : Colors.transparent,
            borderRadius: BorderRadius.circular(20),
            border: Border.all(
              color: kSgAmber.withValues(alpha: active ? 0.8 : 0.3),
            ),
          ),
          child: Text(
            label,
            style: TextStyle(
              color: active ? kSgAmber : Colors.white70,
              fontSize: 12,
              fontWeight: FontWeight.bold,
              fontFamily: 'monospace',
            ),
          ),
        ),
      ),
    );
  }
}

class _PresetRow extends StatelessWidget {
  const _PresetRow({
    required this.preset,
    required this.selected,
    required this.onTap,
  });
  final SubghzPreset preset;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final modColor = preset.modulation == SubghzModulation.fm
        ? const Color(0xFF9B8CFF)
        : kSgAmber;
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(10),
        child: Padding(
          padding: const EdgeInsets.symmetric(vertical: 10, horizontal: 4),
          child: Row(
            children: [
              Icon(
                selected ? Icons.radio_button_checked : Icons.radio_button_off,
                color: selected ? kSgAmber : Colors.white30,
                size: 20,
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Text(
                  preset.label,
                  style: TextStyle(
                    color: selected ? Colors.white : Colors.white70,
                    fontSize: 14,
                    fontWeight: selected ? FontWeight.bold : FontWeight.normal,
                  ),
                ),
              ),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                decoration: BoxDecoration(
                  color: modColor.withValues(alpha: 0.15),
                  borderRadius: BorderRadius.circular(6),
                ),
                child: Text(
                  preset.modulation.label,
                  style: TextStyle(
                    color: modColor,
                    fontSize: 10,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _SquelchPanel extends StatefulWidget {
  const _SquelchPanel({required this.ctrl});
  final DeviceController ctrl;

  @override
  State<_SquelchPanel> createState() => _SquelchPanelState();
}

class _SquelchPanelState extends State<_SquelchPanel> {
  double? _drag;

  @override
  Widget build(BuildContext context) {
    final thr = widget.ctrl.subghzRssiThreshold;
    final off = thr <= -127;
    final value = _drag ?? (off ? -100 : thr.toDouble().clamp(-110, -40));
    return SgPanel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Text(
                'Threshold',
                style: TextStyle(color: Colors.white70, fontSize: 13),
              ),
              const Spacer(),
              Text(
                off ? 'SQUELCH OFF' : '${value.round()} dBm',
                style: TextStyle(
                  color: off ? Colors.white38 : kSgAmber,
                  fontSize: 14,
                  fontWeight: FontWeight.w800,
                  fontFamily: 'monospace',
                ),
              ),
            ],
          ),
          Opacity(
            opacity: off ? 0.4 : 1,
            child: Slider(
              value: value.clamp(-110, -40),
              min: -110,
              max: -40,
              activeColor: kSgAmber,
              onChanged: off
                  ? null
                  : (v) => setState(() => _drag = v),
              onChangeEnd: off
                  ? null
                  : (v) {
                      setState(() => _drag = null);
                      widget.ctrl.subghzSetRssiThreshold(v.round());
                    },
            ),
          ),
          SwitchListTile(
            contentPadding: EdgeInsets.zero,
            dense: true,
            activeThumbColor: kSgAmber,
            title: const Text(
              'Disable squelch (capture everything)',
              style: TextStyle(color: Colors.white70, fontSize: 13),
            ),
            subtitle: const Text(
              'Streams all bursts incl. noise — useful on a quiet bench',
              style: TextStyle(color: Colors.white38, fontSize: 11),
            ),
            value: off,
            onChanged: (v) =>
                widget.ctrl.subghzSetRssiThreshold(v ? -128 : -90),
          ),
        ],
      ),
    );
  }
}

class _StoragePanel extends StatelessWidget {
  const _StoragePanel({required this.ctrl});
  final DeviceController ctrl;

  @override
  Widget build(BuildContext context) {
    final target = ctrl.subghzStorageTarget;
    return SgPanel(
      child: Column(
        children: [
          _StorageRow(
            icon: Icons.sd_card,
            title: 'Internal flash (LittleFS)',
            subtitle: 'Always available',
            selected: target == SubghzStorageTarget.littlefs,
            onTap: () => ctrl.subghzSetStorage(SubghzStorageTarget.littlefs),
          ),
          const Divider(height: 18, color: Colors.white10),
          _StorageRow(
            icon: Icons.sd_storage,
            title: 'SD card',
            subtitle: ctrl.sdPresent
                ? 'Card detected'
                : 'No card detected — selecting it errors on save',
            selected: target == SubghzStorageTarget.sd,
            onTap: () => ctrl.subghzSetStorage(SubghzStorageTarget.sd),
          ),
        ],
      ),
    );
  }
}

class _StorageRow extends StatelessWidget {
  const _StorageRow({
    required this.icon,
    required this.title,
    required this.subtitle,
    required this.selected,
    required this.onTap,
  });
  final IconData icon;
  final String title;
  final String subtitle;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(10),
        child: Row(
          children: [
            Icon(icon, color: selected ? kSgAmber : Colors.white38, size: 22),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    title,
                    style: TextStyle(
                      color: selected ? Colors.white : Colors.white70,
                      fontSize: 14,
                      fontWeight: selected ? FontWeight.bold : FontWeight.normal,
                    ),
                  ),
                  Text(
                    subtitle,
                    style: const TextStyle(color: Colors.white38, fontSize: 11),
                  ),
                ],
              ),
            ),
            Icon(
              selected ? Icons.check_circle : Icons.circle_outlined,
              color: selected ? kSgAmber : Colors.white24,
              size: 20,
            ),
          ],
        ),
      ),
    );
  }
}
