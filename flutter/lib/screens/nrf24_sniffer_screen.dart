import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';
import '../widgets/nrf_kit.dart';

const _kAccent = Color(0xFFFFB000);

class Nrf24SnifferScreen extends StatefulWidget {
  const Nrf24SnifferScreen({super.key});

  @override
  State<Nrf24SnifferScreen> createState() => _Nrf24SnifferScreenState();
}

class _Nrf24SnifferScreenState extends State<Nrf24SnifferScreen> {
  DeviceController? _ctrl;
  double _channel = 76;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      _restart();
    });
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
  }

  @override
  void dispose() {
    unawaited(_ctrl?.nrfSniffStop().catchError((Object _) {}));
    super.dispose();
  }

  Future<void> _restart() async {
    final ctrl = context.read<DeviceController>();
    await ctrl.nrfSniffStop().catchError((Object _) {});
    await Future<void>.delayed(const Duration(milliseconds: 120));
    if (!mounted) return;
    unawaited(ctrl.nrfSniffStart(_channel.round(), 0).catchError((Object e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context)
          .showSnackBar(SnackBar(content: Text('Sniff failed: $e')));
    }));
  }

  Future<void> _replayDialog(DeviceController ctrl, NrfSniffPacket pkt) async {
    var count = 10.0;
    var delayMs = 5.0;
    await showModalBottomSheet<void>(
      context: context,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(18)),
      ),
      builder: (sheetCtx) => StatefulBuilder(
        builder: (sheetCtx, setSheet) => Padding(
          padding: EdgeInsets.fromLTRB(
              20, 18, 20, 18 + MediaQuery.of(sheetCtx).viewInsets.bottom),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  const Icon(Icons.replay, color: _kAccent, size: 20),
                  const SizedBox(width: 8),
                  Text('Replay packet #${pkt.idx}',
                      style: const TextStyle(
                          color: Colors.white,
                          fontSize: 16,
                          fontWeight: FontWeight.bold)),
                ],
              ),
              const SizedBox(height: 6),
              Text('CH ${pkt.channel} · ${pkt.len} bytes',
                  style: const TextStyle(color: Colors.white38, fontSize: 12)),
              const SizedBox(height: 14),
              Text('Repeat ${count.round()}×',
                  style: const TextStyle(color: Colors.white70, fontSize: 13)),
              Slider(
                value: count,
                min: 1,
                max: 500,
                divisions: 499,
                activeColor: _kAccent,
                label: '${count.round()}',
                onChanged: (v) => setSheet(() => count = v),
              ),
              Text('Delay ${delayMs.round()} ms',
                  style: const TextStyle(color: Colors.white70, fontSize: 13)),
              Slider(
                value: delayMs,
                min: 0,
                max: 100,
                divisions: 100,
                activeColor: _kAccent,
                label: '${delayMs.round()} ms',
                onChanged: (v) => setSheet(() => delayMs = v),
              ),
              const SizedBox(height: 10),
              SizedBox(
                width: double.infinity,
                height: 48,
                child: FilledButton.icon(
                  style: FilledButton.styleFrom(
                    backgroundColor: _kAccent.withValues(alpha: 0.2),
                    foregroundColor: _kAccent,
                  ),
                  icon: const Icon(Icons.send),
                  label: const Text('REPLAY',
                      style: TextStyle(
                          fontWeight: FontWeight.bold, letterSpacing: 1)),
                  onPressed: () async {
                    Navigator.of(sheetCtx).pop();
                    // The running sniffer owns the radio; release it before the
                    // replay task tries to claim it, otherwise replay is a no-op.
                    await ctrl.nrfSniffStop().catchError((Object _) {});
                    await Future<void>.delayed(
                        const Duration(milliseconds: 150));
                    await ctrl
                        .nrfReplay(pkt.idx,
                            count: count.round(),
                            delayUs: delayMs.round() * 1000)
                        .catchError((Object _) {});
                  },
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final packets = ctrl.nrfSniffPackets;

    return Scaffold(
      appBar: AppBar(
        iconTheme: const IconThemeData(color: _kAccent),
        title: const Text(
          'nRF SNIFFER',
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
                unawaited(ctrl.nrfSniffClear().catchError((Object _) {})),
          ),
        ],
      ),
      body: Column(
        children: [
          DeviceIssueBanner(controller: ctrl),
          Padding(
            padding: const EdgeInsets.fromLTRB(14, 12, 14, 2),
            child: Row(
              children: [
                _chip('CH ${_channel.round()}', _kAccent),
                const SizedBox(width: 8),
                _chip('CAP ${ctrl.nrfSniffCaptured}', const Color(0xFF39FF14)),
                const SizedBox(width: 8),
                _chip('DROP ${ctrl.nrfSniffDropped}', const Color(0xFFFF5050)),
                const Spacer(),
                if (ctrl.nrfReplayActive)
                  _chip('REPLAY', const Color(0xFF00E5FF))
                else if (ctrl.nrfSniffRunning)
                  const NrfPulseDot(color: _kAccent, size: 9),
              ],
            ),
          ),

          // Channel selector
          Padding(
            padding: const EdgeInsets.fromLTRB(14, 4, 14, 0),
            child: Row(
              children: [
                const Text('Channel',
                    style: TextStyle(color: Colors.white54, fontSize: 12)),
                Expanded(
                  child: Slider(
                    value: _channel,
                    min: 0,
                    max: 125,
                    divisions: 125,
                    activeColor: _kAccent,
                    label: 'CH ${_channel.round()} · '
                        '${2400 + _channel.round()} MHz',
                    onChanged: (v) => setState(() => _channel = v),
                    onChangeEnd: (_) => _restart(),
                  ),
                ),
                Text('${2400 + _channel.round()}MHz',
                    style: const TextStyle(
                        color: Colors.white38,
                        fontSize: 11,
                        fontFamily: 'monospace')),
              ],
            ),
          ),

          const Divider(height: 1, color: Colors.white12),

          Expanded(
            child: packets.isEmpty
                ? Center(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(Icons.sensors,
                            color: Colors.white.withValues(alpha: 0.12),
                            size: 48),
                        const SizedBox(height: 10),
                        const Text('Listening for packets…',
                            style:
                                TextStyle(color: Colors.white38, fontSize: 13)),
                        const SizedBox(height: 4),
                        const Text(
                          'Tip: pseudo-promiscuous capture is noisy.\n'
                          'Tap a packet to replay it.',
                          textAlign: TextAlign.center,
                          style: TextStyle(color: Colors.white24, fontSize: 11),
                        ),
                      ],
                    ),
                  )
                : ListView.separated(
                    padding: const EdgeInsets.fromLTRB(12, 8, 12, 16),
                    itemCount: packets.length,
                    separatorBuilder: (context, index) =>
                        const SizedBox(height: 8),
                    itemBuilder: (_, i) => _packetTile(ctrl, packets[i]),
                  ),
          ),
        ],
      ),
    );
  }

  Widget _packetTile(DeviceController ctrl, NrfSniffPacket p) {
    return Material(
      color: Colors.white.withValues(alpha: 0.03),
      borderRadius: BorderRadius.circular(12),
      child: InkWell(
        onTap: () => _replayDialog(ctrl, p),
        borderRadius: BorderRadius.circular(12),
        child: Container(
          padding: const EdgeInsets.all(12),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: _kAccent.withValues(alpha: 0.18)),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Text('#${p.idx}',
                      style: const TextStyle(
                          color: _kAccent,
                          fontSize: 12,
                          fontWeight: FontWeight.bold,
                          fontFamily: 'monospace')),
                  const SizedBox(width: 10),
                  Text('CH ${p.channel}',
                      style: const TextStyle(
                          color: Colors.white54, fontSize: 11)),
                  const SizedBox(width: 10),
                  Text('${p.len}B',
                      style: const TextStyle(
                          color: Colors.white54, fontSize: 11)),
                  const Spacer(),
                  Icon(Icons.replay,
                      color: _kAccent.withValues(alpha: 0.6), size: 16),
                ],
              ),
              const SizedBox(height: 6),
              Text(
                p.hex.isEmpty ? '—' : p.hex,
                style: const TextStyle(
                    color: Colors.white70,
                    fontSize: 12,
                    height: 1.4,
                    fontFamily: 'monospace'),
              ),
            ],
          ),
        ),
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
