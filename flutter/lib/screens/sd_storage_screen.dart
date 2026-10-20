import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../services/download_store.dart';

const _sdAccent = Color(0xFF4FA8FF);

String _formatSdBytes(int bytes) {
  if (bytes >= 1024 * 1024 * 1024) {
    return '${(bytes / (1024 * 1024 * 1024)).toStringAsFixed(1)} GB';
  }
  if (bytes >= 1024 * 1024) {
    return '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MB';
  }
  if (bytes >= 1024) return '${(bytes / 1024).toStringAsFixed(1)} KB';
  return '$bytes B';
}

class SdStorageScreen extends StatefulWidget {
  const SdStorageScreen({super.key});

  @override
  State<SdStorageScreen> createState() => _SdStorageScreenState();
}

class _SdActivityIcon extends StatefulWidget {
  const _SdActivityIcon({
    required this.active,
    required this.icon,
    required this.color,
  });

  final bool active;
  final IconData icon;
  final Color color;

  @override
  State<_SdActivityIcon> createState() => _SdActivityIconState();
}

class _SdActivityIconState extends State<_SdActivityIcon>
    with SingleTickerProviderStateMixin {
  late final AnimationController _controller = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 900),
  );

  @override
  void initState() {
    super.initState();
    if (widget.active) _controller.repeat();
  }

  @override
  void didUpdateWidget(covariant _SdActivityIcon oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.active == oldWidget.active) return;
    if (widget.active) {
      _controller.repeat();
    } else {
      _controller
        ..stop()
        ..value = 0;
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => RotationTransition(
    turns: _controller,
    child: Icon(widget.icon, color: widget.color, size: 20),
  );
}

class _SdStorageScreenState extends State<SdStorageScreen> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _run(context.read<DeviceController>().sdRefresh());
    });
  }

  Future<void> _run(Future<void> operation) async {
    try {
      await operation;
    } catch (error) {
      if (!mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Operation failed: $error')));
    }
  }

  @override
  Widget build(BuildContext context) {
    final controller = context.watch<DeviceController>();
    return Scaffold(
      appBar: AppBar(
        title: const Text('SD Card'),
        actions: [
          IconButton(
            tooltip: 'Refresh',
            icon: const Icon(Icons.refresh),
            onPressed: controller.sdBusy
                ? null
                : () => _run(controller.sdRefresh()),
          ),
          if (controller.sdMounted)
            IconButton(
              tooltip: 'Safely eject',
              icon: const Icon(Icons.eject),
              onPressed: controller.sdBusy
                  ? null
                  : () => _confirmUnmount(controller),
            ),
          if (controller.sdPresent)
            IconButton(
              tooltip: 'Format card',
              icon: const Icon(Icons.delete_sweep_outlined),
              onPressed: controller.sdBusy
                  ? null
                  : () => _confirmFormat(controller),
            ),
          if (!controller.sdMounted && controller.sdPresent)
            IconButton(
              tooltip: 'Mount card',
              icon: const Icon(Icons.sd_storage),
              onPressed: controller.sdBusy
                  ? null
                  : () => _run(controller.sdMount()),
            ),
        ],
      ),
      body: Column(
        children: [
          _usageCard(controller),
          if (controller.sdBusy || controller.sdListing)
            const LinearProgressIndicator(minHeight: 2),
          Expanded(
            child: !controller.sdPresent
                ? _empty(
                    Icons.help_outline,
                    'No SD card detected',
                    'Check the card and the active-low CD connection.',
                  )
                : !controller.sdMounted
                ? _empty(
                    Icons.sd_card_alert_outlined,
                    'Card is not mounted',
                    'Mount failed or the card was safely ejected. Check its format, then retry.',
                  )
                : controller.sdFiles.isEmpty
                ? _empty(
                    Icons.folder_open_outlined,
                    controller.sdListing ? 'Reading card…' : 'No files found',
                    'Files on the mounted SD card appear here.',
                  )
                : ListView.separated(
                    padding: const EdgeInsets.all(14),
                    itemCount: controller.sdFiles.length,
                    separatorBuilder: (_, _) => const SizedBox(height: 9),
                    itemBuilder: (_, index) =>
                        _fileTile(controller, controller.sdFiles[index]),
                  ),
          ),
        ],
      ),
    );
  }

  Widget _usageCard(DeviceController controller) {
    final usedFraction = controller.sdTotal > 0
        ? (controller.sdUsed / controller.sdTotal).clamp(0.0, 1.0)
        : 0.0;
    final icon = controller.sdBusy
        ? Icons.sync
        : controller.sdPresent
        ? Icons.sd_storage
        : Icons.help_outline;
    return Container(
      margin: const EdgeInsets.fromLTRB(14, 14, 14, 10),
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: const Color(0xFF07201C),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: _sdAccent.withValues(alpha: 0.3)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              _SdActivityIcon(
                active: controller.sdBusy,
                icon: icon,
                color: controller.sdPresent ? _sdAccent : Colors.amber,
              ),
              const SizedBox(width: 8),
              Expanded(
                child: Text(
                  controller.sdBusy
                      ? 'SD card transaction in progress'
                      : controller.sdMounted
                      ? 'Card detected and mounted'
                      : controller.sdPresent
                      ? 'Card detected, not mounted'
                      : 'No card detected',
                  style: const TextStyle(color: Colors.white, fontSize: 13),
                ),
              ),
              Text(
                controller.sdMounted
                    ? '${_formatSdBytes(controller.sdFree)} free'
                    : '—',
                style: const TextStyle(
                  color: Colors.white70,
                  fontSize: 12,
                  fontFamily: 'monospace',
                ),
              ),
            ],
          ),
          if (controller.sdMounted) ...[
            const SizedBox(height: 10),
            Text(
              '${controller.sdFiles.length} file(s) · '
              '${_formatSdBytes(controller.sdUsed)} used / '
              '${_formatSdBytes(controller.sdTotal)} total',
              style: const TextStyle(color: Colors.white54, fontSize: 12),
            ),
            const SizedBox(height: 7),
            ClipRRect(
              borderRadius: BorderRadius.circular(3),
              child: LinearProgressIndicator(
                value: usedFraction,
                minHeight: 6,
                backgroundColor: Colors.white.withValues(alpha: 0.08),
                valueColor: const AlwaysStoppedAnimation<Color>(_sdAccent),
              ),
            ),
          ],
        ],
      ),
    );
  }

  Widget _empty(IconData icon, String title, String detail) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(28),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(icon, size: 48, color: Colors.white24),
            const SizedBox(height: 12),
            Text(
              title,
              style: const TextStyle(color: Colors.white70, fontSize: 15),
            ),
            const SizedBox(height: 6),
            Text(
              detail,
              textAlign: TextAlign.center,
              style: const TextStyle(color: Colors.white38, fontSize: 12),
            ),
          ],
        ),
      ),
    );
  }

  Widget _fileTile(DeviceController controller, SdFile file) {
    return ListTile(
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: BorderSide(color: _sdAccent.withValues(alpha: 0.25)),
      ),
      tileColor: Colors.white.withValues(alpha: 0.03),
      leading: const Icon(Icons.insert_drive_file_outlined, color: _sdAccent),
      title: Text(
        file.path,
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
        style: const TextStyle(color: Colors.white, fontSize: 14),
      ),
      subtitle: Text(
        _formatSdBytes(file.size),
        style: const TextStyle(
          color: Colors.white54,
          fontSize: 11,
          fontFamily: 'monospace',
        ),
      ),
      trailing: IconButton(
        tooltip: 'File actions',
        icon: const Icon(Icons.more_vert, color: Colors.white70),
        onPressed: controller.sdBusy
            ? null
            : () => _showFileActions(controller, file),
      ),
      onTap: controller.sdBusy
          ? null
          : () => _showFileActions(controller, file),
    );
  }

  Future<void> _showFileActions(
    DeviceController controller,
    SdFile file,
  ) async {
    final action = await showModalBottomSheet<String>(
      context: context,
      builder: (context) => SafeArea(
        child: Wrap(
          children: [
            ListTile(
              leading: const Icon(Icons.visibility_outlined),
              title: const Text('View preview'),
              subtitle: Text('${_formatSdBytes(file.size)} · max 2 KB preview'),
              onTap: () => Navigator.pop(context, 'view'),
            ),
            ListTile(
              leading: const Icon(Icons.download_outlined),
              title: const Text('Download to Downloads'),
              onTap: () => Navigator.pop(context, 'download'),
            ),
            ListTile(
              leading: const Icon(Icons.copy_outlined),
              title: const Text('Copy to LittleFS'),
              onTap: () => Navigator.pop(context, 'copy'),
            ),
          ],
        ),
      ),
    );
    if (!mounted) return;
    if (action == 'view') {
      await _viewFile(controller, file);
    } else if (action == 'download') {
      await _downloadFile(controller, file);
    } else if (action == 'copy') {
      await _run(controller.sdCopyToLittleFs(file.path));
    }
  }

  Future<void> _viewFile(DeviceController controller, SdFile file) async {
    try {
      final bytes = await controller.sdPreview(file.path);
      if (!mounted) return;
      final text = utf8.decode(bytes, allowMalformed: true);
      final isText =
          !text.contains('\u0000') &&
          text.runes
              .where((r) => r < 0x20 && r != 0x09 && r != 0x0a && r != 0x0d)
              .isEmpty;
      final preview = isText
          ? text
          : bytes
                .map((byte) => byte.toRadixString(16).padLeft(2, '0'))
                .join(' ');
      await showDialog<void>(
        context: context,
        builder: (context) => AlertDialog(
          title: Text(file.path, maxLines: 1, overflow: TextOverflow.ellipsis),
          content: SizedBox(
            width: 560,
            child: SingleChildScrollView(
              child: SelectableText(
                preview.isEmpty ? '(empty file)' : preview,
                style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
              ),
            ),
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(context),
              child: const Text('Close'),
            ),
          ],
        ),
      );
    } catch (error) {
      if (!mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Preview failed: $error')));
    }
  }

  Future<void> _downloadFile(DeviceController controller, SdFile file) async {
    DownloadStore? store;
    var finished = false;
    try {
      store = await DownloadStore.create(file.path);
      final transfer = await controller.sdDownload(file.path, store.write);
      if (transfer.transferredBytes != file.size || transfer.truncated) {
        throw StateError(
          'Incomplete download: ${transfer.transferredBytes}/${file.size} bytes.',
        );
      }
      final location = await store.finish();
      finished = true;
      if (!mounted) return;
      await showDialog<void>(
        context: context,
        builder: (context) => AlertDialog(
          title: const Text('Download complete'),
          content: SelectableText(
            '${store!.fileName}\n\nSaved to:\n$location\n\n'
            '${_formatSdBytes(transfer.transferredBytes)}',
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(context),
              child: const Text('OK'),
            ),
          ],
        ),
      );
    } catch (error) {
      Object reportedError = error;
      if (store != null && !finished) {
        try {
          await store.abort();
        } catch (cleanupError) {
          reportedError = StateError(
            '$error; failed to remove the partial download: $cleanupError',
          );
        }
      }
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Download failed: $reportedError')),
      );
    }
  }

  Future<void> _confirmUnmount(DeviceController controller) async {
    final accepted = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Safely eject SD card?'),
        content: const Text(
          'The firmware will unmount the filesystem before the card is removed. '
          'Do not pull the card until the operation completes.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: const Text('Cancel'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(context, true),
            child: const Text('Unmount'),
          ),
        ],
      ),
    );
    if (accepted == true) await _run(controller.sdUnmount());
  }

  Future<void> _confirmFormat(DeviceController controller) async {
    final input = TextEditingController();
    final accepted = await showDialog<bool>(
      context: context,
      builder: (context) => StatefulBuilder(
        builder: (context, setDialogState) => AlertDialog(
          title: const Text('Format SD card?'),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text(
                'Formatting permanently erases every file on the card.',
              ),
              const SizedBox(height: 12),
              TextField(
                controller: input,
                autofocus: true,
                decoration: const InputDecoration(
                  labelText: 'Type FORMAT to confirm',
                ),
                onChanged: (_) => setDialogState(() {}),
              ),
            ],
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(context, false),
              child: const Text('Cancel'),
            ),
            FilledButton(
              onPressed: input.text == 'FORMAT'
                  ? () => Navigator.pop(context, true)
                  : null,
              child: const Text('Erase and format'),
            ),
          ],
        ),
      ),
    );
    input.dispose();
    if (accepted == true) await _run(controller.sdFormat());
  }
}
