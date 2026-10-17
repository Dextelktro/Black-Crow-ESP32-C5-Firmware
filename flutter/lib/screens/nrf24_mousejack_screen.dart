import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';
import '../widgets/nrf_kit.dart';

const _kAccent = Color(0xFFB06CFF);

class Nrf24MousejackScreen extends StatefulWidget {
  const Nrf24MousejackScreen({super.key});

  @override
  State<Nrf24MousejackScreen> createState() => _Nrf24MousejackScreenState();
}

class _Nrf24MousejackScreenState extends State<Nrf24MousejackScreen> {
  DeviceController? _ctrl;
  int _duration = 60;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      final ctrl = context.read<DeviceController>();
      unawaited(ctrl.nrfMjScanStart(_duration).catchError((Object e) {
        if (!mounted) return;
        ScaffoldMessenger.of(context)
            .showSnackBar(SnackBar(content: Text('MouseJack failed: $e')));
      }));
    });
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
  }

  @override
  void dispose() {
    unawaited(_ctrl?.nrfMjScanStop().catchError((Object _) {}));
    super.dispose();
  }

  Future<void> _injectDialog(DeviceController ctrl, NrfMjTarget t) async {
    final controller = TextEditingController();
    await showModalBottomSheet<void>(
      context: context,
      isScrollControlled: true,
      backgroundColor: const Color(0xFF17102A),
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(18)),
      ),
      builder: (sheetCtx) => Padding(
        padding: EdgeInsets.fromLTRB(
            20, 18, 20, 18 + MediaQuery.of(sheetCtx).viewInsets.bottom),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                const Icon(Icons.keyboard, color: _kAccent, size: 20),
                const SizedBox(width: 8),
                Expanded(
                  child: Text('Inject → ${t.addr}',
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                          color: Colors.white,
                          fontSize: 15,
                          fontWeight: FontWeight.bold)),
                ),
              ],
            ),
            const SizedBox(height: 4),
            Text('CH ${t.channel} · ${t.isLogitech ? 'Logitech' : 'unknown'}',
                style: const TextStyle(color: Colors.white38, fontSize: 12)),
            const SizedBox(height: 14),
            TextField(
              controller: controller,
              autofocus: true,
              maxLength: 64,
              style: const TextStyle(
                  color: Colors.white, fontFamily: 'monospace'),
              cursorColor: _kAccent,
              decoration: InputDecoration(
                hintText: 'Keystrokes to type…',
                hintStyle: const TextStyle(color: Colors.white30),
                filled: true,
                fillColor: Colors.black.withValues(alpha: 0.3),
                enabledBorder: OutlineInputBorder(
                  borderRadius: BorderRadius.circular(10),
                  borderSide:
                      BorderSide(color: _kAccent.withValues(alpha: 0.3)),
                ),
                focusedBorder: OutlineInputBorder(
                  borderRadius: BorderRadius.circular(10),
                  borderSide: const BorderSide(color: _kAccent),
                ),
              ),
            ),
            const Text(
              'Only a–z, 0–9, space and basic punctuation are mapped. '
              'Injection is best-effort against the captured address.',
              style: TextStyle(color: Colors.white38, fontSize: 11, height: 1.3),
            ),
            const SizedBox(height: 12),
            SizedBox(
              width: double.infinity,
              height: 48,
              child: FilledButton.icon(
                style: FilledButton.styleFrom(
                  backgroundColor: _kAccent.withValues(alpha: 0.2),
                  foregroundColor: _kAccent,
                ),
                icon: const Icon(Icons.send),
                label: const Text('INJECT',
                    style: TextStyle(
                        fontWeight: FontWeight.bold, letterSpacing: 1)),
                onPressed: () {
                  final text = controller.text.trim();
                  if (text.isEmpty) return;
                  Navigator.of(sheetCtx).pop();
                  unawaited(ctrl
                      .nrfMjInject(t.idx, text)
                      .catchError((Object _) {}));
                },
              ),
            ),
          ],
        ),
      ),
    );
    controller.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final targets = ctrl.nrfMjTargets;
    final scanning = ctrl.nrfMjRunning;

    return Scaffold(
      backgroundColor: const Color(0xFF0C0618),
      appBar: AppBar(
        backgroundColor: Colors.black,
        iconTheme: const IconThemeData(color: _kAccent),
        title: const Text(
          'MOUSEJACK',
          style: TextStyle(
              color: _kAccent,
              fontWeight: FontWeight.bold,
              letterSpacing: 2,
              fontSize: 16),
        ),
        actions: [
          IconButton(
            tooltip: 'Clear',
            icon: const Icon(Icons.delete_outline, color: _kAccent),
            onPressed: () =>
                unawaited(ctrl.nrfMjClear().catchError((Object _) {})),
          ),
        ],
      ),
      body: Column(
        children: [
          DeviceIssueBanner(controller: ctrl),
          Padding(
            padding: const EdgeInsets.fromLTRB(14, 12, 14, 8),
            child: Row(
              children: [
                _chip('FOUND ${ctrl.nrfMjFound}', _kAccent),
                const SizedBox(width: 8),
                _chip('${ctrl.nrfMjElapsed}s', const Color(0xFF00E5FF)),
                const Spacer(),
                if (ctrl.nrfMjInjecting)
                  _chip('INJECTING', const Color(0xFFFFB000))
                else if (scanning)
                  const NrfPulseDot(color: _kAccent, size: 9),
              ],
            ),
          ),
          Expanded(
            child: targets.isEmpty
                ? Center(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(Icons.mouse,
                            color: Colors.white.withValues(alpha: 0.12),
                            size: 48),
                        const SizedBox(height: 10),
                        Text(
                            scanning
                                ? 'Scanning for wireless HID dongles…'
                                : 'No targets found.',
                            style: const TextStyle(
                                color: Colors.white38, fontSize: 13)),
                      ],
                    ),
                  )
                : ListView.separated(
                    padding: const EdgeInsets.fromLTRB(12, 4, 12, 16),
                    itemCount: targets.length,
                    separatorBuilder: (context, index) =>
                        const SizedBox(height: 8),
                    itemBuilder: (_, i) =>
                        _targetTile(ctrl, targets[i], scanning),
                  ),
          ),
          _controls(ctrl, scanning),
        ],
      ),
    );
  }

  Widget _targetTile(DeviceController ctrl, NrfMjTarget t, bool scanning) {
    final logi = t.isLogitech;
    final c = logi ? _kAccent : Colors.white54;
    return Material(
      color: Colors.white.withValues(alpha: 0.03),
      borderRadius: BorderRadius.circular(12),
      child: InkWell(
        onTap: scanning ? null : () => _injectDialog(ctrl, t),
        borderRadius: BorderRadius.circular(12),
        child: Container(
          padding: const EdgeInsets.all(14),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: c.withValues(alpha: 0.25)),
          ),
          child: Row(
            children: [
              Container(
                width: 42,
                height: 42,
                decoration: BoxDecoration(
                  color: c.withValues(alpha: 0.12),
                  borderRadius: BorderRadius.circular(10),
                ),
                child: Icon(logi ? Icons.keyboard : Icons.devices_other,
                    color: c, size: 22),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(t.addr,
                        style: const TextStyle(
                            color: Colors.white,
                            fontSize: 14,
                            fontWeight: FontWeight.bold,
                            fontFamily: 'monospace')),
                    const SizedBox(height: 3),
                    Text(
                        '#${t.idx} · CH ${t.channel} · ${t.hits} hits · '
                        '${logi ? 'Logitech' : 'unknown'}',
                        style: const TextStyle(
                            color: Colors.white54, fontSize: 11)),
                  ],
                ),
              ),
              if (scanning)
                const Icon(Icons.lock_outline, color: Colors.white24, size: 18)
              else
                const Icon(Icons.keyboard, color: _kAccent, size: 18),
            ],
          ),
        ),
      ),
    );
  }

  Widget _controls(DeviceController ctrl, bool scanning) {
    return Container(
      padding: const EdgeInsets.fromLTRB(14, 10, 14, 16),
      decoration: const BoxDecoration(
        border: Border(top: BorderSide(color: Colors.white12)),
      ),
      child: Row(
        children: [
          if (!scanning)
            Expanded(
              child: Wrap(
                spacing: 6,
                children: [
                  for (final d in const [30, 60, 120, 300])
                    GestureDetector(
                      onTap: () => setState(() => _duration = d),
                      child: Container(
                        padding: const EdgeInsets.symmetric(
                            horizontal: 10, vertical: 6),
                        decoration: BoxDecoration(
                          color: _duration == d
                              ? _kAccent.withValues(alpha: 0.18)
                              : Colors.white.withValues(alpha: 0.03),
                          borderRadius: BorderRadius.circular(8),
                          border: Border.all(
                              color: _duration == d
                                  ? _kAccent
                                  : Colors.white12),
                        ),
                        child: Text('${d}s',
                            style: TextStyle(
                                color: _duration == d
                                    ? _kAccent
                                    : Colors.white60,
                                fontSize: 12,
                                fontWeight: FontWeight.bold)),
                      ),
                    ),
                ],
              ),
            )
          else
            const Expanded(
              child: Text('Tap a target after the scan stops to inject.',
                  style: TextStyle(color: Colors.white38, fontSize: 11)),
            ),
          const SizedBox(width: 10),
          SizedBox(
            height: 44,
            child: FilledButton.icon(
              style: FilledButton.styleFrom(
                backgroundColor: _kAccent.withValues(alpha: 0.2),
                foregroundColor: _kAccent,
              ),
              icon: Icon(scanning ? Icons.stop : Icons.radar),
              label: Text(scanning ? 'STOP' : 'SCAN',
                  style: const TextStyle(
                      fontWeight: FontWeight.bold, letterSpacing: 1)),
              onPressed: () {
                if (scanning) {
                  unawaited(ctrl.nrfMjScanStop().catchError((Object _) {}));
                } else {
                  unawaited(ctrl
                      .nrfMjScanStart(_duration)
                      .catchError((Object _) {}));
                }
              },
            ),
          ),
        ],
      ),
    );
  }

  Widget _chip(String t, Color c) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
        decoration: BoxDecoration(
          color: c.withValues(alpha: 0.12),
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: c.withValues(alpha: 0.4)),
        ),
        child: Text(t,
            style: TextStyle(
                color: c,
                fontSize: 11,
                fontWeight: FontWeight.bold,
                fontFamily: 'monospace')),
      );
}
