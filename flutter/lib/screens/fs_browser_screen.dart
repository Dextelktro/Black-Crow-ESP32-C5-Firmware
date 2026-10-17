import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';

const _kAccent = Color(0xFF4FA8FF);

String _fmtBytes(int bytes) {
  if (bytes >= 1024 * 1024) {
    return '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MB';
  }
  if (bytes >= 1024) return '${(bytes / 1024).toStringAsFixed(1)} KB';
  return '$bytes B';
}

/// LittleFS file manager — browse and delete files stored on the device
/// (captured IR signals, Evil Portal data, …).
class FsBrowserScreen extends StatefulWidget {
  const FsBrowserScreen({super.key});

  @override
  State<FsBrowserScreen> createState() => _FsBrowserScreenState();
}

class _FsBrowserScreenState extends State<FsBrowserScreen> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      context.read<DeviceController>().fsRefresh();
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
    final files = ctrl.fsFiles;
    return Scaffold(
      appBar: AppBar(
        title: const Text('LittleFS Files'),
        actions: [
          IconButton(
            tooltip: 'Refresh',
            icon: const Icon(Icons.refresh),
            onPressed: () => _guard(ctrl.fsRefresh()),
          ),
        ],
      ),
      body: Column(
        children: [
          _usageHeader(ctrl),
          Expanded(
            child: files.isEmpty
                ? _empty(ctrl)
                : ListView.separated(
                    padding: const EdgeInsets.all(14),
                    itemCount: files.length,
                    separatorBuilder: (_, _) => const SizedBox(height: 10),
                    itemBuilder: (_, i) => _tile(ctrl, files[i]),
                  ),
          ),
        ],
      ),
    );
  }

  Widget _usageHeader(DeviceController ctrl) {
    final total = ctrl.fsTotal;
    final used = ctrl.fsUsed;
    final frac = total > 0 ? (used / total).clamp(0.0, 1.0) : 0.0;
    return Container(
      margin: const EdgeInsets.fromLTRB(14, 14, 14, 0),
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: const Color(0xFF07201C),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: _kAccent.withValues(alpha: 0.3)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Icon(Icons.sd_storage, color: _kAccent, size: 18),
              const SizedBox(width: 8),
              Text(
                '${ctrl.fsFiles.length} file(s)',
                style: const TextStyle(color: Colors.white, fontSize: 13),
              ),
              const Spacer(),
              Text(
                total > 0
                    ? '${_fmtBytes(used)} used · ${_fmtBytes(total)}'
                    : '—',
                style: const TextStyle(
                  color: Colors.white70,
                  fontSize: 12,
                  fontFamily: 'monospace',
                ),
              ),
            ],
          ),
          const SizedBox(height: 8),
          ClipRRect(
            borderRadius: BorderRadius.circular(3),
            child: LinearProgressIndicator(
              value: frac,
              minHeight: 6,
              backgroundColor: Colors.white.withValues(alpha: 0.08),
              valueColor: const AlwaysStoppedAnimation<Color>(_kAccent),
            ),
          ),
        ],
      ),
    );
  }

  Widget _empty(DeviceController ctrl) {
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(
            Icons.folder_off_outlined,
            size: 48,
            color: Colors.white.withValues(alpha: 0.15),
          ),
          const SizedBox(height: 12),
          Text(
            ctrl.fsListing ? 'Loading…' : 'No files stored',
            style: const TextStyle(color: Colors.white54, fontSize: 14),
          ),
          const SizedBox(height: 6),
          const Text(
            'Saved IR signals and Evil Portal data appear here',
            style: TextStyle(color: Colors.white38, fontSize: 12),
          ),
        ],
      ),
    );
  }

  IconData _iconFor(String path) {
    if (path.endsWith('.ir')) return Icons.graphic_eq;
    if (path.startsWith('portal/')) return Icons.vpn_lock;
    return Icons.insert_drive_file_outlined;
  }

  Widget _tile(DeviceController ctrl, FsFile f) {
    return Container(
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.03),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: _kAccent.withValues(alpha: 0.3)),
      ),
      padding: const EdgeInsets.all(12),
      child: Row(
        children: [
          Container(
            width: 42,
            height: 42,
            decoration: BoxDecoration(
              color: _kAccent.withValues(alpha: 0.14),
              borderRadius: BorderRadius.circular(12),
            ),
            child: Icon(_iconFor(f.path), color: _kAccent, size: 20),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  f.path,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 14,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  _fmtBytes(f.size),
                  style: const TextStyle(
                    color: Colors.white54,
                    fontSize: 11,
                    fontFamily: 'monospace',
                  ),
                ),
              ],
            ),
          ),
          IconButton(
            tooltip: 'Delete',
            icon: const Icon(Icons.delete_outline, size: 20),
            color: const Color(0xFFFF5252),
            onPressed: () => _confirmDelete(ctrl, f.path),
          ),
        ],
      ),
    );
  }

  Future<void> _confirmDelete(DeviceController ctrl, String path) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (dctx) => AlertDialog(
        title: const Text('Delete file?'),
        content: Text('Permanently remove "$path" from the device?'),
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
      await _guard(ctrl.fsDelete(path));
    }
  }
}
