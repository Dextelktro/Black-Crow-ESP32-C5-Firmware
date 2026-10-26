import 'package:flutter/material.dart';

enum BleSpamType {
  apple(
    id: 'apple',
    label: 'Apple',
    subtitle: 'AirPods · Beats · HomePod',
    icon: Icons.apple,
    accent: Color(0xFFEEEEEE),
    defaultDelay: 20,
    warnText: null,
  ),
  sourApple(
    id: 'sour',
    label: 'Sour Apple',
    subtitle: 'iOS 17- proximity crash',
    icon: Icons.warning_amber_rounded,
    accent: Color(0xFFFF4040),
    defaultDelay: 20,
    warnText:
        'Sour Apple may crash unpatched iOS 17 devices. '
        'Use only on devices you own or are authorized to test.',
  ),
  samsung(
    id: 'samsung',
    label: 'Samsung',
    subtitle: 'Galaxy Watch · EasySetup',
    icon: Icons.watch_outlined,
    accent: Color(0xFF4FA8FF),
    defaultDelay: 50,
    warnText: null,
  ),
  google(
    id: 'google',
    label: 'Google FastPair',
    subtitle: 'Android 8.0+',
    icon: Icons.android,
    accent: Color(0xFF34A853),
    defaultDelay: 50,
    warnText: null,
  ),
  windows(
    id: 'windows',
    label: 'Windows Swift',
    subtitle: 'Windows 10 / 11',
    icon: Icons.desktop_windows_outlined,
    accent: Color(0xFF00A4EF),
    defaultDelay: 50,
    warnText: null,
  ),
  xiaomi(
    id: 'xiaomi',
    label: 'Xiaomi MiBeacon',
    subtitle: 'Mi Home detection',
    icon: Icons.home_outlined,
    accent: Color(0xFFFF6900),
    defaultDelay: 50,
    warnText:
        'Xiaomi MiBeacon triggers device detection in Mi Home; '
        'it does not show a pairing prompt like Apple or Google.',
  ),
  android(
    id: 'android',
    label: 'Android (Auto)',
    subtitle: 'Samsung + Google mix',
    icon: Icons.smartphone,
    accent: Color(0xFF3DDC84),
    defaultDelay: 50,
    warnText: null,
  ),
  all(
    id: 'all',
    label: 'All Types',
    subtitle: 'Random rotate',
    icon: Icons.all_inclusive,
    accent: Color(0xFFFFB000),
    defaultDelay: 50,
    warnText: null,
  );

  const BleSpamType({
    required this.id,
    required this.label,
    required this.subtitle,
    required this.icon,
    required this.accent,
    required this.defaultDelay,
    required this.warnText,
  });

  final String id;
  final String label;
  final String subtitle;
  final IconData icon;
  final Color accent;
  final int defaultDelay;
  final String? warnText;

  static BleSpamType? fromId(String id) {
    for (final t in BleSpamType.values) {
      if (t.id == id) return t;
    }
    if (id == 'sourapple') return sourApple;
    return null;
  }
}
