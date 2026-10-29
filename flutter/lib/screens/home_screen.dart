import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart' show ScrollCacheExtent;
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../services/settings.dart';
import '../theme/app_theme.dart';
import '../widgets/device_issue_banner.dart';
import '../widgets/glass_surface.dart';
import 'ble_scan_screen.dart';
import 'ble_spam_screen.dart';
import 'ble_spectrum_screen.dart';
import 'ble_wardrive_screen.dart';
import 'deauth_all_screen.dart';
import 'esp_flasher_screen.dart';
import 'device_screen.dart';
import 'findmy_screen.dart';
import 'evil_portal_screen.dart';
import 'fs_browser_screen.dart';
import 'hardware_peripheral_screen.dart';
import 'sd_storage_screen.dart';
import 'ir_hub_screen.dart';
import 'led_screen.dart';
import 'nrf24_hub_screen.dart';
import 'spectrum_screen.dart';
import 'subghz_hub_screen.dart';
import 'wifi_live_scan_screen.dart';
import 'wifi_spam_screen.dart';

const _kAccent = AppColors.accent;

/// Home dashboard. Shows the BLE link, live CPU load, theme toggle, LED
/// control and the RF tool entries.
class HomeBody extends StatefulWidget {
  const HomeBody({super.key});

  @override
  State<HomeBody> createState() => _HomeBodyState();
}

class _HomeBodyState extends State<HomeBody> {
  String _fmtUptime(Duration d) {
    final h = d.inHours;
    final m = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final s = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    return h > 0 ? '${h}h $m:$s' : '$m:$s';
  }

  String _formatDataRate(double bytesPerSecond) {
    if (bytesPerSecond >= 1024) {
      return '${(bytesPerSecond / 1024).toStringAsFixed(1)} KB/s';
    }
    return '${bytesPerSecond.round()} B/s';
  }

  void _push(BuildContext context, DeviceController ctrl, Widget screen) {
    Navigator.of(context).push(
      AppPageRoute(
        builder: (_) =>
            ChangeNotifierProvider.value(value: ctrl, child: screen),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    // Read (not watch): the static tool rows below only need the controller to
    // push routes, so they must NOT rebuild on telemetry. Live cards subscribe
    // locally via Consumer so a ~20 Hz notification rebuilds only them.
    final ctrl = context.read<DeviceController>();
    final settings = context.watch<Settings>();

    // Lazy list: the children are held as cheap widget configs, but ListView
    // mounts each item's costly render objects (GlassCard gradients, shadows,
    // press controller) only as it scrolls into view — so opening the screen
    // does not build and raster ~25 cards on the very first frame.
    final items = <Widget>[
        // ── Live telemetry block ─────────────────────────────
        // Only this subtree is subscribed to DeviceController, so its frequent
        // notifications rebuild just these three cards — not the whole list —
        // which is what keeps scrolling smooth during a live connection.
        Consumer<DeviceController>(
          builder: (context, ctrl, _) {
            final connected = !ctrl.connectionLost;
            final name = ctrl.conn.device.platformName.isNotEmpty
                ? ctrl.conn.device.platformName
                : 'ESP32-C5';
            final uptime = _fmtUptime(
              DateTime.now().difference(ctrl.connectedAt),
            );
            final temperature = ctrl.chipTemperatureC;
            final temperatureAge = ctrl.chipTemperatureUpdatedAt == null
                ? null
                : DateTime.now().difference(ctrl.chipTemperatureUpdatedAt!);
            final temperatureIsFresh =
                temperatureAge != null &&
                temperatureAge < const Duration(milliseconds: 1500);
            final temperatureColor = temperature == null || !temperatureIsFresh
                ? Colors.white54
                : temperature < 35
                ? const Color(0xFF4FA8FF)
                : temperature < 44
                ? const Color(0xFFFFC53D)
                : const Color(0xFFFF5050);
            final sysAge = ctrl.sysUpdatedAt == null
                ? null
                : DateTime.now().difference(ctrl.sysUpdatedAt!);
            final sysIsFresh =
                sysAge != null && sysAge < const Duration(milliseconds: 1000);

            return Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                DeviceIssueBanner(controller: ctrl),

                // ── Link + CPU load hero card ────────────────────────
                GlassCard(
                  padding: const EdgeInsets.all(18),
                  radius: AppRadius.hero,
                  accent: _kAccent,
                  active: true,
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(
                        children: [
                          Icon(
                            connected
                                ? Icons.bluetooth_connected
                                : Icons.bluetooth_disabled,
                            color: connected
                                ? _kAccent
                                : const Color(0xFFFF5050),
                            size: 20,
                          ),
                          const SizedBox(width: 8),
                          Expanded(
                            child: Text(
                              name,
                              overflow: TextOverflow.ellipsis,
                              style: const TextStyle(
                                color: Colors.white,
                                fontSize: 16,
                                fontWeight: FontWeight.bold,
                              ),
                            ),
                          ),
                          LinkStatusChip(connected: connected),
                        ],
                      ),
                      const SizedBox(height: 4),
                      Text(
                        connected ? 'Connected · $uptime' : 'Disconnected',
                        style: const TextStyle(
                          color: Colors.white54,
                          fontSize: 12,
                        ),
                      ),
                      const SizedBox(height: 10),
                      Container(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 10,
                          vertical: 8,
                        ),
                        decoration: BoxDecoration(
                          color: Colors.black.withValues(alpha: 0.28),
                          borderRadius: BorderRadius.circular(8),
                        ),
                        child: Row(
                          children: [
                            const Icon(Icons.speed, color: _kAccent, size: 16),
                            const SizedBox(width: 7),
                            const Text(
                              'BLE RATE',
                              style: TextStyle(
                                color: Colors.white70,
                                fontSize: 10,
                                fontWeight: FontWeight.bold,
                                letterSpacing: 1,
                              ),
                            ),
                            const Spacer(),
                            Text(
                              connected
                                  ? 'RX ${_formatDataRate(ctrl.rxBytesPerSecond)}  ·  TX ${_formatDataRate(ctrl.txBytesPerSecond)}'
                                  : 'RX --  ·  TX --',
                              style: const TextStyle(
                                color: Colors.white,
                                fontSize: 11,
                                fontFamily: 'monospace',
                              ),
                            ),
                          ],
                        ),
                      ),
                      const SizedBox(height: 16),
                      Row(
                        crossAxisAlignment: CrossAxisAlignment.end,
                        children: [
                          Text(
                            sysAge == null ? '--' : '${ctrl.cpuUsagePercent}%',
                            style: const TextStyle(
                              color: _kAccent,
                              fontSize: 52,
                              height: 1,
                              fontWeight: FontWeight.w800,
                              fontFamily: 'monospace',
                            ),
                          ),
                          const SizedBox(width: 6),
                          Padding(
                            padding: const EdgeInsets.only(bottom: 8),
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                const Text(
                                  'CPU LOAD',
                                  style: TextStyle(
                                    color: Colors.white54,
                                    fontSize: 14,
                                  ),
                                ),
                                Text(
                                  sysAge == null
                                      ? 'WAITING'
                                      : sysIsFresh
                                      ? 'LIVE · ${sysAge.inMilliseconds} ms'
                                      : 'STALE · ${sysAge.inSeconds}s',
                                  style: TextStyle(
                                    color: sysIsFresh
                                        ? _kAccent
                                        : Colors.white38,
                                    fontSize: 9,
                                    fontWeight: FontWeight.bold,
                                    letterSpacing: 0.8,
                                  ),
                                ),
                              ],
                            ),
                          ),
                          const Spacer(),
                          Container(
                            padding: const EdgeInsets.symmetric(
                              horizontal: 10,
                              vertical: 6,
                            ),
                            decoration: BoxDecoration(
                              color: Colors.black.withValues(alpha: 0.4),
                              borderRadius: BorderRadius.circular(8),
                              border: Border.all(
                                color: _kAccent.withValues(alpha: 0.3),
                              ),
                            ),
                            child: Text(
                              ctrl.activity,
                              style: const TextStyle(
                                color: _kAccent,
                                fontSize: 12,
                                fontWeight: FontWeight.bold,
                                letterSpacing: 1.4,
                                fontFamily: 'monospace',
                              ),
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 12),
                      Wrap(
                        spacing: 6,
                        runSpacing: 6,
                        children: const [
                          _DeviceSpecBadge(
                            icon: Icons.wifi,
                            label: '2.4 + 5 GHz',
                          ),
                          _DeviceSpecBadge(
                            icon: Icons.memory,
                            label: 'RISC-V CPU',
                          ),
                          _DeviceSpecBadge(
                            icon: Icons.bluetooth,
                            label: 'NimBLE',
                          ),
                        ],
                      ),
                    ],
                  ),
                ),
                const SizedBox(height: 14),

                GlassCard(
                  padding: const EdgeInsets.all(16),
                  accent: temperatureColor,
                  active: true,
                  child: Row(
                    children: [
                      TweenAnimationBuilder<Color?>(
                        tween: ColorTween(end: temperatureColor),
                        duration: const Duration(milliseconds: 450),
                        curve: Curves.easeInOut,
                        builder: (context, color, child) =>
                            Icon(Icons.thermostat, color: color, size: 28),
                      ),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Text(
                              'CHIP TEMPERATURE',
                              style: TextStyle(
                                color: Colors.white70,
                                fontSize: 11,
                                fontWeight: FontWeight.bold,
                                letterSpacing: 1.3,
                              ),
                            ),
                            const SizedBox(height: 4),
                            Text(
                              temperatureAge == null
                                  ? 'Waiting for sensor data'
                                  : temperatureIsFresh
                                  ? 'LIVE · received ${temperatureAge.inMilliseconds} ms ago'
                                  : 'STALE · last received ${temperatureAge.inSeconds}s ago',
                              style: const TextStyle(
                                color: Colors.white54,
                                fontSize: 11,
                              ),
                            ),
                            const SizedBox(height: 3),
                            const Text(
                              'Die sensor · driver error <2°C (20–100°C) · 4 Hz',
                              style: TextStyle(
                                color: Colors.white38,
                                fontSize: 10,
                              ),
                            ),
                          ],
                        ),
                      ),
                      const SizedBox(width: 8),
                      AnimatedDefaultTextStyle(
                        duration: const Duration(milliseconds: 450),
                        curve: Curves.easeInOut,
                        style: TextStyle(
                          color: temperatureColor,
                          fontSize: 24,
                          fontWeight: FontWeight.w800,
                          fontFamily: 'monospace',
                        ),
                        child: Text(
                          temperature == null
                              ? '--.-°C'
                              : '${temperature.toStringAsFixed(1)}°C',
                        ),
                      ),
                    ],
                  ),
                ),
                const SizedBox(height: 14),

                // ── System memory / storage card ─────────────────────
                GlassCard(
                  padding: const EdgeInsets.all(16),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(
                        children: [
                          Icon(
                            Icons.memory,
                            color: sysIsFresh ? _kAccent : Colors.white54,
                            size: 20,
                          ),
                          const SizedBox(width: 10),
                          const Text(
                            'STORAGE',
                            style: TextStyle(
                              color: Colors.white70,
                              fontSize: 11,
                              fontWeight: FontWeight.bold,
                              letterSpacing: 1.3,
                            ),
                          ),
                          const Spacer(),
                          Text(
                            sysAge == null
                                ? 'Waiting…'
                                : sysIsFresh
                                ? 'LIVE'
                                : 'STALE ${sysAge.inSeconds}s',
                            style: TextStyle(
                              color: sysIsFresh ? _kAccent : Colors.white38,
                              fontSize: 10,
                              fontWeight: FontWeight.bold,
                              letterSpacing: 1.2,
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 12),
                      _MemoryRow(
                        label: 'RAM (internal)',
                        free: ctrl.sysHeapFree,
                        total: ctrl.sysHeapTotal,
                        color: AppColors.accent,
                        fresh: sysIsFresh,
                      ),
                      const SizedBox(height: 10),
                      _MemoryRow(
                        label: 'PSRAM',
                        free: ctrl.sysPsramFree,
                        total: ctrl.sysPsramTotal,
                        color: const Color(0xFF9B8CFF),
                        fresh: sysIsFresh,
                      ),
                      const SizedBox(height: 10),
                      _MemoryRow(
                        label: 'Flash (storage)',
                        free: ctrl.sysFlashFree,
                        total: ctrl.sysFlashTotal,
                        color: const Color(0xFF4FA8FF),
                        fresh: sysIsFresh,
                      ),
                    ],
                  ),
                ),
              ],
            );
          },
        ),
        const SizedBox(height: 12),

        // ── LittleFS file manager ────────────────────────────
        _ToolRow(
          icon: Icons.folder_open,
          color: const Color(0xFF4FA8FF),
          title: 'LittleFS Files',
          subtitle: 'Browse & delete stored files — IR signals, portal data',
          onTap: () => _push(context, ctrl, const FsBrowserScreen()),
        ),
        const SizedBox(height: 10),
        // SD card row shows live mount/transfer state → subscribe locally.
        Consumer<DeviceController>(
          builder: (context, ctrl, _) => _ToolRow(
            icon: ctrl.sdPresent ? Icons.sd_storage : Icons.help_outline,
            leadingWidget: ctrl.sdBusy
                ? const _SyncIndicator()
                : Icon(
                    ctrl.sdPresent ? Icons.sd_storage : Icons.help_outline,
                    color: ctrl.sdPresent
                        ? const Color(0xFF4FA8FF)
                        : Colors.amber,
                    size: 24,
                  ),
            color: ctrl.sdPresent ? const Color(0xFF4FA8FF) : Colors.amber,
            title: 'SD Card',
            subtitle: ctrl.sdBusy
                ? 'Synchronizing · ${ctrl.sdTransferSentBytes} bytes transferred'
                : ctrl.sdPresent
                ? ctrl.sdMounted
                      ? '${_fmtBytes(ctrl.sdFree)} free / ${_fmtBytes(ctrl.sdTotal)} · tap to browse'
                      : 'Card detected · tap to mount or inspect'
                : 'No card detected',
            onTap: () => _push(context, ctrl, const SdStorageScreen()),
          ),
        ),
        const SizedBox(height: 14),
        _ToolRow(
          icon: Icons.developer_board,
          color: const Color(0xFF63F2CE),
          title: 'Hardware Peripheral',
          subtitle:
              'ESP32-C5 pin map · peripherals · available GPIO · boot warnings',
          onTap: () => _push(context, ctrl, const HardwarePeripheralScreen()),
        ),
        const SizedBox(height: 14),
        _ToolRow(
          icon: Icons.memory,
          color: const Color(0xFF2EE6C5),
          title: 'ESP Flasher UART',
          subtitle: 'Serial flash, BOOT control and BIN transfer',
          onTap: () => _push(context, ctrl, const EspFlasherScreen()),
        ),
        const SizedBox(height: 14),

        // ── Theme + LED row ──────────────────────────────────
        Row(
          children: [
            Expanded(
              child: _TileCard(
                icon: settings.darkMode ? Icons.dark_mode : Icons.light_mode,
                color: const Color(0xFF9B8CFF),
                title: 'Theme',
                subtitle: settings.darkMode ? 'Dark' : 'Light',
                onTap: () => settings.setDarkMode(!settings.darkMode),
                trailing: Switch(
                  value: settings.darkMode,
                  activeThumbColor: const Color(0xFF9B8CFF),
                  onChanged: (v) => settings.setDarkMode(v),
                ),
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: _TileCard(
                icon: Icons.lightbulb_outline,
                color: const Color(0xFFFFB000),
                title: 'LED',
                subtitle: 'Color & effects',
                onTap: () => _push(context, ctrl, const LedScreen()),
              ),
            ),
          ],
        ),
        const SizedBox(height: 14),

        const SectionHeader('RF TOOLS'),
        _ToolRow(
          icon: Icons.wifi,
          color: const Color(0xFF4FA8FF),
          title: 'Network Target & Monitoring',
          subtitle: 'Scan nearby Wi-Fi networks and monitor signal details',
          onTap: () => _push(context, ctrl, const NetworkTargetScreen()),
        ),
        const SizedBox(height: 10),
        _ToolRow(
          icon: Icons.bluetooth_searching,
          color: const Color(0xFF00E5FF),
          title: 'BLE Scan',
          subtitle: 'Find nearby Bluetooth Low Energy devices',
          onTap: () => _push(context, ctrl, const BleScanScreen()),
        ),
        const SizedBox(height: 10),
        // Attack rows reflect live run state → subscribe locally.
        Consumer<DeviceController>(
          builder: (context, ctrl, _) {
            final running = ctrl.attacking && ctrl.attackMode == 'nuke';
            return _ToolRow(
              icon: Icons.bolt,
              color: Colors.red.shade400,
              title: 'Nuke',
              active: running,
              subtitle: running
                  ? 'RUNNING · tap to manage or stop'
                  : 'Launch the radar and manage its current operation',
              onTap: () => _push(context, ctrl, const NukeScreen()),
            );
          },
        ),
        const SizedBox(height: 10),
        Consumer<DeviceController>(
          builder: (context, ctrl, _) {
            final running = ctrl.attacking && ctrl.attackMode == 'deauthall';
            return _ToolRow(
              icon: Icons.wifi_tethering,
              color: const Color(0xFFFF4800),
              title: 'Deauth All',
              active: running,
              subtitle: running
                  ? 'RUNNING · tap to manage or stop'
                  : 'Manage channel-hopping operation and rescan options',
              onTap: () => _push(context, ctrl, const DeauthAllScreen()),
            );
          },
        ),
        const SizedBox(height: 10),
        Consumer<DeviceController>(
          builder: (context, ctrl, _) => _ToolRow(
            icon: Icons.bluetooth,
            color: const Color(0xFF39FF14),
            title: 'BLE Spam',
            active: ctrl.bleSpamRunning,
            subtitle: ctrl.bleSpamRunning
                ? 'RUNNING · ${ctrl.bleSpamType?.label ?? 'mode'}'
                : 'Choose and manage BLE advertising modes',
            onTap: () => _push(context, ctrl, const BleSpamScreen()),
          ),
        ),
        const SizedBox(height: 10),
        _ToolRow(
          icon: Icons.wifi_tethering,
          color: const Color(0xFFFF6A00),
          title: 'WiFi Spam',
          subtitle: 'Beacon flood — rickroll, funny, random, custom',
          onTap: () => _push(context, ctrl, const WifiSpamScreen()),
        ),
        const SizedBox(height: 10),
        _ToolRow(
          icon: Icons.travel_explore,
          color: const Color(0xFF4FA8FF),
          title: 'WiFi Live Scan',
          subtitle: 'Wardriving — SSID, BSSID, signal, distance',
          onTap: () => _push(context, ctrl, const WifiLiveScanScreen()),
        ),
        const SizedBox(height: 10),
        _ToolRow(
          icon: Icons.vpn_lock,
          color: const Color(0xFFFF5252),
          title: 'Evil Portal',
          subtitle: 'Captive portal — SoftAP, DNS hijack, credential capture',
          onTap: () => _push(context, ctrl, const EvilPortalScreen()),
        ),
        const SizedBox(height: 10),
        _ToolRow(
          icon: Icons.graphic_eq,
          color: const Color(0xFF39FF14),
          title: 'WiFi Spectrum',
          subtitle: '2.4 + 5 GHz channel energy, real-time',
          onTap: () => _push(context, ctrl, const SpectrumScreen()),
        ),
        const SizedBox(height: 10),
        _ToolRow(
          icon: Icons.bluetooth_audio,
          color: const Color(0xFF00E5FF),
          title: 'Bluetooth Spectrum',
          subtitle: 'Live nearby Bluetooth signal strength',
          onTap: () => _push(context, ctrl, const BleSpectrumScreen()),
        ),
        const SizedBox(height: 10),
        _ToolRow(
          icon: Icons.travel_explore,
          color: const Color(0xFF00E5FF),
          title: 'BLE Wardriving',
          subtitle: 'Live BLE devices — vendor, type, privacy, distance',
          onTap: () => _push(context, ctrl, const BleWardriveScreen()),
        ),
        const SizedBox(height: 10),
        _ToolRow(
          icon: Icons.location_searching,
          color: const Color(0xFF8EA6FF),
          title: 'Apple AirTag Monitor',
          subtitle: 'Live scan filtered by AirTag-format BLE advertisements',
          onTap: () => _push(context, ctrl, const FindMyScreen()),
        ),

        const SizedBox(height: 18),
        const SectionHeader('2.4 GHz RADIO · nRF24 MODULE'),
        _ToolRow(
          icon: Icons.settings_input_antenna,
          color: const Color(0xFFFF2D78),
          title: 'nRF24 Toolkit',
          subtitle: 'Scanner · jammer · sniffer · MouseJack',
          onTap: () => _push(context, ctrl, const Nrf24HubScreen()),
        ),

        const SizedBox(height: 18),
        const SectionHeader('INFRARED · 38 kHz'),
        _ToolRow(
          icon: Icons.settings_remote,
          color: const Color(0xFFFF5A52),
          title: 'Infrared',
          subtitle: 'Universal remote · receiver · spectrum · emulate',
          onTap: () => _push(context, ctrl, const IrHubScreen()),
        ),

        const SizedBox(height: 18),
        const SectionHeader('SUB-GHZ RADIO · CC1101 MODULE'),
        _ToolRow(
          icon: Icons.settings_input_antenna,
          color: const Color(0xFFFFB020),
          title: 'Sub-GHz Toolkit',
          subtitle: 'Read · Read RAW · Saved · Analyzer · Spectrum',
          onTap: () => _push(context, ctrl, const SubghzHubScreen()),
        ),
      ];
    return ListView.builder(
      padding: const EdgeInsets.all(14),
      // Build/raster a little beyond the viewport (default is 250 px) so a glass
      // card's first raster — gradient fill + sheen + shadow — happens just
      // ahead of a fast scroll rather than on the frame it becomes visible,
      // which is where the occasional reveal hitch came from. Kept modest: the
      // only telemetry-subscribed cards sit at the very top, so this does not
      // keep extra live cards rebuilding once the list is scrolled down.
      scrollCacheExtent: const ScrollCacheExtent.pixels(500),
      itemCount: items.length,
      itemBuilder: (context, index) => items[index],
    );
  }
}

class _DeviceSpecBadge extends StatelessWidget {
  const _DeviceSpecBadge({required this.icon, required this.label});

  final IconData icon;
  final String label;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.25),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: Colors.white.withValues(alpha: 0.10)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(icon, size: 12, color: _kAccent),
          const SizedBox(width: 5),
          Text(
            label,
            style: const TextStyle(
              color: Colors.white70,
              fontSize: 10,
              fontWeight: FontWeight.w600,
              letterSpacing: 0.3,
            ),
          ),
        ],
      ),
    );
  }
}

class _TileCard extends StatelessWidget {
  const _TileCard({
    required this.icon,
    required this.color,
    required this.title,
    required this.subtitle,
    required this.onTap,
    this.trailing,
  });

  final IconData icon;
  final Color color;
  final String title;
  final String subtitle;
  final VoidCallback onTap;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    return GlassCard(
      onTap: onTap,
      padding: const EdgeInsets.all(14),
      radius: AppRadius.tile,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(icon, color: color, size: 22),
              const Spacer(),
              ?trailing,
            ],
          ),
          const SizedBox(height: 10),
          Text(
            title,
            style: const TextStyle(
              color: AppColors.text,
              fontSize: 14,
              fontWeight: FontWeight.bold,
            ),
          ),
          const SizedBox(height: 2),
          Text(
            subtitle,
            style: const TextStyle(color: AppColors.textDim, fontSize: 12),
          ),
        ],
      ),
    );
  }
}

class _SyncIndicator extends StatefulWidget {
  const _SyncIndicator();

  @override
  State<_SyncIndicator> createState() => _SyncIndicatorState();
}

class _SyncIndicatorState extends State<_SyncIndicator>
    with SingleTickerProviderStateMixin {
  late final AnimationController _controller = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 900),
  )..repeat();

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return RotationTransition(
      turns: _controller,
      child: const Icon(Icons.sync, color: Color(0xFF4FA8FF), size: 24),
    );
  }
}

class _ToolRow extends StatelessWidget {
  const _ToolRow({
    required this.icon,
    this.leadingWidget,
    required this.color,
    required this.title,
    required this.subtitle,
    required this.onTap,
    this.active = false,
  });

  final IconData icon;
  final Widget? leadingWidget;
  final Color color;
  final String title;
  final String subtitle;
  final VoidCallback onTap;
  final bool active;

  @override
  Widget build(BuildContext context) {
    return GlassCard(
      onTap: onTap,
      padding: const EdgeInsets.all(14),
      radius: AppRadius.tile,
      accent: color,
      active: active,
      child: Row(
        children: [
          Container(
            width: 44,
            height: 44,
            decoration: BoxDecoration(
              color: color.withValues(alpha: 0.12),
              borderRadius: BorderRadius.circular(12),
            ),
            child: leadingWidget ?? Icon(icon, color: color, size: 24),
          ),
          const SizedBox(width: 14),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: const TextStyle(
                    color: AppColors.text,
                    fontSize: 15,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  subtitle,
                  style: const TextStyle(
                    color: AppColors.textDim,
                    fontSize: 12,
                  ),
                ),
              ],
            ),
          ),
          Icon(
            Icons.chevron_right,
            color: active ? color : AppColors.textFaint,
          ),
        ],
      ),
    );
  }
}

String _fmtBytes(int bytes) {
  if (bytes >= 1024 * 1024) {
    return '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MB';
  }
  if (bytes >= 1024) {
    return '${(bytes / 1024).toStringAsFixed(0)} KB';
  }
  return '$bytes B';
}

/// One memory/storage line: label, "free of total", and a used-fraction bar.
class _MemoryRow extends StatelessWidget {
  const _MemoryRow({
    required this.label,
    required this.free,
    required this.total,
    required this.color,
    required this.fresh,
  });

  final String label;
  final int free;
  final int total;
  final Color color;
  final bool fresh;

  @override
  Widget build(BuildContext context) {
    final present = total > 0;
    final used = present ? (total - free).clamp(0, total) : 0;
    final usedFraction = present ? used / total : 0.0;
    final barColor = fresh ? color : Colors.white30;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            Text(
              label,
              style: const TextStyle(color: Colors.white70, fontSize: 12),
            ),
            const Spacer(),
            Text(
              present
                  ? '${_fmtBytes(free)} free · ${_fmtBytes(total)}'
                  : 'not present',
              style: TextStyle(
                color: present
                    ? (fresh ? Colors.white : Colors.white54)
                    : Colors.white38,
                fontSize: 12,
                fontWeight: FontWeight.w600,
                fontFamily: 'monospace',
              ),
            ),
          ],
        ),
        const SizedBox(height: 5),
        ClipRRect(
          borderRadius: BorderRadius.circular(3),
          child: LinearProgressIndicator(
            value: present ? usedFraction : 0,
            minHeight: 5,
            backgroundColor: Colors.white.withValues(alpha: 0.08),
            valueColor: AlwaysStoppedAnimation<Color>(barColor),
          ),
        ),
      ],
    );
  }
}
