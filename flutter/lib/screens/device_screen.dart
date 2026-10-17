import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../models/acl_entry.dart';
import '../models/ap_entry.dart';
import '../services/device_controller.dart';
import '../widgets/device_issue_banner.dart';
import '../widgets/waterfall.dart';
import 'home_screen.dart';
import 'monitor_screen.dart';
import 'settings_screen.dart';

void _runDeviceAction(BuildContext context, Future<void> action) {
  unawaited(
    action.catchError((Object e, StackTrace s) {
      debugPrint('[device_action] failed: $e\n$s');
      if (!context.mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Device action failed: $e')));
    }),
  );
}

class DeviceScreen extends StatelessWidget {
  const DeviceScreen({super.key, required this.controller});

  final DeviceController controller;

  @override
  Widget build(BuildContext context) {
    return ChangeNotifierProvider.value(
      value: controller,
      child: const _DeviceView(),
    );
  }
}

class _DeviceView extends StatefulWidget {
  const _DeviceView();

  @override
  State<_DeviceView> createState() => _DeviceViewState();
}

class _DeviceViewState extends State<_DeviceView> {
  Future<bool> _confirmDisconnect(BuildContext context) async {
    final ctrl = context.read<DeviceController>();
    final result = await showDialog<bool>(
      context: context,
      builder: (_) => AlertDialog(
        title: const Text('Disconnect?'),
        content: Text(
          'Disconnect from ${ctrl.conn.device.platformName.isNotEmpty ? ctrl.conn.device.platformName : "device"}?',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: const Text('Stay'),
          ),
          TextButton(
            onPressed: () => Navigator.pop(context, true),
            child: const Text('Disconnect'),
          ),
        ],
      ),
    );
    return result ?? false;
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final name = ctrl.conn.device.platformName.isNotEmpty
        ? ctrl.conn.device.platformName
        : 'Connected';
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) async {
        if (didPop) return;
        final nav = Navigator.of(context);
        if (await _confirmDisconnect(context) && context.mounted) nav.pop();
      },
      child: Scaffold(
        appBar: AppBar(
          title: Text(name),
          actions: [
            IconButton(
              tooltip: 'Disconnect',
              icon: const Icon(Icons.bluetooth_disabled),
              onPressed: () async {
                final nav = Navigator.of(context);
                if (await _confirmDisconnect(context) && context.mounted) {
                  nav.pop();
                }
              },
            ),
            PopupMenuButton<String>(
              icon: const Icon(Icons.menu),
              onSelected: (value) {
                final ctrl = context.read<DeviceController>();
                switch (value) {
                  case 'console':
                    Navigator.of(context).push(
                      MaterialPageRoute(
                        builder: (_) => _ConsoleScreen(controller: ctrl),
                      ),
                    );
                  case 'whitelist':
                    Navigator.of(context).push(
                      MaterialPageRoute(
                        builder: (_) =>
                            _AclScreen(controller: ctrl, isWhitelist: true),
                      ),
                    );
                  case 'blacklist':
                    Navigator.of(context).push(
                      MaterialPageRoute(
                        builder: (_) =>
                            _AclScreen(controller: ctrl, isWhitelist: false),
                      ),
                    );
                  case 'settings':
                    Navigator.of(context).push(
                      MaterialPageRoute(builder: (_) => const SettingsScreen()),
                    );
                }
              },
              itemBuilder: (_) => const [
                PopupMenuItem(
                  value: 'console',
                  child: Row(
                    children: [
                      Icon(Icons.terminal, size: 18),
                      SizedBox(width: 10),
                      Text('Console'),
                    ],
                  ),
                ),
                PopupMenuDivider(),
                PopupMenuItem(
                  value: 'whitelist',
                  child: Row(
                    children: [
                      Icon(Icons.check_circle_outline, size: 18),
                      SizedBox(width: 10),
                      Text('Whitelist'),
                    ],
                  ),
                ),
                PopupMenuItem(
                  value: 'blacklist',
                  child: Row(
                    children: [
                      Icon(Icons.block, size: 18),
                      SizedBox(width: 10),
                      Text('Blacklist'),
                    ],
                  ),
                ),
                PopupMenuDivider(),
                PopupMenuItem(
                  value: 'settings',
                  child: Row(
                    children: [
                      Icon(Icons.settings, size: 18),
                      SizedBox(width: 10),
                      Text('Settings'),
                    ],
                  ),
                ),
              ],
            ),
          ],
        ),
        body: Column(
          children: [
            if (ctrl.connectionIssue != null)
              MaterialBanner(
                content: Text(ctrl.connectionIssue!),
                leading: Icon(
                  ctrl.connectionLost
                      ? Icons.bluetooth_disabled
                      : Icons.warning_amber,
                  color: Colors.red,
                ),
                actions: [
                  TextButton(
                    onPressed: ctrl.connectionLost
                        ? () => Navigator.of(context).pop()
                        : ctrl.dismissConnectionIssue,
                    child: Text(
                      ctrl.connectionLost ? 'BACK TO SCAN' : 'DISMISS',
                    ),
                  ),
                ],
              ),
              Expanded(
                child: const HomeBody(),
              ),
          ],
        ),
      ),
    );
  }
}

// ─── Console / ACL push-route screens ────────────────────────────────────────

class _ConsoleScreen extends StatelessWidget {
  const _ConsoleScreen({required this.controller});
  final DeviceController controller;

  @override
  Widget build(BuildContext context) {
    return ChangeNotifierProvider.value(
      value: controller,
      child: Scaffold(
        appBar: AppBar(title: const Text('Console')),
        body: const _ConsoleTab(),
      ),
    );
  }
}

class _AclScreen extends StatelessWidget {
  const _AclScreen({required this.controller, required this.isWhitelist});
  final DeviceController controller;
  final bool isWhitelist;

  @override
  Widget build(BuildContext context) {
    return ChangeNotifierProvider.value(
      value: controller,
      child: Scaffold(
        appBar: AppBar(title: Text(isWhitelist ? 'Whitelist' : 'Blacklist')),
        body: _AclTab(isWhitelist: isWhitelist),
      ),
    );
  }
}

// ─── Network target screen ───────────────────────────────────────────────────

class NetworkTargetScreen extends StatelessWidget {
  const NetworkTargetScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Network Target & Monitoring')),
      body: const _NetworkTargetBody(),
    );
  }
}

class _NetworkTargetBody extends StatefulWidget {
  const _NetworkTargetBody();

  @override
  State<_NetworkTargetBody> createState() => _NetworkTargetBodyState();
}

class _NetworkTargetBodyState extends State<_NetworkTargetBody> {
  Timer? _ticker;
  final TextEditingController _searchCtrl = TextEditingController();
  String _bandFilter = 'All';
  late final DeviceController _ctrl;
  Future<void> _entryTask = Future<void>.value();

  @override
  void initState() {
    super.initState();
    _ctrl = context.read<DeviceController>();
    _ctrl.clearNetworkScanState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      _entryTask = _prepareScreen();
      _runDeviceAction(context, _entryTask);
    });
  }

  Future<void> _prepareScreen() async {
    if (_ctrl.attacking) return;
    await _ctrl.resetNetworkScanForScreen();
    if (!mounted || _ctrl.attacking) return;
    await _ctrl.scanWifi();
  }

  Future<void> _cleanUpScreen() async {
    try {
      await _entryTask;
    } catch (error) {
      debugPrint('Network screen startup failed: $error');
    }
    _ctrl.clearNetworkScanState();
    if (_ctrl.attacking) return;
    try {
      await _ctrl.resetNetworkScanForScreen();
    } catch (error) {
      debugPrint('Network scan cleanup failed: $error');
    }
  }

  @override
  void dispose() {
    _ticker?.cancel();
    _ticker = null;
    _searchCtrl.dispose();
    _ctrl.clearNetworkScanState();
    unawaited(_cleanUpScreen());
    super.dispose();
  }

  void _syncTicker(bool isAttacking) {
    if (isAttacking && _ticker == null) {
      _ticker = Timer.periodic(const Duration(seconds: 1), (_) {
        if (mounted) setState(() {});
      });
    } else if (!isAttacking && _ticker != null) {
      _ticker!.cancel();
      _ticker = null;
    }
  }

  String _formatElapsed(Duration d) {
    final h = d.inHours;
    final m = d.inMinutes.remainder(60);
    final s = d.inSeconds.remainder(60);
    if (h > 0) return '${h}h ${m}m ${s}s';
    if (m > 0) return '${m}m ${s}s';
    return '${s}s';
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final isAttacking =
        ctrl.attacking &&
        ctrl.attackMode != 'nuke' &&
        ctrl.attackMode != 'deauthall';

    _syncTicker(isAttacking);

    final query = _searchCtrl.text.trim().toLowerCase();
    final visible =
        ctrl.aps.where((ap) {
          final matchesBand =
              _bandFilter == 'All' ||
              (_bandFilter == '2.4 GHz' && !ap.is5ghz) ||
              (_bandFilter == '5 GHz' && ap.is5ghz);
          final matchesQuery =
              query.isEmpty ||
              ap.ssid.toLowerCase().contains(query) ||
              ap.bssid.toLowerCase().contains(query) ||
              ap.channel.toString() == query;
          return matchesBand && matchesQuery;
        }).toList()..sort((a, b) {
          final strength = b.rssi.compareTo(a.rssi);
          if (strength != 0) return strength;
          return a.ssid.toLowerCase().compareTo(b.ssid.toLowerCase());
        });

    return Column(
      children: [
        _toolbar(context, ctrl, visible.length),
        if (isAttacking) _attackStatus(ctrl),
        Expanded(
          child: ctrl.aps.isEmpty
              ? _emptyAps(ctrl)
              : visible.isEmpty
              ? Center(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Icon(
                        Icons.filter_alt_off_rounded,
                        size: 34,
                        color: Theme.of(context).colorScheme.outline,
                      ),
                      const SizedBox(height: 8),
                      Text(
                        'No networks match this view',
                        style: Theme.of(context).textTheme.titleSmall,
                      ),
                      const SizedBox(height: 3),
                      Text(
                        'Try another band or search term.',
                        style: Theme.of(context).textTheme.bodySmall?.copyWith(
                          color: Theme.of(context).colorScheme.onSurfaceVariant,
                        ),
                      ),
                    ],
                  ),
                )
              : ListView.separated(
                  padding: const EdgeInsets.fromLTRB(12, 4, 12, 18),
                  itemCount: visible.length,
                  separatorBuilder: (_, _) => const SizedBox(height: 9),
                  itemBuilder: (_, i) => _apTile(context, ctrl, visible[i]),
                ),
        ),
      ],
    );
  }

  Widget _toolbar(
    BuildContext context,
    DeviceController ctrl,
    int visibleCount,
  ) {
    final isAttacking =
        ctrl.attacking &&
        ctrl.attackMode != 'nuke' &&
        ctrl.attackMode != 'deauthall';
    final colors = Theme.of(context).colorScheme;
    return Padding(
      padding: const EdgeInsets.fromLTRB(12, 12, 12, 8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(
                width: 42,
                height: 42,
                decoration: BoxDecoration(
                  color: colors.primary.withValues(alpha: 0.12),
                  borderRadius: BorderRadius.circular(14),
                  border: Border.all(
                    color: colors.primary.withValues(alpha: 0.28),
                  ),
                ),
                child: Icon(Icons.radar, color: colors.primary),
              ),
              const SizedBox(width: 11),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      'NETWORK TARGET & MONITORING',
                      style: Theme.of(context).textTheme.titleSmall?.copyWith(
                        fontWeight: FontWeight.w800,
                        letterSpacing: 0.6,
                        fontSize: 12,
                      ),
                    ),
                    Text(
                      '${ctrl.aps.length} found · $visibleCount shown · '
                      '${ctrl.selectedIdxs.length} selected',
                      style: Theme.of(context).textTheme.bodySmall?.copyWith(
                        color: colors.onSurfaceVariant,
                      ),
                    ),
                  ],
                ),
              ),
              FilledButton.tonalIcon(
                // Scanning retunes the radio; never allow it during an attack.
                onPressed: (ctrl.scanning || ctrl.attacking)
                    ? null
                    : () => _runDeviceAction(context, ctrl.scanWifi()),
                icon: ctrl.scanning
                    ? const SizedBox(
                        width: 15,
                        height: 15,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Icon(Icons.refresh, size: 18),
                label: Text(ctrl.scanning ? 'Scanning' : 'Scan'),
              ),
            ],
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              Expanded(
                child: SizedBox(
                  height: 44,
                  child: TextField(
                    controller: _searchCtrl,
                    onChanged: (_) => setState(() {}),
                    decoration: InputDecoration(
                      hintText: 'Find SSID, BSSID or channel',
                      prefixIcon: const Icon(Icons.search, size: 19),
                      suffixIcon: _searchCtrl.text.isEmpty
                          ? null
                          : IconButton(
                              tooltip: 'Clear search',
                              onPressed: () {
                                _searchCtrl.clear();
                                setState(() {});
                              },
                              icon: const Icon(Icons.close, size: 18),
                            ),
                      isDense: true,
                      filled: true,
                      fillColor: colors.surfaceContainerLow,
                      border: OutlineInputBorder(
                        borderRadius: BorderRadius.circular(14),
                        borderSide: BorderSide.none,
                      ),
                    ),
                  ),
                ),
              ),
              const SizedBox(width: 8),
              if (isAttacking)
                IconButton.filled(
                  tooltip: 'Stop attack',
                  style: IconButton.styleFrom(
                    backgroundColor: colors.errorContainer,
                    foregroundColor: colors.onErrorContainer,
                  ),
                  onPressed: () => _runDeviceAction(context, ctrl.stopAttack()),
                  icon: const Icon(Icons.stop),
                )
              else
                IconButton.filledTonal(
                  tooltip: 'Start attack on selected networks',
                  onPressed: ctrl.selectedIdxs.isEmpty
                      ? null
                      : () => _runDeviceAction(context, ctrl.startAttack()),
                  icon: const Icon(Icons.bolt),
                ),
            ],
          ),
          const SizedBox(height: 9),
          SingleChildScrollView(
            scrollDirection: Axis.horizontal,
            child: Row(
              children: [
                for (final band in const ['All', '2.4 GHz', '5 GHz'])
                  Padding(
                    padding: const EdgeInsets.only(right: 7),
                    child: ChoiceChip(
                      label: Text(band),
                      selected: _bandFilter == band,
                      onSelected: (_) => setState(() => _bandFilter = band),
                      visualDensity: VisualDensity.compact,
                    ),
                  ),
              ],
            ),
          ),
          if (ctrl.aps.isNotEmpty) ...[
            const SizedBox(height: 3),
            Row(
              children: [
                Icon(
                  Icons.info_outline,
                  size: 13,
                  color: colors.onSurfaceVariant,
                ),
                const SizedBox(width: 5),
                Expanded(
                  child: Text(
                    ctrl.scanning
                        ? 'Scan in progress · RSSI is signal level, not distance.'
                        : ctrl.scanIsStale
                        ? 'Snapshot older than 5 min · scan to refresh.'
                        : 'Snapshot under 5 min · RSSI is not a distance estimate.',
                    style: Theme.of(context).textTheme.labelSmall?.copyWith(
                      color: colors.onSurfaceVariant,
                    ),
                  ),
                ),
              ],
            ),
          ],
        ],
      ),
    );
  }

  Widget _attackStatus(DeviceController ctrl) {
    Duration elapsed = Duration.zero;
    if (ctrl.attackStartedAt != null) {
      elapsed = DateTime.now().difference(ctrl.attackStartedAt!);
    }
    return Container(
      width: double.infinity,
      color: Colors.red.shade900.withAlpha(180),
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      child: Text(
        'Attacking ${ctrl.attackAps} network${ctrl.attackAps == 1 ? '' : 's'}'
        '   ${_formatElapsed(elapsed)} elapsed',
        style: TextStyle(color: Colors.red.shade200, fontSize: 13),
      ),
    );
  }

  Widget _emptyAps(DeviceController ctrl) {
    final operationActive = ctrl.attacking;
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(
            Icons.wifi_find,
            size: 54,
            color: Theme.of(
              context,
            ).colorScheme.primary.withValues(alpha: 0.65),
          ),
          const SizedBox(height: 14),
          Text(
            ctrl.scanning
                ? 'Listening for nearby networks…'
                : operationActive
                ? 'Wi-Fi operation in progress'
                : 'No networks found yet',
          ),
          const SizedBox(height: 5),
          Text(
            ctrl.scanning
                ? 'Results appear as the radio discovers access points.'
                : operationActive
                ? 'Stop the current operation before starting a fresh scan.'
                : 'Run a scan to build a fresh local network map.',
            style: Theme.of(context).textTheme.bodySmall?.copyWith(
              color: Theme.of(context).colorScheme.onSurfaceVariant,
            ),
            textAlign: TextAlign.center,
          ),
          if (ctrl.scanning) ...[
            const SizedBox(height: 16),
            const CircularProgressIndicator(),
          ],
        ],
      ),
    );
  }

  Widget _apTile(BuildContext context, DeviceController ctrl, ApEntry ap) {
    final isSelected = ctrl.selectedIdxs.contains(ap.idx);
    final colors = Theme.of(context).colorScheme;
    final bandColor = ap.is5ghz ? const Color(0xFF9B7BFF) : colors.primary;
    final isAttacking = ctrl.attacking;
    final signal = ((ap.rssi + 100) / 60).clamp(0.0, 1.0);
    final signalColor = ap.rssi >= -60
        ? const Color(0xFF49C98B)
        : ap.rssi >= -75
        ? const Color(0xFFE0B44D)
        : colors.error;
    return Material(
      color: isSelected
          ? colors.primaryContainer.withValues(alpha: 0.38)
          : colors.surfaceContainerLow,
      borderRadius: BorderRadius.circular(18),
      child: InkWell(
        borderRadius: BorderRadius.circular(18),
        onTap: isAttacking
            ? null
            : () => _runDeviceAction(context, ctrl.toggleAp(ap.idx)),
        child: Container(
          padding: const EdgeInsets.fromLTRB(13, 12, 9, 12),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(18),
            border: Border.all(
              color: isSelected
                  ? colors.primary.withValues(alpha: 0.75)
                  : colors.outlineVariant.withValues(alpha: 0.55),
              width: isSelected ? 1.4 : 0.8,
            ),
          ),
          child: Row(
            children: [
              Container(
                width: 43,
                height: 43,
                decoration: BoxDecoration(
                  color: bandColor.withValues(alpha: 0.12),
                  borderRadius: BorderRadius.circular(14),
                ),
                child: Icon(Icons.wifi_rounded, color: bandColor),
              ),
              const SizedBox(width: 11),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Expanded(
                          child: Text(
                            ap.ssid.isNotEmpty ? ap.ssid : 'Hidden network',
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: Theme.of(context).textTheme.titleSmall
                                ?.copyWith(fontWeight: FontWeight.w700),
                          ),
                        ),
                        Container(
                          padding: const EdgeInsets.symmetric(
                            horizontal: 7,
                            vertical: 3,
                          ),
                          decoration: BoxDecoration(
                            color: bandColor.withValues(alpha: 0.12),
                            borderRadius: BorderRadius.circular(8),
                          ),
                          child: Text(
                            ap.is5ghz ? '5 GHz' : '2.4 GHz',
                            style: TextStyle(
                              color: bandColor,
                              fontSize: 10,
                              fontWeight: FontWeight.w700,
                            ),
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 3),
                    Text(
                      '${ap.bssid}   ·   CH ${ap.channel}',
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: Theme.of(context).textTheme.labelSmall?.copyWith(
                        color: colors.onSurfaceVariant,
                        fontFamily: 'monospace',
                      ),
                    ),
                    const SizedBox(height: 9),
                    Row(
                      children: [
                        Expanded(
                          child: ClipRRect(
                            borderRadius: BorderRadius.circular(8),
                            child: LinearProgressIndicator(
                              value: signal,
                              minHeight: 5,
                              backgroundColor: colors.surfaceContainerHighest,
                              color: signalColor,
                            ),
                          ),
                        ),
                        const SizedBox(width: 9),
                        Text(
                          '${ap.rssi} dBm',
                          style: TextStyle(
                            color: signalColor,
                            fontSize: 11,
                            fontWeight: FontWeight.w800,
                            fontFamily: 'monospace',
                          ),
                        ),
                      ],
                    ),
                  ],
                ),
              ),
              const SizedBox(width: 3),
              Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(
                    isSelected
                        ? Icons.check_circle
                        : Icons.radio_button_unchecked,
                    size: 19,
                    color: isSelected ? colors.primary : colors.outline,
                  ),
                  IconButton(
                    icon: const Icon(Icons.insights, size: 19),
                    color: colors.secondary,
                    tooltip: isAttacking
                        ? 'Stop attack to monitor'
                        : 'Monitor network',
                    onPressed: isAttacking
                        ? null
                        : () => _openMonitor(context, ctrl, ap),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }

  void _openMonitor(BuildContext context, DeviceController ctrl, ApEntry ap) {
    Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) => ChangeNotifierProvider.value(
          value: ctrl,
          child: MonitorScreen(controller: ctrl, ap: ap),
        ),
      ),
    );
  }
}

// ─── ACL tab (Whitelist / Blacklist) ─────────────────────────────────────────

class _AclTab extends StatefulWidget {
  const _AclTab({required this.isWhitelist});

  final bool isWhitelist;

  @override
  State<_AclTab> createState() => _AclTabState();
}

class _AclTabState extends State<_AclTab> {
  final _macCtrl = TextEditingController();
  String _kind = 'auto';

  @override
  void dispose() {
    _macCtrl.dispose();
    super.dispose();
  }

  List<AclEntry> _list(DeviceController ctrl) =>
      widget.isWhitelist ? ctrl.whitelist : ctrl.blacklist;

  Future<void> _add(DeviceController ctrl) async {
    final mac = _macCtrl.text.trim();
    if (mac.isEmpty) return;
    if (!RegExp(r'^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}$').hasMatch(mac)) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Enter a valid MAC address.')),
      );
      return;
    }
    if (widget.isWhitelist) {
      await ctrl.addToWhitelist(mac, _kind);
    } else {
      await ctrl.addToBlacklist(mac, _kind);
    }
    if (!mounted) return;
    _macCtrl.clear();
  }

  Future<void> _remove(DeviceController ctrl, String mac) async {
    if (widget.isWhitelist) {
      await ctrl.removeFromWhitelist(mac);
    } else {
      await ctrl.removeFromBlacklist(mac);
    }
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final entries = _list(ctrl);
    final accent = widget.isWhitelist ? Colors.teal : Colors.red;

    return Column(
      children: [
        DeviceIssueBanner(controller: ctrl),
        Padding(
          padding: const EdgeInsets.all(12),
          child: Row(
            children: [
              Expanded(
                child: TextField(
                  controller: _macCtrl,
                  decoration: InputDecoration(
                    labelText: 'MAC address (XX:XX:XX:XX:XX:XX)',
                    isDense: true,
                    border: const OutlineInputBorder(),
                    prefixIcon: Icon(
                      widget.isWhitelist
                          ? Icons.check_circle_outline
                          : Icons.block,
                      color: accent,
                    ),
                  ),
                  inputFormatters: [
                    FilteringTextInputFormatter.allow(RegExp(r'[0-9A-Fa-f:]')),
                    LengthLimitingTextInputFormatter(17),
                  ],
                ),
              ),
              const SizedBox(width: 8),
              DropdownButtonHideUnderline(
                child: DropdownButton<String>(
                  value: _kind,
                  items: const [
                    DropdownMenuItem(value: 'auto', child: Text('auto')),
                    DropdownMenuItem(value: 'bssid', child: Text('bssid')),
                    DropdownMenuItem(value: 'sta', child: Text('sta')),
                  ],
                  onChanged: (v) {
                    if (v != null) setState(() => _kind = v);
                  },
                ),
              ),
              const SizedBox(width: 8),
              ElevatedButton(
                onPressed: () => _runDeviceAction(context, _add(ctrl)),
                child: const Text('Add'),
              ),
            ],
          ),
        ),
        const Divider(height: 1),
        Expanded(
          child: entries.isEmpty
              ? Center(
                  child: Text(
                    '${widget.isWhitelist ? "Whitelist" : "Blacklist"} is empty',
                    style: const TextStyle(color: Colors.grey),
                  ),
                )
              : ListView.separated(
                  itemCount: entries.length,
                  separatorBuilder: (_, _) => const Divider(height: 1),
                  itemBuilder: (_, i) {
                    final e = entries[i];
                    return ListTile(
                      dense: true,
                      leading: Icon(
                        widget.isWhitelist
                            ? Icons.check_circle_outline
                            : Icons.block,
                        color: accent,
                      ),
                      title: Text(
                        e.mac,
                        style: const TextStyle(fontFamily: 'monospace'),
                      ),
                      subtitle: Text(e.kind),
                      trailing: IconButton(
                        icon: const Icon(Icons.delete_outline),
                        tooltip: 'Remove',
                        onPressed: () =>
                            _runDeviceAction(context, _remove(ctrl, e.mac)),
                      ),
                    );
                  },
                ),
        ),
      ],
    );
  }
}

// ─── Console tab ─────────────────────────────────────────────────────────────

class _ConsoleTab extends StatefulWidget {
  const _ConsoleTab();

  @override
  State<_ConsoleTab> createState() => _ConsoleTabState();
}

class _ConsoleTabState extends State<_ConsoleTab> {
  final _input = TextEditingController();
  final _scroll = ScrollController();
  String _prevLog = '';

  @override
  void dispose() {
    _input.dispose();
    _scroll.dispose();
    super.dispose();
  }

  void _scrollToBottom() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!_scroll.hasClients) return;
      _scroll.animateTo(
        _scroll.position.maxScrollExtent,
        duration: const Duration(milliseconds: 100),
        curve: Curves.easeOut,
      );
    });
  }

  Future<void> _send(BuildContext context, String cmd) async {
    cmd = cmd.trim();
    if (cmd.isEmpty) return;
    final ctrl = context.read<DeviceController>();
    try {
      await ctrl.send(cmd);
      if (!context.mounted) return;
      _input.clear();
    } catch (error) {
      if (!context.mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Command failed: $error')));
    }
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    if (ctrl.rawLog != _prevLog) {
      _prevLog = ctrl.rawLog;
      _scrollToBottom();
    }
    return Column(
      children: [
        DeviceIssueBanner(controller: ctrl),
        Expanded(
          child: Container(
            width: double.infinity,
            color: Colors.black,
            child: Scrollbar(
              controller: _scroll,
              child: SingleChildScrollView(
                controller: _scroll,
                padding: const EdgeInsets.all(8),
                child: SelectableText(
                  ctrl.rawLog,
                  style: const TextStyle(
                    color: Color(0xFF8FFF8F),
                    fontFamily: 'monospace',
                    fontSize: 12,
                    height: 1.3,
                  ),
                ),
              ),
            ),
          ),
        ),
        const Divider(height: 1),
        Padding(
          padding: const EdgeInsets.fromLTRB(8, 4, 8, 8),
          child: Row(
            children: [
              Expanded(
                child: TextField(
                  controller: _input,
                  decoration: const InputDecoration(
                    hintText: 'raw command',
                    isDense: true,
                    border: OutlineInputBorder(),
                  ),
                  textInputAction: TextInputAction.send,
                  onSubmitted: (v) => _send(context, v),
                  inputFormatters: [
                    FilteringTextInputFormatter.deny(RegExp(r'\n')),
                  ],
                ),
              ),
              const SizedBox(width: 8),
              ElevatedButton.icon(
                onPressed: () => _send(context, _input.text),
                icon: const Icon(Icons.send, size: 16),
                label: const Text('Send'),
              ),
              const SizedBox(width: 4),
              IconButton(
                tooltip: 'Clear log',
                icon: const Icon(Icons.clear_all),
                onPressed: () => context.read<DeviceController>().clearLog(),
              ),
            ],
          ),
        ),
      ],
    );
  }
}

class NukeScreen extends StatelessWidget {
  const NukeScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Nuke')),
      body: const _NukeBody(),
    );
  }
}

// ─── Nuke screen body ────────────────────────────────────────────────────────
class _NukeBody extends StatefulWidget {
  const _NukeBody();

  @override
  State<_NukeBody> createState() => _NukeBodyState();
}

class _NukeBodyState extends State<_NukeBody>
    with SingleTickerProviderStateMixin {
  final _durationCtrl = TextEditingController(text: '60');
  Timer? _countdownTicker;
  late AnimationController _radarCtrl;
  Offset? _inputPos;
  DeviceController? _ctrl;
  bool _reconcileScheduled = false;

  @override
  void initState() {
    super.initState();
    _radarCtrl = AnimationController(
      vsync: this,
      duration: const Duration(seconds: 30),
    )..repeat();
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ctrl = context.read<DeviceController>();
  }

  @override
  void dispose() {
    unawaited(_ctrl?.wlscanStop().catchError((Object _) {}));
    _countdownTicker?.cancel();
    _countdownTicker = null;
    _durationCtrl.dispose();
    _radarCtrl.dispose();
    super.dispose();
  }

  /// Coalesce start/stop decisions into one post-frame call so the live scan
  /// is never toggled from inside a build / notifyListeners dispatch.
  void _scheduleReconcile() {
    if (_reconcileScheduled) return;
    _reconcileScheduled = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _reconcileScheduled = false;
      _reconcileScan();
    });
  }

  /// Keep live capture active while this route is visible and no attack is
  /// pending. The firmware tears it down when an attack starts.
  void _reconcileScan() {
    if (!mounted) return;
    final ctrl = _ctrl;
    if (ctrl == null) return;
    // A nested route may own the radio while this route remains mounted.
    final onTop = ModalRoute.of(context)?.isCurrent ?? true;
    if (!onTop) return;
    final attackPending =
        ctrl.attacking ||
        ctrl.nukeStartedAt != null ||
        ctrl.deauthAllStartedAt != null;
    if (!attackPending && !ctrl.scanning && !ctrl.wlscanRunning) {
      unawaited(
        ctrl
            .wlscanStart(mode: 'balanced', intervalMs: 0)
            .catchError((Object _) {}),
      );
    }
  }

  /// APs to plot: the live-scan set while it is populated (so the radar tracks
  /// the airwaves in real time), falling back to the last one-shot scan.
  List<ApEntry> _radarAps(DeviceController ctrl) {
    if (ctrl.wlscanAps.isNotEmpty) {
      return [
        for (final w in ctrl.wlscanAps)
          ApEntry(
            idx: 0,
            channel: w.channel,
            is5ghz: w.is5ghz,
            rssi: w.rssi,
            bssid: w.bssid,
            ssid: w.ssid,
          ),
      ];
    }
    return ctrl.aps;
  }

  void _syncCountdown(bool isRunning) {
    if (isRunning && _countdownTicker == null) {
      _countdownTicker = Timer.periodic(const Duration(seconds: 1), (_) {
        if (mounted) setState(() {});
      });
    } else if (!isRunning && _countdownTicker != null) {
      _countdownTicker!.cancel();
      _countdownTicker = null;
    }
  }

  String _formatRemaining(Duration d) {
    if (d <= Duration.zero) return '0s';
    final h = d.inHours;
    final m = d.inMinutes.remainder(60);
    final s = d.inSeconds.remainder(60);
    if (h > 0) return '${h}h ${m}m';
    if (m > 0) return '${m}m ${s}s';
    return '${s}s';
  }

  String _fmtPkts(int n) {
    if (n >= 1000000) return '${(n / 1000000).toStringAsFixed(1)}M';
    if (n >= 1000) return '${(n / 1000).toStringAsFixed(1)}k';
    return '$n';
  }

  Widget _nukeStatusPanel({
    required Color accent,
    required String statusText,
    required bool isNuking,
    required bool isDeauthAll,
    required bool anyRunning,
    required int apCount,
    required Duration remaining,
    required int frames,
    required int cycles,
    required int currentChannel,
    required int nets,
  }) {
    final tiles = <Widget>[
      _NukeStat(label: 'NETS', value: '$nets', accent: accent),
      _NukeStat(label: 'FRAMES', value: _fmtPkts(frames), accent: accent),
    ];
    if (isDeauthAll) {
      tiles.add(
        _NukeStat(
          label: 'CH',
          value: currentChannel > 0 ? '$currentChannel' : '—',
          accent: accent,
        ),
      );
      tiles.add(_NukeStat(label: 'CYCLES', value: '$cycles', accent: accent));
    }
    if (anyRunning) {
      tiles.add(
        _NukeStat(
          label: 'LEFT',
          value: _formatRemaining(remaining),
          accent: accent,
        ),
      );
    }

    return Container(
      width: double.infinity,
      margin: const EdgeInsets.fromLTRB(12, 2, 12, 0),
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.55),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: accent.withValues(alpha: 0.4), width: 1.2),
        boxShadow: [
          BoxShadow(color: accent.withValues(alpha: 0.10), blurRadius: 16),
        ],
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(
                width: 10,
                height: 10,
                decoration: BoxDecoration(
                  color: accent,
                  shape: BoxShape.circle,
                  boxShadow: anyRunning
                      ? [BoxShadow(color: accent, blurRadius: 10)]
                      : null,
                ),
              ),
              const SizedBox(width: 10),
              Text(
                statusText,
                style: TextStyle(
                  color: accent,
                  fontSize: 20,
                  fontWeight: FontWeight.w800,
                  letterSpacing: 2.5,
                  fontFamily: 'monospace',
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              for (int i = 0; i < tiles.length; i++) ...[
                if (i > 0) const SizedBox(width: 10),
                Expanded(child: tiles[i]),
              ],
            ],
          ),
        ],
      ),
    );
  }

  /// Real-time air sniffer shown under the radar: a Marauder-style frame-rate
  /// monitor fed by the `wlscan: AIR` telemetry. Management + data frames are
  /// counted per second; a deauth/disassoc spike raises an attack alert.
  Widget _airSnifferPanel(DeviceController ctrl, {required bool live}) {
    final alert = live && ctrl.airAlert;
    final accent = alert
        ? const Color(0xFFFF2A2A)
        : (live ? const Color(0xFF39FF14) : Colors.white38);

    // Auto-scaling normalization for the sparkline (floor keeps idle flat).
    final hist = ctrl.airRateHistory;
    final peak = hist.isEmpty ? 1 : hist.reduce((a, b) => a > b ? a : b);
    final scale = (peak < 20 ? 20 : peak).toDouble();
    final values = <double>[
      for (final v in hist) (v / scale).clamp(0.0, 1.0).toDouble(),
    ];

    return Container(
      width: double.infinity,
      margin: const EdgeInsets.fromLTRB(12, 8, 12, 0),
      padding: const EdgeInsets.fromLTRB(12, 8, 12, 10),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.55),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: accent.withValues(alpha: 0.4), width: 1.2),
        boxShadow: alert
            ? [BoxShadow(color: accent.withValues(alpha: 0.30), blurRadius: 16)]
            : null,
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(
                alert ? Icons.warning_amber_rounded : Icons.sensors,
                size: 15,
                color: accent,
              ),
              const SizedBox(width: 6),
              Text(
                'AIR SNIFFER',
                style: TextStyle(
                  color: accent,
                  fontSize: 11,
                  fontWeight: FontWeight.bold,
                  letterSpacing: 1.6,
                  fontFamily: 'monospace',
                ),
              ),
              const Spacer(),
              Text(
                '${ctrl.airPps} f/s',
                style: TextStyle(
                  color: accent,
                  fontSize: 13,
                  fontWeight: FontWeight.bold,
                  fontFamily: 'monospace',
                ),
              ),
              const SizedBox(width: 8),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                decoration: BoxDecoration(
                  color: accent.withValues(alpha: 0.15),
                  borderRadius: BorderRadius.circular(6),
                  border: Border.all(color: accent.withValues(alpha: 0.4)),
                ),
                child: Text(
                  live ? 'MON' : 'PAUSED',
                  style: TextStyle(
                    color: accent,
                    fontSize: 9,
                    fontWeight: FontWeight.bold,
                    letterSpacing: 1,
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 6),
          SizedBox(
            height: 40,
            child: ClipRRect(
              borderRadius: BorderRadius.circular(6),
              child: values.isEmpty
                  ? Container(color: Colors.black.withValues(alpha: 0.4))
                  : SpectrumStrip(
                      values: values,
                      rev: ctrl.airRev,
                      accent: accent,
                    ),
            ),
          ),
          const SizedBox(height: 6),
          Row(
            children: [
              _airStat('MGMT', ctrl.airMgmtPps, Colors.tealAccent),
              _airStat('DATA', ctrl.airDataPps, Colors.lightBlueAccent),
              _airStat(
                'DEAUTH',
                ctrl.airDeauthPps,
                ctrl.airDeauthPps > 0
                    ? const Color(0xFFFF4040)
                    : Colors.white38,
              ),
              _airStat(
                'DISASC',
                ctrl.airDisassocPps,
                ctrl.airDisassocPps > 0
                    ? const Color(0xFFFF7040)
                    : Colors.white38,
              ),
              _airStat(
                'EAPOL',
                ctrl.airEapolPps,
                ctrl.airEapolPps > 0 ? const Color(0xFFFFD000) : Colors.white38,
              ),
            ],
          ),
          if (alert) ...[
            const SizedBox(height: 8),
            Container(
              width: double.infinity,
              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
              decoration: BoxDecoration(
                color: accent.withValues(alpha: 0.16),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: accent.withValues(alpha: 0.6)),
              ),
              child: Text(
                '⚠ POSSIBLE DEAUTH / DISASSOC FLOOD — AIR ATTACK NEARBY '
                '(${ctrl.airDeauthPps + ctrl.airDisassocPps}/s)',
                style: const TextStyle(
                  color: Color(0xFFFF6060),
                  fontSize: 10.5,
                  fontWeight: FontWeight.bold,
                  letterSpacing: 0.4,
                ),
              ),
            ),
          ],
        ],
      ),
    );
  }

  Widget _airStat(String label, int value, Color color) {
    return Expanded(
      child: Column(
        children: [
          FittedBox(
            fit: BoxFit.scaleDown,
            child: Text(
              '$value',
              style: TextStyle(
                color: color,
                fontSize: 15,
                fontWeight: FontWeight.bold,
                fontFamily: 'monospace',
              ),
            ),
          ),
          Text(
            label,
            style: TextStyle(
              color: Colors.white.withValues(alpha: 0.45),
              fontSize: 8,
              letterSpacing: 0.8,
            ),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    _scheduleReconcile();

    final isNuking = ctrl.attacking && ctrl.attackMode == 'nuke';
    final isDeauthAll = ctrl.attacking && ctrl.attackMode == 'deauthall';
    final isOtherAttack = ctrl.attacking && !isNuking && !isDeauthAll;
    final anyRunning = isNuking || isDeauthAll;
    final radarAps = _radarAps(ctrl);
    final apCount = radarAps.length;

    _syncCountdown(anyRunning);

    Duration remaining = Duration.zero;
    if (isNuking && ctrl.nukeStartedAt != null) {
      final elapsed = DateTime.now().difference(ctrl.nukeStartedAt!);
      final total = Duration(seconds: ctrl.nukeDurationSecs);
      final rem = total - elapsed;
      remaining = rem.isNegative ? Duration.zero : rem;
    } else if (isDeauthAll && ctrl.deauthAllStartedAt != null) {
      final elapsed = DateTime.now().difference(ctrl.deauthAllStartedAt!);
      final total = Duration(seconds: ctrl.deauthAllDurationSecs);
      final rem = total - elapsed;
      remaining = rem.isNegative ? Duration.zero : rem;
    }

    final statusText = isNuking
        ? 'NUKING'
        : isDeauthAll
        ? 'DEAUTH-ALL'
        : ctrl.wlscanRunning
        ? (apCount == 0 ? 'SCANNING…' : 'LIVE SCAN')
        : apCount == 0
        ? 'NO TARGETS'
        : 'TARGETS IN RANGE';

    final accent = anyRunning
        ? const Color(0xFFFF4800)
        : const Color(0xFF39FF14);

    return Column(
      children: [
        Expanded(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(12, 12, 12, 4),
            child: MouseRegion(
              onHover: (e) => setState(() => _inputPos = e.localPosition),
              onExit: (_) => setState(() => _inputPos = null),
              child: GestureDetector(
                onTapDown: (e) => setState(() => _inputPos = e.localPosition),
                child: AnimatedBuilder(
                  animation: _radarCtrl,
                  builder: (_, _) => CustomPaint(
                    painter: _RadarPainter(
                      sweepAngle: _radarCtrl.value * 2 * math.pi,
                      aps: radarAps,
                      isNuking: anyRunning,
                      inputPos: _inputPos,
                    ),
                    child: const SizedBox.expand(),
                  ),
                ),
              ),
            ),
          ),
        ),

        // ── Enlarged status panel (moved out of the radar overlay) ──
        _nukeStatusPanel(
          accent: accent,
          statusText: statusText,
          isNuking: isNuking,
          isDeauthAll: isDeauthAll,
          anyRunning: anyRunning,
          apCount: apCount,
          remaining: remaining,
          frames: isNuking
              ? ctrl.nukePkts
              : isDeauthAll
              ? ctrl.deauthAllPkts
              : 0,
          cycles: ctrl.deauthAllCycles,
          currentChannel: ctrl.deauthAllCurrentChannel,
          nets: isNuking ? ctrl.nukeApCount : apCount,
        ),

        // ── Air sniffer: real-time mgmt/data frame monitor + attack alert ──
        _airSnifferPanel(ctrl, live: ctrl.wlscanRunning),

        if (isOtherAttack)
          Container(
            color: Colors.orange.shade900.withAlpha(120),
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
            child: const Text(
              'Stop the current attack before launching Nuke / Deauth-All.',
              textAlign: TextAlign.center,
              style: TextStyle(fontSize: 12),
            ),
          ),

        Padding(
          padding: const EdgeInsets.fromLTRB(16, 4, 16, 16),
          child: Column(
            children: [
              if (!anyRunning) ...[
                Row(
                  children: [
                    SizedBox(
                      width: 110,
                      child: TextField(
                        controller: _durationCtrl,
                        keyboardType: TextInputType.number,
                        decoration: const InputDecoration(
                          labelText: 'Seconds',
                          isDense: true,
                          border: OutlineInputBorder(),
                        ),
                        inputFormatters: [
                          FilteringTextInputFormatter.digitsOnly,
                        ],
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
              ],

              if (isNuking)
                SizedBox(
                  width: double.infinity,
                  height: 48,
                  child: ElevatedButton.icon(
                    style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.red.shade800,
                    ),
                    onPressed: () =>
                        _runDeviceAction(context, ctrl.stopAttack()),
                    icon: const Icon(Icons.stop, size: 20),
                    label: const Text('Stop Nuke'),
                  ),
                )
              else if (isDeauthAll)
                SizedBox(
                  width: double.infinity,
                  height: 48,
                  child: ElevatedButton.icon(
                    style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.deepOrange.shade800,
                    ),
                    onPressed: () =>
                        _runDeviceAction(context, ctrl.stopAttack()),
                    icon: const Icon(Icons.stop, size: 20),
                    label: const Text('Stop Deauth-All'),
                  ),
                )
              else
                Row(
                  children: [
                    Expanded(
                      child: SizedBox(
                        height: 48,
                        child: ElevatedButton.icon(
                          style: ElevatedButton.styleFrom(
                            backgroundColor: (apCount == 0 || isOtherAttack)
                                ? null
                                : Colors.red.shade700,
                          ),
                          onPressed: (apCount == 0 || isOtherAttack)
                              ? null
                              : () {
                                  final parsed =
                                      int.tryParse(_durationCtrl.text) ?? 60;
                                  final secs = parsed.clamp(1, 3600);
                                  _runDeviceAction(context, ctrl.nuke(secs));
                                },
                          icon: const Icon(Icons.bolt, size: 20),
                          label: const Text('Nuke'),
                        ),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: SizedBox(
                        height: 48,
                        child: ElevatedButton.icon(
                          style: ElevatedButton.styleFrom(
                            backgroundColor: isOtherAttack
                                ? null
                                : Colors.deepOrange.shade700,
                          ),
                          onPressed: isOtherAttack
                              ? null
                              : () {
                                  final parsed =
                                      int.tryParse(_durationCtrl.text) ?? 60;
                                  final secs = parsed.clamp(1, 3600);
                                  _runDeviceAction(
                                    context,
                                    ctrl.deauthAll(secs),
                                  );
                                },
                          icon: const Icon(Icons.wifi_tethering, size: 20),
                          label: const Text('Deauth All'),
                        ),
                      ),
                    ),
                  ],
                ),
            ],
          ),
        ),
      ],
    );
  }
}

class _NukeStat extends StatelessWidget {
  const _NukeStat({
    required this.label,
    required this.value,
    required this.accent,
  });
  final String label;
  final String value;
  final Color accent;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 6),
      decoration: BoxDecoration(
        color: accent.withValues(alpha: 0.08),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: accent.withValues(alpha: 0.25)),
      ),
      child: Column(
        children: [
          FittedBox(
            fit: BoxFit.scaleDown,
            child: Text(
              value,
              style: TextStyle(
                color: accent,
                fontSize: 22,
                fontWeight: FontWeight.bold,
                fontFamily: 'monospace',
              ),
            ),
          ),
          const SizedBox(height: 2),
          Text(
            label,
            style: TextStyle(
              color: Colors.white.withValues(alpha: 0.55),
              fontSize: 10,
              letterSpacing: 1.5,
              fontFamily: 'monospace',
            ),
          ),
        ],
      ),
    );
  }
}

// ─── Military radar CustomPainter ────────────────────────────────────────────

class _RadarPainter extends CustomPainter {
  _RadarPainter({
    required this.sweepAngle,
    required this.aps,
    required this.isNuking,
    this.inputPos,
  });

  final double sweepAngle;
  final List<ApEntry> aps;
  final bool isNuking;
  final Offset? inputPos;

  static const _bg = Color(0xFF010D01);
  static const _ring = Color(0xFF0C3A0C);
  static const _phosphor = Color(0xFF39FF14);
  static const _dimGreen = Color(0xFF1B5C1B);
  static const _targetC = Color(0xFFFF4800);

  @override
  void paint(Canvas canvas, Size size) {
    final cx = size.width / 2;
    final cy = size.height / 2;
    final center = Offset(cx, cy);
    final r = math.min(cx, cy) - 2;

    var maxM = 10.0;
    for (final ap in aps) {
      final meters = math.pow(10.0, (-45.0 - ap.rssi) / 27.0).toDouble();
      if (meters > maxM) maxM = meters;
    }
    maxM = (maxM * 1.25).clamp(10.0, 200.0);

    final groups = <String, List<ApEntry>>{};
    for (final ap in aps) {
      groups.putIfAbsent(_normSsid(ap.ssid), () => []).add(ap);
    }
    for (final group in groups.values) {
      group.sort((a, b) => (a.is5ghz ? 1 : 0).compareTo(b.is5ghz ? 1 : 0));
    }
    const fanStep = 0.085;
    final angles = <String, double>{};
    for (final entry in groups.entries) {
      final members = entry.value;
      final base = _groupAngle(entry.key);
      for (var i = 0; i < members.length; i++) {
        angles[members[i].bssid] =
            base + (i - (members.length - 1) / 2.0) * fanStep;
      }
    }

    canvas.save();
    canvas.clipPath(
      Path()..addOval(Rect.fromCircle(center: center, radius: r)),
    );

    canvas.drawPaint(Paint()..color = _bg);

    final ringPaint = Paint()
      ..color = _ring
      ..style = PaintingStyle.stroke
      ..strokeWidth = 0.8;
    for (var i = 1; i <= 4; i++) {
      canvas.drawCircle(center, r * i / 4, ringPaint);
    }

    final xhPaint = Paint()
      ..color = _ring
      ..strokeWidth = 0.4;
    canvas.drawLine(Offset(cx - r, cy), Offset(cx + r, cy), xhPaint);
    canvas.drawLine(Offset(cx, cy - r), Offset(cx, cy + r), xhPaint);
    final diag = r / math.sqrt2;
    canvas.drawLine(
      Offset(cx - diag, cy - diag),
      Offset(cx + diag, cy + diag),
      xhPaint,
    );
    canvas.drawLine(
      Offset(cx + diag, cy - diag),
      Offset(cx - diag, cy + diag),
      xhPaint,
    );

    final tickPaint = Paint()
      ..color = _dimGreen
      ..strokeWidth = 0.9;
    for (int i = 0; i < 12; i++) {
      final a = i * math.pi / 6;
      canvas.drawLine(
        Offset(cx + (r - 8) * math.cos(a), cy + (r - 8) * math.sin(a)),
        Offset(cx + r * math.cos(a), cy + r * math.sin(a)),
        tickPaint,
      );
    }

    const trailSpan = math.pi * 0.65;
    final trailRect = Rect.fromCircle(center: center, radius: r);
    final trailPath = Path()
      ..moveTo(cx, cy)
      ..arcTo(trailRect, sweepAngle - trailSpan, trailSpan, false)
      ..close();
    canvas.drawPath(
      trailPath,
      Paint()
        ..shader = SweepGradient(
          startAngle: sweepAngle - trailSpan,
          endAngle: sweepAngle,
          colors: [Colors.transparent, _phosphor.withValues(alpha: 0.22)],
          tileMode: TileMode.clamp,
        ).createShader(trailRect),
    );

    String? highlightBssid;
    if (inputPos != null) {
      double best = 30.0;
      for (final ap in aps) {
        final a = angles[ap.bssid]!;
        final dr = _apRadius(ap.rssi, r, maxM);
        final d =
            (Offset(cx + dr * math.cos(a), cy + dr * math.sin(a)) - inputPos!)
                .distance;
        if (d < best) {
          best = d;
          highlightBssid = ap.bssid;
        }
      }
    }

    for (final ap in aps) {
      _paintAp(
        canvas,
        ap,
        angles[ap.bssid]!,
        center,
        r,
        maxM,
        ap.bssid == highlightBssid,
      );
    }

    canvas.drawLine(
      center,
      Offset(cx + r * math.cos(sweepAngle), cy + r * math.sin(sweepAngle)),
      Paint()
        ..color = _phosphor.withValues(alpha: 0.92)
        ..strokeWidth = 1.5,
    );

    canvas.drawCircle(center, 2.5, Paint()..color = _phosphor);
    canvas.drawCircle(
      center,
      6,
      Paint()
        ..color = _phosphor.withValues(alpha: 0.35)
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.0,
    );

    canvas.restore();
    _paintRangeLabels(canvas, center, r, maxM);
  }

  void _paintAp(
    Canvas canvas,
    ApEntry ap,
    double angle,
    Offset center,
    double maxR,
    double maxM,
    bool highlighted,
  ) {
    final dotR = _apRadius(ap.rssi, maxR, maxM);
    final pos = Offset(
      center.dx + dotR * math.cos(angle),
      center.dy + dotR * math.sin(angle),
    );

    final age = _pingAge(angle);
    final glow = math.max(0.0, 1.0 - age / 3.2);
    final col = isNuking ? _targetC : _phosphor;
    final dim = isNuking ? _targetC.withValues(alpha: 0.32) : _dimGreen;

    if (highlighted) {
      canvas.drawCircle(
        pos,
        16,
        Paint()
          ..color = col.withValues(alpha: 0.22)
          ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 8),
      );
    }

    if (glow > 0.04) {
      canvas.drawCircle(
        pos,
        10 * glow,
        Paint()
          ..color = col.withValues(alpha: 0.18 * glow)
          ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 5),
      );
    }

    canvas.drawCircle(
      pos,
      highlighted ? 4.5 : 2.5,
      Paint()..color = highlighted ? col : (glow > 0.04 ? col : dim),
    );

    final dist = _fmtDist(ap.rssi);
    final ssid = ap.ssid.length > 18
        ? '${ap.ssid.substring(0, 16)}..'
        : ap.ssid;
    final label = '$ssid · $dist';
    final fSize = highlighted ? 14.0 : 11.0;
    final fAlpha = highlighted
        ? 1.0
        : (glow > 0.04 ? 0.92 + 0.08 * glow : 0.72);
    final textPainter = TextPainter(
      text: TextSpan(
        text: label,
        style: TextStyle(
          color: (highlighted ? col : (glow > 0.04 ? col : dim)).withValues(
            alpha: fAlpha,
          ),
          fontSize: fSize,
          fontWeight: highlighted ? FontWeight.bold : FontWeight.w600,
        ),
      ),
      textDirection: TextDirection.ltr,
    )..layout(maxWidth: 160);
    textPainter.paint(
      canvas,
      Offset(pos.dx + 6, pos.dy - textPainter.height / 2),
    );
  }

  static String _fmtDist(int rssi) {
    final meters = math.pow(10.0, (-45.0 - rssi) / 27.0).toDouble();
    if (meters < 1.0) return '<1m';
    if (meters < 10.0) return '${meters.toStringAsFixed(1)}m';
    return '${meters.round()}m';
  }

  void _paintRangeLabels(Canvas canvas, Offset center, double r, double maxM) {
    final labels = [
      '${(maxM / 4).round()}m',
      '${(maxM / 2).round()}m',
      '${(maxM * 3 / 4).round()}m',
      '${maxM.round()}m',
    ];
    for (var i = 0; i < 4; i++) {
      final textPainter = TextPainter(
        text: TextSpan(
          text: labels[i],
          style: const TextStyle(color: _dimGreen, fontSize: 7),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      textPainter.paint(
        canvas,
        Offset(
          center.dx + 3,
          center.dy - (i + 1) / 4.0 * r - textPainter.height / 2,
        ),
      );
    }
  }

  static final _suffixRe = RegExp(
    r'[-_ ](5g(hz)?|2\.?4g(hz)?|2g(hz)?|6g(hz)?|5|2|ext(ender)?|plus|fast|iot|mesh|guest)$',
    caseSensitive: false,
  );

  static String _normSsid(String ssid) {
    var normalized = ssid.trim();
    String previous;
    do {
      previous = normalized;
      normalized = normalized.replaceAll(_suffixRe, '').trim();
    } while (normalized != previous && normalized.length > 2);
    return normalized.toLowerCase();
  }

  static double _groupAngle(String normalizedSsid) {
    var hash = 2166136261;
    for (final codeUnit in normalizedSsid.codeUnits) {
      hash ^= codeUnit;
      hash = (hash * 16777619) & 0xFFFFFFFF;
    }
    return (hash / 0xFFFFFFFF) * 2 * math.pi;
  }

  static double _apRadius(int rssi, double maxR, double maxM) {
    final meters = math.pow(10.0, (-45.0 - rssi) / 27.0).toDouble();
    return (meters.clamp(1.0, maxM) / maxM) * maxR;
  }

  double _pingAge(double dotAngle) {
    final sweep = ((sweepAngle % (2 * math.pi)) + 2 * math.pi) % (2 * math.pi);
    final dot = ((dotAngle % (2 * math.pi)) + 2 * math.pi) % (2 * math.pi);
    return ((sweep - dot + 2 * math.pi) % (2 * math.pi)) / (2 * math.pi) * 30.0;
  }

  @override
  bool shouldRepaint(_RadarPainter old) => true;
}
