import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../theme/app_theme.dart';

const _kFindMyAccent = Color(0xFF8EA6FF);

class FindMyScreen extends StatefulWidget {
  const FindMyScreen({super.key});

  @override
  State<FindMyScreen> createState() => _FindMyScreenState();
}

class _FindMyScreenState extends State<FindMyScreen> {
  DeviceController? _ctrl;
  bool _startedHere = false;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl ??= context.read<DeviceController>();
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) unawaited(_startScan());
    });
  }

  @override
  void dispose() {
    final ctrl = _ctrl;
    if (_startedHere && ctrl != null && ctrl.bleWarRunning) {
      unawaited(
        ctrl.bleWarStop().catchError((Object error, StackTrace stackTrace) {
          debugPrint('Find My scan stop failed: $error\n$stackTrace');
        }),
      );
    }
    super.dispose();
  }

  Future<void> _startScan() async {
    final ctrl = _ctrl;
    if (ctrl == null || ctrl.bleWarRunning || ctrl.connectionLost) return;
    if (ctrl.bleScanRunning ||
        ctrl.bleSpamRunning ||
        ctrl.bleSpectrumRunning ||
        ctrl.wspamRunning) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text(
            'Stop the active BLE or Wi-Fi operation before starting Find My scan.',
          ),
        ),
      );
      return;
    }
    _startedHere = true;
    try {
      await ctrl.bleWarStart(active: false);
    } catch (error) {
      _startedHere = false;
      if (!mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Find My scan failed: $error')));
    }
  }

  List<BleWarDevice> _findMyCandidates(DeviceController ctrl) => ctrl
      .bleWarDevices
      .where(
        (device) =>
            device.vendor == 'apple' &&
            device.company.toUpperCase() == '004C' &&
            device.dtype == 'findmy' &&
            device.isFindMyAirTagFormat,
      )
      .toList(growable: false);

  void _openMonitor(BleWarDevice device) {
    final ctrl = _ctrl;
    if (ctrl == null) return;
    Navigator.of(context).push(
      AppPageRoute<void>(
        builder: (_) => ChangeNotifierProvider.value(
          value: ctrl,
          child: FindMyMonitorScreen(device: device),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final devices = _findMyCandidates(ctrl);
    final strongest = devices.isEmpty
        ? null
        : devices.map((device) => device.rssi).reduce((a, b) => a > b ? a : b);

    return Scaffold(
      appBar: AppBar(
        iconTheme: const IconThemeData(color: _kFindMyAccent),
        title: const Text(
          'APPLE AIRTAG SCAN',
          style: TextStyle(
            color: _kFindMyAccent,
            fontWeight: FontWeight.bold,
            letterSpacing: 1.5,
            fontSize: 16,
          ),
        ),
        actions: [
          if (ctrl.bleWarRunning)
            const Padding(
              padding: EdgeInsets.only(right: 16),
              child: Center(
                child: SizedBox(
                  width: 16,
                  height: 16,
                  child: CircularProgressIndicator(
                    strokeWidth: 2,
                    color: _kFindMyAccent,
                  ),
                ),
              ),
            ),
        ],
      ),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 12, 12, 8),
            child: Row(
              children: [
                _stat('CANDIDATES', '${devices.length}'),
                const SizedBox(width: 8),
                _stat('OBSERVATIONS', '${ctrl.bleFindMyObservations}'),
                const SizedBox(width: 8),
                _stat('STRONGEST', strongest == null ? '--' : '$strongest dBm'),
              ],
            ),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 0, 12, 8),
            child: Row(
              children: [
                Expanded(
                  child: Text(
                    ctrl.bleWarRunning
                        ? 'LIVE · ${ctrl.bleWarActive ? 'ACTIVE' : 'PASSIVE'} scan'
                        : 'SCAN STOPPED',
                    style: TextStyle(
                      color: ctrl.bleWarRunning
                          ? _kFindMyAccent
                          : Colors.white54,
                      fontSize: 11,
                      fontWeight: FontWeight.bold,
                      letterSpacing: 1,
                    ),
                  ),
                ),
                if (!ctrl.bleWarRunning)
                  TextButton.icon(
                    onPressed: _startScan,
                    icon: const Icon(Icons.radar, size: 16),
                    label: const Text('SCAN'),
                  ),
              ],
            ),
          ),
          Container(
            width: double.infinity,
            margin: const EdgeInsets.fromLTRB(12, 0, 12, 10),
            padding: const EdgeInsets.all(10),
            decoration: BoxDecoration(
              color: _kFindMyAccent.withValues(alpha: 0.08),
              borderRadius: BorderRadius.circular(10),
              border: Border.all(color: _kFindMyAccent.withValues(alpha: 0.22)),
            ),
            child: const Text(
              'Shows Apple advertisements that match the Find My lost-mode '
              'AirTag payload format. This BLE format does not cryptographically '
              'prove that a device is a genuine AirTag; compatible trackers or '
              'spoofed advertisements may use the same format.',
              style: TextStyle(
                color: Colors.white70,
                fontSize: 11,
                height: 1.4,
              ),
            ),
          ),
          Expanded(
            child: devices.isEmpty
                ? Center(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(
                          Icons.bluetooth_searching,
                          size: 42,
                          color: _kFindMyAccent.withValues(alpha: 0.45),
                        ),
                        const SizedBox(height: 10),
                        Text(
                          ctrl.connectionLost
                              ? 'ESP32-C5 disconnected'
                              : ctrl.bleWarRunning
                              ? 'Listening for AirTag-format advertisements…'
                              : 'No AirTag-format candidates in this scan',
                          textAlign: TextAlign.center,
                          style: const TextStyle(color: Colors.white54),
                        ),
                      ],
                    ),
                  )
                : ListView.separated(
                    padding: const EdgeInsets.fromLTRB(12, 0, 12, 14),
                    itemCount: devices.length,
                    separatorBuilder: (_, _) => const SizedBox(height: 8),
                    itemBuilder: (context, index) {
                      final device = devices[index];
                      return _CandidateTile(
                        device: device,
                        onTap: () => _openMonitor(device),
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }

  Widget _stat(String label, String value) => Expanded(
    child: Container(
      padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 9),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: _kFindMyAccent.withValues(alpha: 0.22)),
      ),
      child: Column(
        children: [
          Text(
            value,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(
              color: _kFindMyAccent,
              fontSize: 14,
              fontWeight: FontWeight.bold,
              fontFamily: 'monospace',
            ),
          ),
          const SizedBox(height: 3),
          Text(
            label,
            style: const TextStyle(
              color: Colors.white54,
              fontSize: 8,
              letterSpacing: 0.6,
            ),
          ),
        ],
      ),
    ),
  );
}

class _CandidateTile extends StatelessWidget {
  const _CandidateTile({required this.device, required this.onTap});

  final BleWarDevice device;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final color = _signalColor(device.rssi);
    final title = device.name.isEmpty ? 'AirTag-format candidate' : device.name;
    final secondsAgo = DateTime.now().difference(device.lastSeen).inSeconds;
    final lastSeen = secondsAgo < 2 ? 'just now' : '${secondsAgo}s ago';

    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(12),
        child: Container(
          padding: const EdgeInsets.all(12),
          decoration: BoxDecoration(
            color: Colors.black.withValues(alpha: 0.38),
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: _kFindMyAccent.withValues(alpha: 0.22)),
          ),
          child: Column(
            children: [
              Row(
                children: [
                  const Icon(
                    Icons.location_searching,
                    color: _kFindMyAccent,
                    size: 22,
                  ),
                  const SizedBox(width: 10),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          title,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: const TextStyle(
                            color: Colors.white,
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                        Text(
                          'BLE address ${device.addr}',
                          style: const TextStyle(
                            color: Colors.white54,
                            fontSize: 10,
                            fontFamily: 'monospace',
                          ),
                        ),
                      ],
                    ),
                  ),
                  Text(
                    '${device.rssi} dBm',
                    style: TextStyle(
                      color: color,
                      fontWeight: FontWeight.bold,
                      fontFamily: 'monospace',
                    ),
                  ),
                  const SizedBox(width: 5),
                  const Icon(Icons.chevron_right, color: Colors.white38),
                ],
              ),
              const SizedBox(height: 9),
              Row(
                children: [
                  Expanded(child: _SignalMeter(rssi: device.rssi)),
                  const SizedBox(width: 10),
                  Text(
                    '${device.seen} observations · $lastSeen',
                    style: const TextStyle(color: Colors.white54, fontSize: 10),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class FindMyMonitorScreen extends StatefulWidget {
  const FindMyMonitorScreen({super.key, required this.device});

  final BleWarDevice device;

  @override
  State<FindMyMonitorScreen> createState() => _FindMyMonitorScreenState();
}

class _FindMyMonitorScreenState extends State<FindMyMonitorScreen> {
  static const int _maxSamples = 60;
  final List<int> _samples = [];
  Timer? _ticker;
  DateTime? _lastSampleAt;
  bool _saving = false;

  @override
  void initState() {
    super.initState();
    _ticker = Timer.periodic(const Duration(seconds: 1), (_) {
      if (!mounted) return;
      final current = _currentDevice();
      if (current != null && current.lastSeen != _lastSampleAt) {
        _lastSampleAt = current.lastSeen;
        _samples.add(current.rssi);
        if (_samples.length > _maxSamples) _samples.removeAt(0);
      }
      setState(() {});
    });
  }

  @override
  void dispose() {
    _ticker?.cancel();
    super.dispose();
  }

  BleWarDevice? _currentDevice() {
    for (final device in context.read<DeviceController>().bleWarDevices) {
      if (device.addr == widget.device.addr) return device;
    }
    return null;
  }

  Future<void> _save() async {
    if (_saving) return;
    setState(() => _saving = true);
    try {
      final path = await context.read<DeviceController>().bleFindMySave(
        widget.device.addr,
      );
      if (!mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Saved to LittleFS: $path')));
    } catch (error) {
      if (!mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Save failed: $error')));
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final currentDevice = _currentDevice();
    final isLive = ctrl.bleWarRunning && currentDevice != null;
    final device = currentDevice ?? widget.device;
    final signal = _signalLabel(device.rssi);
    final samples = List<int>.unmodifiable(_samples);

    return Scaffold(
      appBar: AppBar(
        iconTheme: const IconThemeData(color: _kFindMyAccent),
        title: const Text(
          'LIVE MONITOR',
          style: TextStyle(
            color: _kFindMyAccent,
            fontWeight: FontWeight.bold,
            letterSpacing: 1.5,
            fontSize: 16,
          ),
        ),
      ),
      body: ListView(
        padding: const EdgeInsets.all(14),
        children: [
          Container(
            padding: const EdgeInsets.all(16),
            decoration: BoxDecoration(
              color: const Color(0xFF0A1020),
              borderRadius: BorderRadius.circular(14),
              border: Border.all(color: _kFindMyAccent.withValues(alpha: 0.28)),
            ),
            child: Row(
              children: [
                const Icon(
                  Icons.location_searching,
                  color: _kFindMyAccent,
                  size: 30,
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        device.name.isEmpty
                            ? 'AirTag-format candidate'
                            : device.name,
                        style: const TextStyle(
                          color: Colors.white,
                          fontSize: 17,
                          fontWeight: FontWeight.bold,
                        ),
                      ),
                      Text(
                        isLive ? 'LIVE · ${device.dtype}' : 'LAST SEEN',
                        style: TextStyle(
                          color: isLive ? _kFindMyAccent : Colors.white54,
                          fontSize: 10,
                          letterSpacing: 1,
                          fontWeight: FontWeight.bold,
                        ),
                      ),
                    ],
                  ),
                ),
                Icon(
                  isLive ? Icons.sensors : Icons.sensors_off,
                  color: isLive ? _kFindMyAccent : Colors.white38,
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          _section(
            title: 'SIGNAL MONITOR',
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  crossAxisAlignment: CrossAxisAlignment.end,
                  children: [
                    Text(
                      '${device.rssi}',
                      style: TextStyle(
                        color: _signalColor(device.rssi),
                        fontSize: 38,
                        height: 1,
                        fontWeight: FontWeight.w800,
                        fontFamily: 'monospace',
                      ),
                    ),
                    const Padding(
                      padding: EdgeInsets.only(left: 5, bottom: 4),
                      child: Text(
                        'dBm',
                        style: TextStyle(color: Colors.white70, fontSize: 13),
                      ),
                    ),
                    const Spacer(),
                    Text(
                      signal,
                      style: TextStyle(
                        color: _signalColor(device.rssi),
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 10),
                _SignalMeter(rssi: device.rssi, height: 8),
                const SizedBox(height: 12),
                const Text(
                  'Session RSSI history (candidate updates about every 2 seconds)',
                  style: TextStyle(color: Colors.white54, fontSize: 10),
                ),
                const SizedBox(height: 6),
                SizedBox(
                  height: 118,
                  width: double.infinity,
                  child: CustomPaint(painter: _RssiChartPainter(samples)),
                ),
                const Text(
                  'Distance cannot be estimated accurately from BLE RSSI. '
                  'The indicator above shows relative signal strength only.',
                  style: TextStyle(color: Colors.white54, fontSize: 11),
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          _section(
            title: 'IDENTITY & OBSERVATIONS',
            child: Column(
              children: [
                _infoRow('Current BLE address', device.addr, monospace: true),
                _infoRow('Company ID', 'Apple · 0x${device.company}'),
                _infoRow('Advertisement type', 'AirTag-format candidate'),
                _infoRow('Observations', '${device.seen}'),
                _infoRow('First seen', _formatTime(device.firstSeen)),
                _infoRow('Last seen', _formatTime(device.lastSeen)),
                _infoRow('Tracker location', 'Not available from BLE advertisements'),
              ],
            ),
          ),
          const SizedBox(height: 12),
          Container(
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(
              color: Colors.white.withValues(alpha: 0.035),
              borderRadius: BorderRadius.circular(10),
              border: Border.all(color: Colors.white12),
            ),
            child: const Text(
              'BLE addresses can change and do not prove a persistent identity. '
              'A matching payload may come from an AirTag or a compatible device '
              'using a similar format. The location shown here is not the '
              'tracker location.',
              style: TextStyle(
                color: Colors.white60,
                fontSize: 11,
                height: 1.45,
              ),
            ),
          ),
          const SizedBox(height: 14),
          Align(
            alignment: Alignment.centerLeft,
            child: ElevatedButton.icon(
              onPressed: isLive && !_saving ? _save : null,
              icon: _saving
                  ? const SizedBox(
                      width: 16,
                      height: 16,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : const Icon(Icons.save_outlined, size: 18),
              label: Text(_saving ? 'SAVING…' : 'SAVE TO LITTLEFS'),
              style: ElevatedButton.styleFrom(
                backgroundColor: _kFindMyAccent,
                foregroundColor: Colors.black,
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _section({required String title, required Widget child}) => Container(
    padding: const EdgeInsets.all(14),
    decoration: BoxDecoration(
      color: Colors.white.withValues(alpha: 0.025),
      borderRadius: BorderRadius.circular(12),
      border: Border.all(color: Colors.white12),
    ),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          title,
          style: const TextStyle(
            color: _kFindMyAccent,
            fontSize: 10,
            letterSpacing: 1.3,
            fontWeight: FontWeight.bold,
          ),
        ),
        const SizedBox(height: 12),
        child,
      ],
    ),
  );

  Widget _infoRow(String label, String value, {bool monospace = false}) =>
      Padding(
        padding: const EdgeInsets.symmetric(vertical: 5),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Expanded(
              flex: 4,
              child: Text(
                label,
                style: const TextStyle(color: Colors.white54, fontSize: 11),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              flex: 6,
              child: Text(
                value,
                textAlign: TextAlign.end,
                style: TextStyle(
                  color: Colors.white,
                  fontSize: 11,
                  fontFamily: monospace ? 'monospace' : null,
                ),
              ),
            ),
          ],
        ),
      );
}

class _SignalMeter extends StatelessWidget {
  const _SignalMeter({required this.rssi, this.height = 5});

  final int rssi;
  final double height;

  @override
  Widget build(BuildContext context) {
    final strength = ((rssi + 100) / 65).clamp(0.0, 1.0).toDouble();
    final color = _signalColor(rssi);
    return ClipRRect(
      borderRadius: BorderRadius.circular(height),
      child: LinearProgressIndicator(
        value: strength,
        minHeight: height,
        backgroundColor: Colors.white12,
        valueColor: AlwaysStoppedAnimation<Color>(color),
      ),
    );
  }
}

class _RssiChartPainter extends CustomPainter {
  _RssiChartPainter(this.samples);

  final List<int> samples;

  @override
  void paint(Canvas canvas, Size size) {
    final gridPaint = Paint()
      ..color = Colors.white.withValues(alpha: 0.08)
      ..strokeWidth = 1;
    final linePaint = Paint()
      ..color = _kFindMyAccent
      ..strokeWidth = 2
      ..style = PaintingStyle.stroke
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;

    for (var i = 1; i <= 3; i++) {
      final y = size.height * i / 4;
      canvas.drawLine(Offset(0, y), Offset(size.width, y), gridPaint);
    }
    if (samples.isEmpty) return;

    final path = Path();
    for (var i = 0; i < samples.length; i++) {
      final x = samples.length == 1
          ? size.width
          : size.width * i / (_maxSamplesForChart - 1);
      final normalized = ((samples[i] + 100) / 65).clamp(0.0, 1.0).toDouble();
      final y = size.height * (1 - normalized);
      if (i == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
    }
    canvas.drawPath(path, linePaint);
  }

  static const int _maxSamplesForChart = 60;

  @override
  bool shouldRepaint(covariant _RssiChartPainter oldDelegate) =>
      oldDelegate.samples != samples;
}

Color _signalColor(int rssi) {
  if (rssi >= -60) return const Color(0xFF52D6A3);
  if (rssi >= -75) return const Color(0xFFFFC34D);
  return const Color(0xFFFF805D);
}

String _signalLabel(int rssi) {
  if (rssi >= -60) return 'STRONG';
  if (rssi >= -75) return 'MEDIUM';
  return 'WEAK';
}

String _formatTime(DateTime time) {
  final local = time.toLocal();
  final h = local.hour.toString().padLeft(2, '0');
  final m = local.minute.toString().padLeft(2, '0');
  final s = local.second.toString().padLeft(2, '0');
  return '$h:$m:$s';
}
