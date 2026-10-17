import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/ir_models.dart';
import '../services/device_controller.dart';
import 'ir_hub_screen.dart';

/// IR receiver: a live spectrum of the captured burst, the decoded bytes,
/// the list of detected signals, and Start Receiver / Spectrum controls.
class IrReceiverScreen extends StatefulWidget {
  const IrReceiverScreen({super.key});

  @override
  State<IrReceiverScreen> createState() => _IrReceiverScreenState();
}

class _IrReceiverScreenState extends State<IrReceiverScreen>
    with SingleTickerProviderStateMixin {
  late final AnimationController _sweep;
  DeviceController? _controller;
  bool _initialized = false;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (_initialized) return;
    _initialized = true;
    _controller = context.read<DeviceController>();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      final messenger = ScaffoldMessenger.of(context);
      _controller!.irReceiverReset().catchError((Object error) {
        if (mounted) {
          messenger.showSnackBar(
            SnackBar(content: Text('Failed to reset receiver: $error')),
          );
        }
      });
    });
  }

  @override
  void initState() {
    super.initState();
    _sweep = AnimationController(
      vsync: this,
      duration: const Duration(seconds: 2),
    )..repeat();
  }

  @override
  void dispose() {
    _sweep.dispose();
    final ctrl = _controller;
    if (ctrl != null) {
      unawaited(ctrl.irReceiverClose());
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final latest = ctrl.irDetected.isNotEmpty ? ctrl.irDetected.first : null;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Infrared Receiver'),
        actions: [
          IconButton(
            tooltip: 'Clear',
            icon: const Icon(Icons.clear_all),
            onPressed: ctrl.irDetected.isEmpty
                ? null
                : () => ctrl.irClearDetected(),
          ),
        ],
      ),
      body: Column(
        children: [
          _spectrumPanel(ctrl),
          _bytesStrip(latest),
          const SizedBox(height: 4),
          _detectedHeader(ctrl),
          Expanded(child: _detectedList(ctrl)),
          _controls(context, ctrl),
        ],
      ),
    );
  }

  // ── Spectrum ─────────────────────────────────────────────
  Widget _spectrumPanel(DeviceController ctrl) {
    return Container(
      height: 150,
      margin: const EdgeInsets.fromLTRB(14, 14, 14, 8),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(16),
        color: const Color(0xFF0B0504),
        border: Border.all(color: kIrAccent.withValues(alpha: 0.35)),
        boxShadow: [
          BoxShadow(color: kIrAccent.withValues(alpha: 0.10), blurRadius: 16),
        ],
      ),
      clipBehavior: Clip.antiAlias,
      child: AnimatedBuilder(
        animation: _sweep,
        builder: (_, _) => CustomPaint(
          painter: _SpectrumPainter(
            bins: ctrl.irSpectrumBins,
            active: ctrl.irSpectrumOn,
            sweep: _sweep.value,
          ),
          child: Stack(
            children: [
              Positioned(
                left: 12,
                top: 10,
                child: Row(
                  children: [
                    Icon(
                      Icons.graphic_eq,
                      size: 15,
                      color: ctrl.irSpectrumOn ? kIrAccent : Colors.white24,
                    ),
                    const SizedBox(width: 6),
                    Text(
                      ctrl.irSpectrumOn ? 'IR SPECTRUM' : 'SPECTRUM OFF',
                      style: TextStyle(
                        color: ctrl.irSpectrumOn ? kIrAccent : Colors.white24,
                        fontSize: 10,
                        letterSpacing: 2,
                        fontFamily: 'monospace',
                      ),
                    ),
                  ],
                ),
              ),
              if (ctrl.irSpectrumTotalUs > 0)
                Positioned(
                  right: 12,
                  top: 10,
                  child: Text(
                    '${(ctrl.irSpectrumTotalUs / 1000).toStringAsFixed(1)} ms',
                    style: const TextStyle(
                      color: Colors.white38,
                      fontSize: 10,
                      fontFamily: 'monospace',
                    ),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }

  // ── Decoded bytes strip ──────────────────────────────────
  Widget _bytesStrip(IrDetectedSignal? latest) {
    final text = latest == null
        ? 'bytes —  ·  waiting for signal'
        : '${latest.proto}  ·  ${latest.addrCmd}  ·  bytes ${latest.bytes}';
    return Container(
      width: double.infinity,
      margin: const EdgeInsets.symmetric(horizontal: 14),
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.45),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white.withValues(alpha: 0.08)),
      ),
      child: Text(
        text,
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
        style: TextStyle(
          color: latest == null ? Colors.white38 : const Color(0xFFFFB38A),
          fontSize: 12.5,
          fontFamily: 'monospace',
          letterSpacing: 0.5,
        ),
      ),
    );
  }

  Widget _detectedHeader(DeviceController ctrl) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(18, 10, 18, 4),
      child: Row(
        children: [
          const Text(
            'DETECTED',
            style: TextStyle(
              color: Colors.white38,
              fontSize: 11,
              letterSpacing: 2,
              fontWeight: FontWeight.bold,
            ),
          ),
          const Spacer(),
          Text(
            '${ctrl.irDetected.length}',
            style: const TextStyle(color: Colors.white38, fontSize: 11),
          ),
        ],
      ),
    );
  }

  Widget _detectedList(DeviceController ctrl) {
    if (ctrl.irDetected.isEmpty) {
      return Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(
              Icons.settings_remote,
              size: 44,
              color: Colors.white.withValues(alpha: 0.15),
            ),
            const SizedBox(height: 12),
            Text(
              ctrl.irRxRunning
                  ? 'Point a remote at the receiver and press a button'
                  : 'Start the receiver to capture signals',
              style: const TextStyle(color: Colors.white38, fontSize: 13),
              textAlign: TextAlign.center,
            ),
          ],
        ),
      );
    }
    return ListView.separated(
      padding: const EdgeInsets.fromLTRB(14, 0, 14, 8),
      itemCount: ctrl.irDetected.length,
      separatorBuilder: (_, _) => const SizedBox(height: 8),
      itemBuilder: (_, i) => _signalTile(ctrl, ctrl.irDetected[i], i == 0),
    );
  }

  Widget _signalTile(DeviceController ctrl, IrDetectedSignal sig, bool latest) {
    final known = sig.proto != 'RAW' && sig.proto != 'Unknown';
    final color = known ? const Color(0xFF4FA8FF) : const Color(0xFFB0B0B0);
    return Material(
      color: Colors.transparent,
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: () => _showDetail(ctrl, sig, latest),
        child: Container(
          padding: const EdgeInsets.all(12),
          decoration: BoxDecoration(
            color: Colors.white.withValues(alpha: 0.03),
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: color.withValues(alpha: 0.3)),
          ),
          child: Row(
            children: [
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 5),
                decoration: BoxDecoration(
                  color: color.withValues(alpha: 0.14),
                  borderRadius: BorderRadius.circular(8),
                ),
                child: Text(
                  sig.proto,
                  style: TextStyle(
                    color: color,
                    fontSize: 11,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      known ? sig.addrCmd : '${sig.rawCount} edges',
                      style: const TextStyle(
                        color: Colors.white,
                        fontSize: 13,
                        fontFamily: 'monospace',
                      ),
                    ),
                    const SizedBox(height: 2),
                    Text(
                      'bytes ${sig.bytes}  ·  ${_ago(sig.at)}',
                      style: const TextStyle(
                        color: Colors.white38,
                        fontSize: 11,
                      ),
                    ),
                  ],
                ),
              ),
              if (latest)
                const Icon(
                  Icons.fiber_manual_record,
                  color: kIrAccent,
                  size: 10,
                ),
              const SizedBox(width: 4),
              const Icon(Icons.chevron_right, color: Colors.white24, size: 20),
            ],
          ),
        ),
      ),
    );
  }

  String _ago(DateTime t) {
    final s = DateTime.now().difference(t).inSeconds;
    if (s < 1) return 'now';
    if (s < 60) return '${s}s ago';
    return '${s ~/ 60}m ago';
  }

  // ── Detail sheet ─────────────────────────────────────────
  void _showDetail(DeviceController ctrl, IrDetectedSignal sig, bool latest) {
    showModalBottomSheet<void>(
      context: context,
      backgroundColor: const Color(0xFF14100F),
      isScrollControlled: true,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(22)),
      ),
      builder: (sheetCtx) => _SignalDetailSheet(
        signal: sig,
        canSave: sig.captureId > 0 || latest,
        onSave: () {
          Navigator.pop(sheetCtx);
          _promptSave(ctrl, sig);
        },
      ),
    );
  }

  Future<void> _promptSave(
    DeviceController ctrl,
    IrDetectedSignal signal,
  ) async {
    final nameCtrl = TextEditingController(
      text:
          'ir_${signal.captureId}_${DateTime.now().millisecondsSinceEpoch % 100000}',
    );
    final messenger = ScaffoldMessenger.of(context);
    final name = await showDialog<String>(
      context: context,
      builder: (dctx) => AlertDialog(
        title: const Text('Save signal'),
        content: TextField(
          controller: nameCtrl,
          autofocus: true,
          decoration: const InputDecoration(
            labelText: 'Name',
            hintText: 'e.g. living-room-tv-power',
            border: OutlineInputBorder(),
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(dctx),
            child: const Text('Cancel'),
          ),
          ElevatedButton(
            onPressed: () => Navigator.pop(dctx, nameCtrl.text.trim()),
            child: const Text('Save'),
          ),
        ],
      ),
    );
    if (name == null || name.isEmpty) return;
    // Single token only — keep it consistent with the firmware's filename
    // sanitizer so emulate/delete (which reference by name) always match.
    final safe = name.replaceAll(RegExp(r'[^A-Za-z0-9_\-.]'), '_');
    try {
      await ctrl.irSaveCapture(safe, signal.captureId);
      messenger.showSnackBar(SnackBar(content: Text('Saved "$safe"')));
    } catch (e) {
      messenger.showSnackBar(SnackBar(content: Text('Save failed: $e')));
    }
  }

  // ── Bottom controls ──────────────────────────────────────
  Widget _controls(BuildContext context, DeviceController ctrl) {
    final messenger = ScaffoldMessenger.of(context);
    Future<void> guard(Future<void> f) async {
      try {
        await f;
      } catch (e) {
        messenger.showSnackBar(SnackBar(content: Text('Failed: $e')));
      }
    }

    return SafeArea(
      top: false,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(14, 6, 14, 12),
        child: Row(
          children: [
            Expanded(
              flex: 3,
              child: SizedBox(
                height: 52,
                child: ElevatedButton.icon(
                  style: ElevatedButton.styleFrom(
                    backgroundColor: ctrl.irRxRunning
                        ? const Color(0xFF8A1F1F)
                        : kIrAccent,
                    foregroundColor: Colors.white,
                  ),
                  icon: Icon(
                    ctrl.irRxRunning ? Icons.stop : Icons.sensors,
                    size: 20,
                  ),
                  label: Text(
                    ctrl.irRxRunning ? 'Stop Receiver' : 'Start Receiver',
                    style: const TextStyle(
                      fontSize: 15,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  onPressed: () => guard(
                    ctrl.irRxRunning ? ctrl.irRxStop() : ctrl.irRxStart(),
                  ),
                ),
              ),
            ),
            const SizedBox(width: 10),
            Expanded(
              flex: 2,
              child: SizedBox(
                height: 52,
                child: OutlinedButton.icon(
                  style: OutlinedButton.styleFrom(
                    foregroundColor: ctrl.irSpectrumOn
                        ? kIrAccent
                        : Colors.white54,
                    side: BorderSide(
                      color: ctrl.irSpectrumOn ? kIrAccent : Colors.white24,
                    ),
                  ),
                  icon: Icon(
                    ctrl.irSpectrumOn ? Icons.graphic_eq : Icons.show_chart,
                    size: 18,
                  ),
                  label: Text(
                    ctrl.irSpectrumOn ? 'Spectrum' : 'Spectrum',
                    style: const TextStyle(fontSize: 13),
                  ),
                  onPressed: () =>
                      guard(ctrl.irSpectrumSet(!ctrl.irSpectrumOn)),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

// ─── Signal detail sheet ────────────────────────────────────────────────────

class _SignalDetailSheet extends StatelessWidget {
  const _SignalDetailSheet({
    required this.signal,
    required this.canSave,
    required this.onSave,
  });

  final IrDetectedSignal signal;
  final bool canSave;
  final VoidCallback onSave;

  @override
  Widget build(BuildContext context) {
    final known = signal.proto != 'RAW' && signal.proto != 'Unknown';
    return Padding(
      padding: EdgeInsets.only(
        left: 20,
        right: 20,
        top: 18,
        bottom: MediaQuery.of(context).viewInsets.bottom + 24,
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Center(
            child: Container(
              width: 42,
              height: 4,
              decoration: BoxDecoration(
                color: Colors.white24,
                borderRadius: BorderRadius.circular(2),
              ),
            ),
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              const Icon(Icons.article_outlined, color: kIrAccent),
              const SizedBox(width: 10),
              Text(
                'Signal profile',
                style: TextStyle(
                  color: Colors.white.withValues(alpha: 0.9),
                  fontSize: 18,
                  fontWeight: FontWeight.bold,
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          _row('Protocol', signal.proto),
          if (known) ...[
            _row(
              'Address',
              '0x${signal.address.toRadixString(16).toUpperCase()}',
            ),
            _row(
              'Command',
              '0x${signal.command.toRadixString(16).toUpperCase()}',
            ),
            if (signal.bits > 0) _row('Bits', '${signal.bits}'),
          ],
          _row('Bytes', signal.bytes),
          _row('Raw edges', '${signal.rawCount}'),
          const SizedBox(height: 14),
          const Text(
            'RAW TIMING (µs)',
            style: TextStyle(
              color: Colors.white38,
              fontSize: 11,
              letterSpacing: 1.5,
              fontWeight: FontWeight.bold,
            ),
          ),
          const SizedBox(height: 6),
          Container(
            width: double.infinity,
            constraints: const BoxConstraints(maxHeight: 120),
            padding: const EdgeInsets.all(10),
            decoration: BoxDecoration(
              color: Colors.black.withValues(alpha: 0.5),
              borderRadius: BorderRadius.circular(10),
            ),
            child: SingleChildScrollView(
              child: Text(
                signal.raw.isEmpty ? '(receiving…)' : signal.raw.join(' '),
                style: const TextStyle(
                  color: Color(0xFF8FFF8F),
                  fontFamily: 'monospace',
                  fontSize: 11,
                  height: 1.4,
                ),
              ),
            ),
          ),
          const SizedBox(height: 18),
          SizedBox(
            width: double.infinity,
            height: 50,
            child: ElevatedButton.icon(
              style: ElevatedButton.styleFrom(
                backgroundColor: canSave ? kIrAccent : Colors.white10,
                foregroundColor: canSave ? Colors.white : Colors.white38,
              ),
              icon: const Icon(Icons.save_alt),
              label: Text(
                canSave
                    ? 'Save to storage'
                    : 'Capture is no longer available to save',
              ),
              onPressed: canSave ? onSave : null,
            ),
          ),
        ],
      ),
    );
  }

  Widget _row(String k, String v) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 5),
      child: Row(
        children: [
          SizedBox(
            width: 110,
            child: Text(
              k,
              style: const TextStyle(color: Colors.white54, fontSize: 13),
            ),
          ),
          Expanded(
            child: Text(
              v,
              style: const TextStyle(
                color: Colors.white,
                fontSize: 14,
                fontFamily: 'monospace',
                fontWeight: FontWeight.w600,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

// ─── Spectrum painter ───────────────────────────────────────────────────────

class _SpectrumPainter extends CustomPainter {
  _SpectrumPainter({
    required this.bins,
    required this.active,
    required this.sweep,
  });

  final List<int> bins;
  final bool active;
  final double sweep;

  static const _grid = Color(0xFF3A1512);

  @override
  void paint(Canvas canvas, Size size) {
    final gridPaint = Paint()
      ..color = _grid
      ..strokeWidth = 0.6;
    for (int i = 1; i < 4; i++) {
      final y = size.height * i / 4;
      canvas.drawLine(Offset(0, y), Offset(size.width, y), gridPaint);
    }

    final n = bins.isEmpty ? 48 : bins.length;
    final barW = size.width / n;
    final baseline = size.height - 6;
    final usable = size.height - 34;

    for (int i = 0; i < n; i++) {
      final v = bins.isEmpty ? 0 : bins[i].clamp(0, 100);
      final h = bins.isEmpty ? 2.0 : (v / 100.0) * usable;
      final x = i * barW;
      final rect = Rect.fromLTWH(
        x + barW * 0.15,
        baseline - h,
        barW * 0.7,
        h < 2 ? 2 : h,
      );
      final t = i / n;
      final color = Color.lerp(kIrAccent, kIrAccent2, t)!;
      canvas.drawRRect(
        RRect.fromRectAndRadius(rect, const Radius.circular(1.5)),
        Paint()
          ..color = color.withValues(alpha: bins.isEmpty ? 0.18 : 0.9)
          ..maskFilter = bins.isEmpty
              ? null
              : const MaskFilter.blur(BlurStyle.normal, 0.4),
      );
    }

    // Scanning line when the receiver spectrum is live but idle.
    if (active && bins.isEmpty) {
      final x = sweep * size.width;
      canvas.drawLine(
        Offset(x, 28),
        Offset(x, baseline),
        Paint()
          ..color = kIrAccent.withValues(alpha: 0.5)
          ..strokeWidth = 1.5,
      );
    }
  }

  @override
  bool shouldRepaint(_SpectrumPainter old) =>
      old.bins != bins || old.active != active || old.sweep != sweep;
}
