import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/settings.dart';
import '../widgets/board_selector.dart';
import '../widgets/glass_surface.dart';

const _kPanel = Color(0xFF0D1D19);
const _kMint = Color(0xFF63F2CE);
const _kAmber = Color(0xFFFFC66D);
const _kViolet = Color(0xFFB5A5FF);
const _kMuted = Color(0xFF91AAA4);

class HardwarePeripheralScreen extends StatelessWidget {
  const HardwarePeripheralScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final board = context.watch<Settings>().board;
    final spec = board == BoardVariant.xiao ? _xiaoSpec : _devkitSpec;
    return Scaffold(
      backgroundColor: Colors.transparent,
      appBar: AppBar(
        backgroundColor: Colors.transparent,
        foregroundColor: Colors.white,
        title: const Text(
          'Hardware Peripheral',
          style: TextStyle(fontWeight: FontWeight.w700),
        ),
        actions: [
          Container(
            margin: const EdgeInsets.only(right: 14),
            padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 5),
            decoration: BoxDecoration(
              color: _kMint.withValues(alpha: 0.10),
              borderRadius: BorderRadius.circular(7),
              border: Border.all(color: _kMint.withValues(alpha: 0.30)),
            ),
            child: Text(
              spec.badge,
              style: const TextStyle(
                color: _kMint,
                fontFamily: 'monospace',
                fontSize: 10,
                fontWeight: FontWeight.bold,
                letterSpacing: 1,
              ),
            ),
          ),
        ],
      ),
      body: Stack(
        children: [
          const Positioned.fill(child: CustomPaint(painter: _CircuitGridPainter())),
          SafeArea(
            top: false,
            child: ListView(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 28),
              children: [
                BoardSelector(style: _boardSelectorStyle()),
                const SizedBox(height: 14),
                _BoardHero(spec: spec),
                const SizedBox(height: 24),
                const _SectionHeading(
                  eyebrow: 'FIRMWARE DEFAULTS',
                  title: 'Peripheral routing',
                  index: '01',
                ),
                const SizedBox(height: 10),
                ...spec.routes.map(
                  (route) => Padding(
                    padding: const EdgeInsets.only(bottom: 9),
                    child: _PeripheralCard(route: route),
                  ),
                ),
                const SizedBox(height: 15),
                const _SectionHeading(
                  eyebrow: 'AVAILABLE GPIO',
                  title: 'Available pins',
                  index: '02',
                ),
                const SizedBox(height: 10),
                _OpenPinsPanel(pins: spec.openPins),
                const SizedBox(height: 10),
                for (final caution in spec.cautions) ...[
                  caution,
                  const SizedBox(height: 8),
                ],
                const SizedBox(height: 16),
                const _SectionHeading(
                  eyebrow: 'PHYSICAL HEADER',
                  title: 'Physical pin map',
                  index: '03',
                ),
                const SizedBox(height: 10),
                _HeaderMapExpansion(groups: spec.headerGroups),
                const SizedBox(height: 14),
                Text(
                  spec.footerMain,
                  style: const TextStyle(
                      color: Colors.white38, fontSize: 11, height: 1.5),
                ),
                const SizedBox(height: 5),
                Text(
                  spec.footerRef,
                  style: const TextStyle(
                    color: _kMuted,
                    fontSize: 10,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

/// The Hardware Peripheral screen's themed look for the shared [BoardSelector].
ButtonStyle _boardSelectorStyle() {
  return ButtonStyle(
    backgroundColor: WidgetStateProperty.resolveWith(
      (states) => states.contains(WidgetState.selected)
          ? _kMint.withValues(alpha: 0.16)
          : Colors.white.withValues(alpha: 0.03),
    ),
    foregroundColor: WidgetStateProperty.resolveWith(
      (states) => states.contains(WidgetState.selected) ? _kMint : Colors.white70,
    ),
    side: WidgetStatePropertyAll(
      BorderSide(color: _kMint.withValues(alpha: 0.30)),
    ),
    textStyle: const WidgetStatePropertyAll(
      TextStyle(fontSize: 12, fontWeight: FontWeight.bold),
    ),
  );
}

const _peripheralRoutes = <_PeripheralRoute>[
  _PeripheralRoute(
    icon: Icons.alt_route,
    color: _kViolet,
    title: 'Shared SPI bus',
    subtitle: 'Shared bus for nRF24, CC1101, and microSD',
    note: 'microSD initializes before radio access is enabled.',
    pins: [
      _PinUse('SCK', 6),
      _PinUse('MISO', 2, warning: true),
      _PinUse('MOSI', 7, warning: true),
    ],
  ),
  _PeripheralRoute(
    icon: Icons.settings_input_antenna,
    color: Color(0xFF66B7FF),
    title: 'nRF24L01+',
    subtitle: 'Radio 2.4 GHz',
    note: 'Menggunakan shared SPI bus.',
    pins: [_PinUse('CSN', 9), _PinUse('CE', 8)],
  ),
  _PeripheralRoute(
    icon: Icons.settings_input_antenna,
    color: _kAmber,
    title: 'CC1101',
    subtitle: 'Radio sub-GHz',
    note: 'Menggunakan shared SPI bus.',
    pins: [_PinUse('CS', 10), _PinUse('GDO0', 26)],
  ),
  _PeripheralRoute(
    icon: Icons.settings_remote,
    color: Color(0xFFFF7770),
    title: 'Infrared',
    subtitle: 'Receiver and transmitter',
    note: 'RX receives signals; TX transmits infrared signals.',
    pins: [_PinUse('RX', 5), _PinUse('TX', 4)],
  ),
  _PeripheralRoute(
    icon: Icons.circle,
    color: Color(0xFFFFB44C),
    title: 'Onboard RGB LED',
    subtitle: 'Onboard status LED',
    note: 'GPIO27 is also an ESP32-C5 strapping pin.',
    pins: [_PinUse('DATA', 27, warning: true)],
  ),
  _PeripheralRoute(
    icon: Icons.usb_rounded,
    color: _kMint,
    title: 'UART0',
    subtitle: 'UART on the board header',
    note: 'UART0 connects to the board’s USB-to-UART bridge.',
    pins: [_PinUse('TX', 11), _PinUse('RX', 12)],
  ),
  _PeripheralRoute(
    icon: Icons.sd_storage,
    color: Color(0xFF84AFFF),
    title: 'microSD',
    subtitle: 'Chip select and card detection',
    note: 'Data lines use the shared SPI bus.',
    pins: [_PinUse('CS', 1), _PinUse('DETECT', 0)],
  ),
];

class _BoardHero extends StatelessWidget {
  const _BoardHero({required this.spec});

  final _BoardSpec spec;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(18),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(22),
        gradient: const LinearGradient(
          colors: [Color(0xFF12382E), Color(0xFF0B1A19), Color(0xFF10162B)],
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
        ),
        border: Border.all(color: _kMint.withValues(alpha: 0.30)),
        boxShadow: [
          BoxShadow(
            color: _kMint.withValues(alpha: 0.08),
            blurRadius: 28,
            offset: const Offset(0, 12),
          ),
        ],
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text(
                      'BOARD MAP  /  01',
                      style: TextStyle(
                        color: _kMint,
                        fontFamily: 'monospace',
                        fontSize: 10,
                        fontWeight: FontWeight.bold,
                        letterSpacing: 1.7,
                      ),
                    ),
                    const SizedBox(height: 8),
                    Text(
                      spec.heroTitle,
                      style: const TextStyle(
                        color: Colors.white,
                        fontSize: 27,
                        height: 1,
                        fontWeight: FontWeight.w800,
                        letterSpacing: -0.7,
                      ),
                    ),
                    const SizedBox(height: 7),
                    Text(
                      spec.heroSubtitle,
                      style: TextStyle(
                        color: Colors.white.withValues(alpha: 0.68),
                        fontSize: 11,
                      ),
                    ),
                  ],
                ),
              ),
              const _BoardStamp(),
            ],
          ),
          const SizedBox(height: 20),
          Row(children: _metricRow(spec.metrics)),
          const SizedBox(height: 14),
          Row(
            children: [
              Icon(Icons.info_outline, color: Colors.white.withValues(alpha: 0.58), size: 14),
              const SizedBox(width: 7),
              Expanded(
                child: Text(
                  spec.heroNote,
                  style: TextStyle(
                    color: Colors.white.withValues(alpha: 0.62),
                    fontSize: 10,
                    height: 1.35,
                  ),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  static List<Widget> _metricRow(List<_Metric> metrics) {
    final out = <Widget>[];
    for (var i = 0; i < metrics.length; i++) {
      if (i > 0) out.add(const SizedBox(width: 8));
      out.add(Expanded(child: metrics[i]));
    }
    return out;
  }
}

class _BoardStamp extends StatelessWidget {
  const _BoardStamp();

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 72,
      height: 72,
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.22),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: _kMint.withValues(alpha: 0.35)),
      ),
      child: Stack(
        alignment: Alignment.center,
        children: [
          Container(
            width: 42,
            height: 42,
            decoration: BoxDecoration(
              color: _kMint.withValues(alpha: 0.10),
              borderRadius: BorderRadius.circular(10),
              border: Border.all(color: _kMint.withValues(alpha: 0.45)),
            ),
            child: const Icon(Icons.memory, color: _kMint, size: 25),
          ),
          Positioned(
            top: 8,
            right: 9,
            child: Container(
              width: 5,
              height: 5,
              decoration: const BoxDecoration(color: _kAmber, shape: BoxShape.circle),
            ),
          ),
          Positioned(
            bottom: 9,
            left: 8,
            child: Container(
              width: 14,
              height: 2,
              color: _kMint.withValues(alpha: 0.70),
            ),
          ),
        ],
      ),
    );
  }
}

class _Metric extends StatelessWidget {
  const _Metric({required this.value, required this.label, required this.color});

  final String value;
  final String label;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 9),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.20),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white.withValues(alpha: 0.08)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            value,
            style: TextStyle(
              color: color,
              fontFamily: 'monospace',
              fontSize: 22,
              height: 1,
              fontWeight: FontWeight.w800,
            ),
          ),
          const SizedBox(height: 5),
          Text(
            label,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(
              color: Colors.white54,
              fontSize: 8,
              fontWeight: FontWeight.bold,
              letterSpacing: 1.1,
            ),
          ),
        ],
      ),
    );
  }
}

class _SectionHeading extends StatelessWidget {
  const _SectionHeading({required this.eyebrow, required this.title, required this.index});

  final String eyebrow;
  final String title;
  final String index;

  @override
  Widget build(BuildContext context) {
    return Row(
      crossAxisAlignment: CrossAxisAlignment.end,
      children: [
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                eyebrow,
                style: const TextStyle(
                  color: _kMint,
                  fontFamily: 'monospace',
                  fontSize: 9,
                  fontWeight: FontWeight.bold,
                  letterSpacing: 1.7,
                ),
              ),
              const SizedBox(height: 3),
              Text(
                title,
                style: const TextStyle(
                  color: Colors.white,
                  fontSize: 19,
                  fontWeight: FontWeight.w800,
                ),
              ),
            ],
          ),
        ),
        Text(
          index,
          style: const TextStyle(
            color: Colors.white24,
            fontFamily: 'monospace',
            fontSize: 17,
            fontWeight: FontWeight.w700,
          ),
        ),
      ],
    );
  }
}

class _PeripheralRoute {
  const _PeripheralRoute({
    required this.icon,
    required this.color,
    required this.title,
    required this.subtitle,
    required this.note,
    required this.pins,
  });

  final IconData icon;
  final Color color;
  final String title;
  final String subtitle;
  final String note;
  final List<_PinUse> pins;
}

class _PinUse {
  const _PinUse(this.signal, this.gpio, {this.warning = false});

  final String signal;
  final int gpio;
  final bool warning;
}

class _PeripheralCard extends StatelessWidget {
  const _PeripheralCard({required this.route});

  final _PeripheralRoute route;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: glassSurfaceDecoration(
        context,
        borderRadius: BorderRadius.circular(16),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(
                width: 38,
                height: 38,
                decoration: BoxDecoration(
                  color: route.color.withValues(alpha: 0.12),
                  borderRadius: BorderRadius.circular(11),
                ),
                child: Icon(route.icon, color: route.color, size: 20),
              ),
              const SizedBox(width: 11),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      route.title,
                      style: const TextStyle(
                        color: Colors.white,
                        fontSize: 14,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                    const SizedBox(height: 2),
                    Text(
                      route.subtitle,
                      style: const TextStyle(color: Colors.white54, fontSize: 10),
                    ),
                  ],
                ),
              ),
              Icon(Icons.arrow_outward, color: route.color.withValues(alpha: 0.65), size: 15),
            ],
          ),
          const SizedBox(height: 12),
          Wrap(
            spacing: 7,
            runSpacing: 7,
            children: route.pins.map((pin) => _PinBadge(pin: pin)).toList(),
          ),
          const SizedBox(height: 9),
          Text(
            route.note,
            style: const TextStyle(color: Colors.white38, fontSize: 10, height: 1.4),
          ),
        ],
      ),
    );
  }
}

class _PinBadge extends StatelessWidget {
  const _PinBadge({required this.pin});

  final _PinUse pin;

  @override
  Widget build(BuildContext context) {
    final color = pin.warning ? _kAmber : _kMint;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 6),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.08),
        borderRadius: BorderRadius.circular(9),
        border: Border.all(color: color.withValues(alpha: 0.23)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Text(
            pin.signal,
            style: TextStyle(
              color: color.withValues(alpha: 0.85),
              fontSize: 9,
              fontWeight: FontWeight.bold,
            ),
          ),
          const SizedBox(width: 7),
          Text(
            'GPIO${pin.gpio}',
            style: const TextStyle(
              color: Colors.white,
              fontFamily: 'monospace',
              fontSize: 11,
              fontWeight: FontWeight.w700,
            ),
          ),
          if (pin.warning) ...[
            const SizedBox(width: 4),
            const Icon(Icons.warning_amber_rounded, color: _kAmber, size: 12),
          ],
        ],
      ),
    );
  }
}

class _OpenPinsPanel extends StatelessWidget {
  const _OpenPinsPanel({required this.pins});

  final List<_OpenPin> pins;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(13),
      decoration: glassSurfaceDecoration(
        context,
        borderRadius: BorderRadius.circular(17),
      ),
      child: LayoutBuilder(
        builder: (context, constraints) {
          final itemWidth = (constraints.maxWidth - 8) / 2;
          return Wrap(
            spacing: 8,
            runSpacing: 8,
            children: pins
                .map((pin) => _OpenPinCard(pin: pin, width: itemWidth))
                .toList(),
          );
        },
      ),
    );
  }
}

class _OpenPin {
  const _OpenPin(this.gpio, this.note, {this.caution = false});

  final int gpio;
  final String note;
  final bool caution;
}

class _OpenPinCard extends StatelessWidget {
  const _OpenPinCard({required this.pin, required this.width});

  final _OpenPin pin;
  final double width;

  @override
  Widget build(BuildContext context) {
    final color = pin.caution ? _kAmber : _kMint;
    return Container(
      width: width,
      constraints: const BoxConstraints(minHeight: 59),
      padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 8),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.06),
        borderRadius: BorderRadius.circular(11),
        border: Border.all(color: color.withValues(alpha: 0.21)),
      ),
      child: Row(
        children: [
          Icon(
            pin.caution ? Icons.warning_amber_rounded : Icons.add_circle_outline,
            size: 15,
            color: color,
          ),
          const SizedBox(width: 7),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Text(
                  'GPIO${pin.gpio}',
                  style: const TextStyle(
                    color: Colors.white,
                    fontFamily: 'monospace',
                    fontSize: 12,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  pin.note,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(color: color.withValues(alpha: 0.82), fontSize: 9),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _CautionNote extends StatelessWidget {
  const _CautionNote({
    required this.color,
    required this.icon,
    required this.title,
    required this.body,
  });

  final Color color;
  final IconData icon;
  final String title;
  final String body;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(13),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.07),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: color.withValues(alpha: 0.26)),
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(icon, color: color, size: 17),
          const SizedBox(width: 9),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: TextStyle(
                    color: color,
                    fontFamily: 'monospace',
                    fontSize: 9,
                    fontWeight: FontWeight.bold,
                    letterSpacing: 0.8,
                  ),
                ),
                const SizedBox(height: 5),
                Text(
                  body,
                  style: const TextStyle(color: Colors.white70, fontSize: 10, height: 1.5),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _HeaderGroup {
  const _HeaderGroup(this.title, this.pins);

  final String title;
  final List<_HeaderPin> pins;
}

class _HeaderMapExpansion extends StatelessWidget {
  const _HeaderMapExpansion({required this.groups});

  final List<_HeaderGroup> groups;

  @override
  Widget build(BuildContext context) {
    final children = <Widget>[];
    for (var i = 0; i < groups.length; i++) {
      if (i > 0) children.add(const SizedBox(height: 10));
      children.add(_HeaderPinMap(title: groups[i].title, pins: groups[i].pins));
    }
    return Container(
      decoration: BoxDecoration(
        color: _kPanel.withValues(alpha: 0.97),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: _kMint.withValues(alpha: 0.20)),
      ),
      child: Theme(
        data: Theme.of(context).copyWith(dividerColor: Colors.transparent),
        child: ExpansionTile(
          tilePadding: const EdgeInsets.symmetric(horizontal: 14, vertical: 3),
          childrenPadding: const EdgeInsets.fromLTRB(12, 0, 12, 13),
          iconColor: _kMint,
          collapsedIconColor: _kMuted,
          title: const Text(
            'Open physical pin map',
            style: TextStyle(color: Colors.white, fontSize: 13, fontWeight: FontWeight.bold),
          ),
          subtitle: const Text(
            'Header/pad · GPIO · board function',
            style: TextStyle(color: Colors.white54, fontSize: 10),
          ),
          children: children,
        ),
      ),
    );
  }
}

class _HeaderPin {
  const _HeaderPin(
    this.position,
    this.label,
    this.role, {
    this.inUse = false,
    this.warning = false,
    this.blocked = false,
  });

  final int position;
  final String label;
  final String role;
  final bool inUse;
  final bool warning;
  final bool blocked;
}

const _j1Pins = <_HeaderPin>[
  _HeaderPin(1, '3V3', 'Power', blocked: true),
  _HeaderPin(2, 'RST', 'Active-low reset', blocked: true),
  _HeaderPin(3, 'GPIO2', 'SPI MISO · strap', inUse: true, warning: true),
  _HeaderPin(4, 'GPIO3', 'Unused · strap / MTDI', warning: true),
  _HeaderPin(5, 'GPIO0', 'microSD detect', inUse: true),
  _HeaderPin(6, 'GPIO1', 'microSD CS', inUse: true),
  _HeaderPin(7, 'GPIO6', 'SPI SCK', inUse: true),
  _HeaderPin(8, 'GPIO7', 'SPI MOSI · strap', inUse: true, warning: true),
  _HeaderPin(9, 'GPIO8', 'nRF24 CE', inUse: true),
  _HeaderPin(10, 'GPIO9', 'nRF24 CSN', inUse: true),
  _HeaderPin(11, 'GPIO10', 'CC1101 CS', inUse: true),
  _HeaderPin(12, 'GPIO26', 'CC1101 GDO0 · strap', inUse: true, warning: true),
  _HeaderPin(13, 'GPIO25', 'Unused · strap', warning: true),
  _HeaderPin(14, '5V', 'Power', blocked: true),
  _HeaderPin(15, 'GND', 'Ground', blocked: true),
  _HeaderPin(16, 'NC', 'Not connected', blocked: true),
];

const _j3Pins = <_HeaderPin>[
  _HeaderPin(1, 'GND', 'Ground', blocked: true),
  _HeaderPin(2, 'GPIO11', 'UART0 TX', inUse: true),
  _HeaderPin(3, 'GPIO12', 'UART0 RX', inUse: true),
  _HeaderPin(4, 'GPIO24', 'Unused', warning: false),
  _HeaderPin(5, 'GPIO23', 'ESP flasher BOOT', inUse: true),
  _HeaderPin(6, 'NC/15', 'PSRAM internal · unavailable', blocked: true),
  _HeaderPin(7, 'GPIO27', 'RGB LED · strap', inUse: true, warning: true),
  _HeaderPin(8, 'GPIO4', 'Infrared TX', inUse: true),
  _HeaderPin(9, 'GPIO5', 'Infrared RX', inUse: true),
  _HeaderPin(10, 'NC', 'Not connected', blocked: true),
  _HeaderPin(11, 'GPIO28', 'Unused · strap', warning: true),
  _HeaderPin(12, 'GND', 'Ground', blocked: true),
  _HeaderPin(13, 'GPIO14', 'Unused · USB D+ / SDIO2', warning: true),
  _HeaderPin(14, 'GPIO13', 'Unused · USB D− / SDIO3', warning: true),
  _HeaderPin(15, 'GND', 'Ground', blocked: true),
  _HeaderPin(16, 'NC', 'Not connected', blocked: true),
];

// ── Board specifications ─────────────────────────────────────────────────────

/// All board-specific data rendered by [HardwarePeripheralScreen], selected by
/// the app-wide board setting. The DevKit spec reuses the original const data
/// so its view is unchanged.
class _BoardSpec {
  const _BoardSpec({
    required this.badge,
    required this.heroTitle,
    required this.heroSubtitle,
    required this.heroNote,
    required this.metrics,
    required this.routes,
    required this.openPins,
    required this.cautions,
    required this.headerGroups,
    required this.footerMain,
    required this.footerRef,
  });

  final String badge;
  final String heroTitle;
  final String heroSubtitle;
  final String heroNote;
  final List<_Metric> metrics;
  final List<_PeripheralRoute> routes;
  final List<_OpenPin> openPins;
  final List<Widget> cautions;
  final List<_HeaderGroup> headerGroups;
  final String footerMain;
  final String footerRef;
}

const _devkitOpenPins = <_OpenPin>[
  _OpenPin(3, 'BOOT · MTDI', caution: true),
  _OpenPin(10, 'Available'),
  _OpenPin(13, 'USB D− · SDIO3', caution: true),
  _OpenPin(14, 'USB D+ · SDIO2', caution: true),
  _OpenPin(25, 'STRAPPING', caution: true),
  _OpenPin(26, 'STRAPPING', caution: true),
  _OpenPin(28, 'STRAPPING', caution: true),
];

const _devkitCautions = <Widget>[
  _CautionNote(
    color: _kAmber,
    icon: Icons.warning_amber_rounded,
    title: 'BOOT / STRAPPING WARNING',
    body: 'GPIO2, GPIO3, GPIO7, GPIO25, GPIO26, GPIO27, and GPIO28 affect '
        'configuration during boot or reset. Unused GPIO3, GPIO25, and GPIO28 '
        'should be protected from external circuits changing their logic levels.',
  ),
  _CautionNote(
    color: _kViolet,
    icon: Icons.usb_rounded,
    title: 'ALTERNATE BOARD FUNCTIONS',
    body: 'GPIO13/14 are unused by this project’s peripheral wiring, but also '
        'serve USB D−/D+ and SDIO functions, so using them may conflict with '
        'USB. GPIO15 is reserved for PSRAM on the N16R8 module and is not '
        'considered available.',
  ),
];

const _devkitSpec = _BoardSpec(
  badge: 'N16R8',
  heroTitle: 'ESP32-C5',
  heroSubtitle: 'DevKitC-1 · WROOM-1-N16R8',
  heroNote: '14 unique GPIOs are assigned functions by the firmware and board.',
  metrics: [
    _Metric(value: '14', label: 'DIPAKAI', color: _kMint),
    _Metric(value: '7', label: 'UNUSED', color: Colors.white),
    _Metric(value: '7', label: 'STRAPPING', color: _kAmber),
  ],
  routes: _peripheralRoutes,
  openPins: _devkitOpenPins,
  cautions: _devkitCautions,
  headerGroups: [
    _HeaderGroup('J1', _j1Pins),
    _HeaderGroup('J3', _j3Pins),
  ],
  footerMain: 'This map is based on the project’s default firmware and the '
      'ESP32-C5-DevKitC-1 v1.2. Verify your board revision and module before '
      'connecting wires.',
  footerRef: 'Board reference: Espressif ESP32-C5-DevKitC-1 · Hardware Reference',
);

// ── Seeed XIAO ESP32-C5 (8 MB flash / 8 MB PSRAM) ────────────────────────────

const _xiaoRoutes = <_PeripheralRoute>[
  _PeripheralRoute(
    icon: Icons.alt_route,
    color: _kViolet,
    title: 'Shared SPI bus',
    subtitle: 'Shared bus for nRF24, CC1101, and microSD',
    note: 'Native SPI pads D8/D9/D10. microSD initializes before radio access.',
    pins: [_PinUse('SCK', 8), _PinUse('MISO', 9), _PinUse('MOSI', 10)],
  ),
  _PeripheralRoute(
    icon: Icons.settings_input_antenna,
    color: Color(0xFF66B7FF),
    title: 'nRF24L01+',
    subtitle: 'Radio 2.4 GHz',
    note: 'Uses the shared SPI bus (pads D6/D7).',
    pins: [_PinUse('CSN', 11), _PinUse('CE', 12)],
  ),
  _PeripheralRoute(
    icon: Icons.settings_input_antenna,
    color: _kAmber,
    title: 'CC1101',
    subtitle: 'Radio sub-GHz',
    note: 'Uses the shared SPI bus (pads D4/D5).',
    pins: [_PinUse('CS', 23), _PinUse('GDO0', 24)],
  ),
  _PeripheralRoute(
    icon: Icons.settings_remote,
    color: Color(0xFFFF7770),
    title: 'Infrared',
    subtitle: 'Receiver and transmitter',
    note: 'RX receives signals; TX transmits (pads D2/D0).',
    pins: [_PinUse('RX', 25), _PinUse('TX', 1)],
  ),
  _PeripheralRoute(
    icon: Icons.circle,
    color: Color(0xFFFFB44C),
    title: 'Onboard USER LED',
    subtitle: 'Single-colour status LED',
    note: 'USER_LED on GPIO27 driven by PWM, not an addressable WS2812.',
    pins: [_PinUse('LED', 27)],
  ),
  _PeripheralRoute(
    icon: Icons.sd_storage,
    color: Color(0xFF84AFFF),
    title: 'microSD',
    subtitle: 'Chip select and card detection',
    note: 'Data lines use the shared SPI bus (pads D1/D3).',
    pins: [_PinUse('CS', 0), _PinUse('DETECT', 7)],
  ),
];

const _xiaoOpenPins = <_OpenPin>[
  _OpenPin(2, 'JTAG MTMS · rear', caution: true),
  _OpenPin(3, 'JTAG MTDI · rear', caution: true),
  _OpenPin(4, 'JTAG MTCK · rear', caution: true),
  _OpenPin(5, 'JTAG MTDO · rear', caution: true),
  _OpenPin(6, 'ADC_BAT · battery', caution: true),
  _OpenPin(26, 'ADC_CRL · battery', caution: true),
  _OpenPin(28, 'BOOT', caution: true),
];

const _xiaoCautions = <Widget>[
  _CautionNote(
    color: _kAmber,
    icon: Icons.warning_amber_rounded,
    title: 'BOOT / RESERVED PINS',
    body: 'All 11 castellated pads (D0–D10) are assigned. GPIO28 is BOOT and '
        'GPIO27 is the USER_LED. The JTAG pads GPIO2/3/4/5 (MTMS/MTDI/MTCK/MTDO) '
        'are on the reverse side and are reserved for debugging.',
  ),
  _CautionNote(
    color: _kViolet,
    icon: Icons.usb_rounded,
    title: 'BATTERY & NO UART FLASHER',
    body: 'GPIO6 (ADC_BAT) and GPIO26 (ADC_CRL) are wired to the onboard '
        'battery-voltage divider and are not exposed as pads. No pad is free for '
        'the UART ESP-flasher, so that feature is unavailable on the XIAO.',
  ),
];

const _xiaoLeftPads = <_HeaderPin>[
  _HeaderPin(1, 'GPIO1', 'D0 · Infrared TX', inUse: true),
  _HeaderPin(2, 'GPIO0', 'D1 · microSD CS', inUse: true),
  _HeaderPin(3, 'GPIO25', 'D2 · Infrared RX', inUse: true),
  _HeaderPin(4, 'GPIO7', 'D3 · microSD detect', inUse: true),
  _HeaderPin(5, 'GPIO23', 'D4 · CC1101 CS', inUse: true),
  _HeaderPin(6, 'GPIO24', 'D5 · CC1101 GDO0', inUse: true),
  _HeaderPin(7, 'GPIO11', 'D6 · nRF24 CSN', inUse: true),
];

const _xiaoRightPads = <_HeaderPin>[
  _HeaderPin(1, '5V', 'Power', blocked: true),
  _HeaderPin(2, 'GND', 'Ground', blocked: true),
  _HeaderPin(3, '3V3', 'Power', blocked: true),
  _HeaderPin(4, 'GPIO10', 'D10 · SPI MOSI', inUse: true),
  _HeaderPin(5, 'GPIO9', 'D9 · SPI MISO', inUse: true),
  _HeaderPin(6, 'GPIO8', 'D8 · SPI SCK', inUse: true),
  _HeaderPin(7, 'GPIO12', 'D7 · nRF24 CE', inUse: true),
];

const _xiaoSpec = _BoardSpec(
  badge: 'C5HR8',
  heroTitle: 'XIAO ESP32-C5',
  heroSubtitle: 'Seeed Studio · 8 MB flash / 8 MB PSRAM',
  heroNote: 'All 11 castellated pads (D0–D10) are assigned by the firmware.',
  metrics: [
    _Metric(value: '11', label: 'PADS USED', color: _kMint),
    _Metric(value: '0', label: 'FREE', color: Colors.white),
    _Metric(value: '4', label: 'JTAG REAR', color: _kAmber),
  ],
  routes: _xiaoRoutes,
  openPins: _xiaoOpenPins,
  cautions: _xiaoCautions,
  headerGroups: [
    _HeaderGroup('Left pads', _xiaoLeftPads),
    _HeaderGroup('Right pads', _xiaoRightPads),
  ],
  footerMain: 'This map is based on the project firmware and the Seeed Studio '
      'XIAO ESP32-C5 pad layout. Verify your board before connecting wires.',
  footerRef: 'Board reference: Seeed Studio XIAO ESP32-C5 · Wiki pinout',
);

class _HeaderPinMap extends StatelessWidget {
  const _HeaderPinMap({required this.title, required this.pins});

  final String title;
  final List<_HeaderPin> pins;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.fromLTRB(10, 9, 10, 5),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.19),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white.withValues(alpha: 0.07)),
      ),
      child: Column(
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(4, 0, 4, 7),
            child: Row(
              children: [
                Text(
                  title,
                  style: const TextStyle(
                    color: _kMint,
                    fontFamily: 'monospace',
                    fontSize: 13,
                    fontWeight: FontWeight.w800,
                  ),
                ),
                const SizedBox(width: 7),
                Text('${pins.length}-pin header',
                    style: const TextStyle(color: Colors.white38, fontSize: 9)),
              ],
            ),
          ),
          for (final pin in pins) _HeaderPinRow(pin: pin),
        ],
      ),
    );
  }
}

class _HeaderPinRow extends StatelessWidget {
  const _HeaderPinRow({required this.pin});

  final _HeaderPin pin;

  @override
  Widget build(BuildContext context) {
    final color = pin.blocked
        ? Colors.white30
        : pin.warning
        ? _kAmber
        : pin.inUse
        ? _kMint
        : Colors.white70;
    final state = pin.blocked
        ? 'BOARD'
        : pin.inUse
        ? 'PAKAI'
        : 'KOSONG';
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 5),
      decoration: BoxDecoration(
        border: Border(top: BorderSide(color: Colors.white.withValues(alpha: 0.045))),
      ),
      child: Row(
        children: [
          SizedBox(
            width: 25,
            child: Text(
              '${pin.position}'.padLeft(2, '0'),
              style: const TextStyle(
                color: Colors.white30,
                fontFamily: 'monospace',
                fontSize: 9,
              ),
            ),
          ),
          SizedBox(
            width: 66,
            child: Text(
              pin.label,
              style: TextStyle(
                color: color,
                fontFamily: 'monospace',
                fontSize: 10,
                fontWeight: FontWeight.bold,
              ),
            ),
          ),
          Expanded(
            child: Text(
              pin.role,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(color: Colors.white60, fontSize: 9),
            ),
          ),
          const SizedBox(width: 4),
          if (pin.warning)
            const Icon(Icons.warning_amber_rounded, color: _kAmber, size: 12)
          else
            Container(
              width: 5,
              height: 5,
              decoration: BoxDecoration(color: color, shape: BoxShape.circle),
            ),
          const SizedBox(width: 4),
          SizedBox(
            width: 36,
            child: Text(
              state,
              textAlign: TextAlign.right,
              style: TextStyle(
                color: color.withValues(alpha: 0.75),
                fontSize: 7,
                fontWeight: FontWeight.bold,
                letterSpacing: 0.4,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _CircuitGridPainter extends CustomPainter {
  const _CircuitGridPainter();

  @override
  void paint(Canvas canvas, Size size) {
    final dotPaint = Paint()..color = _kMint.withValues(alpha: 0.055);
    final linePaint = Paint()
      ..color = _kMint.withValues(alpha: 0.035)
      ..strokeWidth = 1;
    for (var x = 14.0; x < size.width; x += 24) {
      for (var y = 10.0; y < size.height; y += 24) {
        canvas.drawCircle(Offset(x, y), 1, dotPaint);
      }
    }
    canvas.drawLine(Offset(0, 185), Offset(size.width * 0.19, 185), linePaint);
    canvas.drawLine(
      Offset(size.width * 0.84, 612),
      Offset(size.width, 612),
      linePaint,
    );
  }

  @override
  bool shouldRepaint(covariant _CircuitGridPainter oldDelegate) => false;
}
