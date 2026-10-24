import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/subghz_models.dart';
import '../services/device_controller.dart';
import '../widgets/subghz_kit.dart';
import 'subghz_common.dart';

/// Saved `.sub` library: view each file's decoded fields, emulate (transmit) or
/// delete it.
class SubghzSavedScreen extends StatefulWidget {
  const SubghzSavedScreen({super.key});

  @override
  State<SubghzSavedScreen> createState() => _SubghzSavedScreenState();
}

class _SubghzSavedScreenState extends State<SubghzSavedScreen> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final ctrl = context.read<DeviceController>();
      ctrl.subghzRefreshStorage();
      ctrl.subghzRefreshSaved();
    });
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final files = ctrl.subghzSaved;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Saved'),
        actions: [
          IconButton(
            tooltip: 'Refresh',
            icon: const Icon(Icons.refresh),
            onPressed: ctrl.subghzBusy ? null : ctrl.subghzRefreshSaved,
          ),
        ],
      ),
      body: Column(
        children: [
          SubghzNoticeBanner(ctrl: ctrl),
          Expanded(
            child: ListView(
              padding: const EdgeInsets.all(16),
              children: [
                _StorageHeader(ctrl: ctrl),
                const SizedBox(height: 14),
                if (ctrl.subghzTxRunning)
                  const Padding(
                    padding: EdgeInsets.only(bottom: 12),
                    child: _Transmitting(),
                  ),
                if (files.isEmpty)
                  _Empty(listing: ctrl.subghzListing)
                else
                  for (final f in files)
                    _FileRow(
                      file: f,
                      onTap: () => _openFile(ctrl, f),
                    ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Future<void> _openFile(DeviceController ctrl, SubghzSavedFile file) async {
    ctrl.subghzInfo(file.name);
    await showModalBottomSheet<void>(
      context: context,
      backgroundColor: kSgInk2,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(20)),
      ),
      builder: (_) => ChangeNotifierProvider.value(
        value: ctrl,
        child: _FileSheet(file: file),
      ),
    );
  }
}

class _StorageHeader extends StatelessWidget {
  const _StorageHeader({required this.ctrl});
  final DeviceController ctrl;

  @override
  Widget build(BuildContext context) {
    final target = ctrl.subghzStorageTarget;
    final unset = target == SubghzStorageTarget.unset;
    return SgPanel(
      accent: unset ? kSgDanger : kSgAmber,
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
      child: Row(
        children: [
          Icon(
            unset ? Icons.error_outline : Icons.folder_special,
            color: unset ? kSgDanger : kSgAmber,
            size: 20,
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  unset ? 'No save location chosen' : target.label,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 13,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                Text(
                  unset
                      ? 'Pick one in Radio Settings to save & list files'
                      : '${ctrl.subghzSaved.length} file(s)',
                  style: const TextStyle(color: Colors.white38, fontSize: 11),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _Transmitting extends StatelessWidget {
  const _Transmitting();

  @override
  Widget build(BuildContext context) {
    return SgPanel(
      child: Row(
        children: const [
          SizedBox(
            width: 18,
            height: 18,
            child: CircularProgressIndicator(
              strokeWidth: 2.4,
              valueColor: AlwaysStoppedAnimation(kSgAmber),
            ),
          ),
          SizedBox(width: 12),
          Text(
            'Transmitting…',
            style: TextStyle(color: kSgAmber, fontWeight: FontWeight.bold),
          ),
        ],
      ),
    );
  }
}

class _Empty extends StatelessWidget {
  const _Empty({required this.listing});
  final bool listing;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 50),
      alignment: Alignment.center,
      child: Column(
        children: [
          if (listing)
            const CircularProgressIndicator(
              valueColor: AlwaysStoppedAnimation(kSgAmber),
            )
          else ...[
            const Icon(Icons.inbox, color: Colors.white24, size: 42),
            const SizedBox(height: 12),
            const Text(
              'No saved signals yet.\nCapture one in Read or Read RAW, then save.',
              textAlign: TextAlign.center,
              style: TextStyle(color: Colors.white38, fontSize: 13, height: 1.5),
            ),
          ],
        ],
      ),
    );
  }
}

class _FileRow extends StatelessWidget {
  const _FileRow({required this.file, required this.onTap});
  final SubghzSavedFile file;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.only(bottom: 8),
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          onTap: onTap,
          borderRadius: BorderRadius.circular(12),
          child: Container(
            padding: const EdgeInsets.all(14),
            decoration: BoxDecoration(
              color: Colors.white.withValues(alpha: 0.03),
              borderRadius: BorderRadius.circular(12),
              border: Border.all(color: kSgAmber.withValues(alpha: 0.25)),
            ),
            child: Row(
              children: [
                const Icon(Icons.description, color: kSgAmber, size: 22),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        file.name,
                        style: const TextStyle(
                          color: Colors.white,
                          fontSize: 14,
                          fontWeight: FontWeight.w600,
                        ),
                      ),
                      Text(
                        '${file.bytes} bytes',
                        style: const TextStyle(
                          color: Colors.white38,
                          fontSize: 11,
                          fontFamily: 'monospace',
                        ),
                      ),
                    ],
                  ),
                ),
                const Icon(Icons.chevron_right, color: Colors.white38),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _FileSheet extends StatefulWidget {
  const _FileSheet({required this.file});
  final SubghzSavedFile file;

  @override
  State<_FileSheet> createState() => _FileSheetState();
}

class _FileSheetState extends State<_FileSheet> {
  int _repeats = 3;

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final info = ctrl.subghzLastInfo?.name == widget.file.name
        ? ctrl.subghzLastInfo
        : null;

    return Padding(
      padding: EdgeInsets.only(
        left: 18,
        right: 18,
        top: 18,
        bottom: 18 + MediaQuery.of(context).padding.bottom,
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Icon(Icons.description, color: kSgAmber),
              const SizedBox(width: 10),
              Expanded(
                child: Text(
                  widget.file.name,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 17,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          if (info == null)
            const Padding(
              padding: EdgeInsets.symmetric(vertical: 14),
              child: Row(
                children: [
                  SizedBox(
                    width: 16,
                    height: 16,
                    child: CircularProgressIndicator(
                      strokeWidth: 2,
                      valueColor: AlwaysStoppedAnimation(kSgAmber),
                    ),
                  ),
                  SizedBox(width: 10),
                  Text('Reading file…',
                      style: TextStyle(color: Colors.white54)),
                ],
              ),
            )
          else
            _InfoBody(info: info),
          const SizedBox(height: 18),
          Row(
            children: [
              const Text('Repeats',
                  style: TextStyle(color: Colors.white70, fontSize: 13)),
              const Spacer(),
              _Stepper(
                value: _repeats,
                min: 1,
                max: 20,
                onChanged: (v) => setState(() => _repeats = v),
              ),
            ],
          ),
          const SizedBox(height: 14),
          SgButton(
            label: 'EMULATE (TRANSMIT)',
            icon: Icons.wifi_tethering,
            busy: ctrl.subghzTxRunning,
            onPressed: ctrl.subghzBusy && !ctrl.subghzTxRunning
                ? null
                : () async {
                    await ctrl.subghzEmulate(widget.file.name,
                        repeats: _repeats);
                    if (context.mounted) Navigator.of(context).pop();
                  },
          ),
          const SizedBox(height: 10),
          SgButton(
            label: 'DELETE',
            icon: Icons.delete_outline,
            danger: true,
            filled: false,
            onPressed: () async {
              final ok = await _confirmDelete(context, widget.file.name);
              if (ok && context.mounted) {
                await ctrl.subghzDelete(widget.file.name);
                if (context.mounted) Navigator.of(context).pop();
              }
            },
          ),
        ],
      ),
    );
  }

  Future<bool> _confirmDelete(BuildContext context, String name) async {
    final r = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: const Text('Delete file?'),
        content: Text('"$name" will be permanently removed from the device.'),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(c).pop(false),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () => Navigator.of(c).pop(true),
            child: const Text('Delete',
                style: TextStyle(color: kSgDanger)),
          ),
        ],
      ),
    );
    return r ?? false;
  }
}

class _InfoBody extends StatelessWidget {
  const _InfoBody({required this.info});
  final SubghzFileInfo info;

  @override
  Widget build(BuildContext context) {
    if (!info.readable) {
      return const Text('File is unreadable or malformed.',
          style: TextStyle(color: kSgDanger));
    }
    return SgPanel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                decoration: BoxDecoration(
                  color: kSgAmber.withValues(alpha: 0.15),
                  borderRadius: BorderRadius.circular(6),
                ),
                child: Text(
                  info.isKey ? 'PROTOCOL KEY' : 'RAW',
                  style: const TextStyle(
                    color: kSgAmber,
                    fontSize: 10,
                    fontWeight: FontWeight.bold,
                    letterSpacing: 1,
                  ),
                ),
              ),
              const Spacer(),
              if (info.freqHz > 0)
                Text(
                  '${sgMhz(info.freqHz)} MHz',
                  style: const TextStyle(
                    color: kSgAmber,
                    fontSize: 13,
                    fontFamily: 'monospace',
                    fontWeight: FontWeight.bold,
                  ),
                ),
            ],
          ),
          const SizedBox(height: 12),
          if (info.isKey) ...[
            _kv('Protocol', info.proto),
            _kv('Bits', '${info.bits}'),
            if (info.keyHex.isNotEmpty) _kv('Key', info.keyHex, mono: true),
            if (info.te > 0) _kv('Te', '${info.te} µs'),
          ] else
            _kv('Pulses', '${info.count}'),
          if (info.preset.isNotEmpty) _kv('Preset', _presetShort(info.preset)),
        ],
      ),
    );
  }

  String _presetShort(String flipperName) {
    for (final p in SubghzPreset.all) {
      if (flipperName.contains('Ook270') && p.id == 'ook270') return p.label;
      if (flipperName.contains('Ook650') && p.id == 'ook650') return p.label;
      if (flipperName.contains('2FSKDev238') && p.id == '2fsk238') return p.label;
      if (flipperName.contains('2FSKDev476') && p.id == '2fsk476') return p.label;
    }
    return flipperName;
  }

  Widget _kv(String k, String v, {bool mono = false}) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 6),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(
            width: 76,
            child: Text(k,
                style: const TextStyle(color: Colors.white38, fontSize: 12)),
          ),
          Expanded(
            child: Text(
              v,
              style: TextStyle(
                color: Colors.white,
                fontSize: 13,
                fontFamily: mono ? 'monospace' : null,
                fontWeight: mono ? FontWeight.bold : FontWeight.w500,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _Stepper extends StatelessWidget {
  const _Stepper({
    required this.value,
    required this.min,
    required this.max,
    required this.onChanged,
  });
  final int value;
  final int min;
  final int max;
  final ValueChanged<int> onChanged;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        _btn(Icons.remove, value > min ? () => onChanged(value - 1) : null),
        Container(
          width: 40,
          alignment: Alignment.center,
          child: Text(
            '$value',
            style: const TextStyle(
              color: Colors.white,
              fontSize: 16,
              fontWeight: FontWeight.bold,
              fontFamily: 'monospace',
            ),
          ),
        ),
        _btn(Icons.add, value < max ? () => onChanged(value + 1) : null),
      ],
    );
  }

  Widget _btn(IconData icon, VoidCallback? onTap) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(8),
        child: Container(
          width: 34,
          height: 34,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(8),
            border: Border.all(
              color: kSgAmber.withValues(alpha: onTap == null ? 0.2 : 0.5),
            ),
          ),
          child: Icon(icon,
              size: 18,
              color: onTap == null ? Colors.white24 : kSgAmber),
        ),
      ),
    );
  }
}
