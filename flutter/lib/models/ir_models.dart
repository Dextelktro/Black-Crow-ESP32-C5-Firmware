import 'package:flutter/material.dart';

/// One IR remote button exposed by the universal remote.
class IrButton {
  const IrButton(this.token, this.label, this.icon);
  final String token; // canonical token sent to firmware (e.g. "vol_up")
  final String label; // human label
  final IconData icon;
}

/// A universal-remote category (TV, A/C, …) with its button layout.
class IrCategory {
  const IrCategory({
    required this.id,
    required this.label,
    required this.icon,
    required this.color,
    required this.buttons,
    this.builtIn = true,
  });

  final String id; // firmware category token (tv/ac/projector/led/fan/audio)
  final String label;
  final IconData icon;
  final Color color;
  final List<IrButton> buttons;

  /// Whether the firmware ships built-in codes for this category. When
  /// false, the universal action falls back to capture/emulate.
  final bool builtIn;
}

/// The six universal categories and their built-in button layouts.
const List<IrCategory> kIrCategories = [
  IrCategory(
    id: 'tv',
    label: 'Universal TV',
    icon: Icons.tv,
    color: Color(0xFF4FA8FF),
    buttons: [
      IrButton('power', 'Power', Icons.power_settings_new),
      IrButton('mute', 'Mute', Icons.volume_off),
      IrButton('vol_up', 'Vol +', Icons.volume_up),
      IrButton('vol_dn', 'Vol −', Icons.volume_down),
      IrButton('ch_up', 'Ch +', Icons.keyboard_arrow_up),
      IrButton('ch_dn', 'Ch −', Icons.keyboard_arrow_down),
    ],
  ),
  IrCategory(
    id: 'audio',
    label: 'Universal Audio',
    icon: Icons.speaker,
    color: Color(0xFF00E5FF),
    buttons: [
      IrButton('power', 'Power', Icons.power_settings_new),
      IrButton('mute', 'Mute', Icons.volume_off),
      IrButton('vol_up', 'Vol +', Icons.volume_up),
      IrButton('vol_dn', 'Vol −', Icons.volume_down),
      IrButton('play', 'Play', Icons.play_arrow),
      IrButton('pause', 'Pause', Icons.pause),
      IrButton('next', 'Next', Icons.skip_next),
      IrButton('prev', 'Previous', Icons.skip_previous),
    ],
  ),
  IrCategory(
    id: 'ac',
    label: 'Universal A/C',
    icon: Icons.ac_unit,
    color: Color(0xFF2EE6C5),
    buttons: [
      IrButton('off', 'Off', Icons.power_settings_new),
      IrButton('dry', 'Dry', Icons.water_drop),
      IrButton('cool_hi', 'Cool · High', Icons.ac_unit),
      IrButton('cool_lo', 'Cool · Low', Icons.ac_unit),
      IrButton('heat_hi', 'Heat · High', Icons.whatshot),
      IrButton('heat_lo', 'Heat · Low', Icons.whatshot),
    ],
  ),
  IrCategory(
    id: 'projector',
    label: 'Universal Projector',
    icon: Icons.videocam,
    color: Color(0xFF9B8CFF),
    buttons: [
      IrButton('power', 'Power', Icons.power_settings_new),
      IrButton('vol_up', 'Vol +', Icons.volume_up),
      IrButton('vol_dn', 'Vol −', Icons.volume_down),
      IrButton('mute', 'Mute', Icons.volume_off),
    ],
  ),
  IrCategory(
    id: 'led',
    label: 'Universal LED',
    icon: Icons.lightbulb,
    color: Color(0xFFFFB000),
    buttons: [
      IrButton('power_on', 'Power on', Icons.power_settings_new),
      IrButton('power_off', 'Power off', Icons.power_off),
      IrButton('bright_up', 'Bright +', Icons.brightness_high),
      IrButton('bright_dn', 'Bright −', Icons.brightness_low),
      IrButton('red', 'Red', Icons.circle),
      IrButton('green', 'Green', Icons.circle),
      IrButton('blue', 'Blue', Icons.circle),
      IrButton('white', 'White', Icons.circle),
    ],
  ),
  IrCategory(
    id: 'fan',
    label: 'Universal Fan',
    icon: Icons.air,
    color: Color(0xFF39FF14),
    buttons: [
      IrButton('power', 'Power', Icons.power_settings_new),
      IrButton('speed_up', 'Speed +', Icons.speed),
      IrButton('speed_dn', 'Speed −', Icons.speed),
      IrButton('swing', 'Swing', Icons.sync),
      IrButton('mode', 'Mode', Icons.tune),
      IrButton('timer', 'Timer', Icons.timer),
    ],
  ),
];

/// A signal picked up by the IR receiver (decoded summary + raw timings).
class IrDetectedSignal {
  IrDetectedSignal({
    required this.proto,
    required this.address,
    required this.command,
    required this.bits,
    required this.rawCount,
    required this.bytes,
    required this.at,
    required this.captureId,
  });

  final int captureId;
  final String proto;
  final int address;
  final int command;
  final int bits;
  int rawCount;
  final String bytes; // dot-separated hex, or "-"
  final List<int> raw = [];
  final DateTime at;

  String get title {
    if (proto == 'RAW' || proto == 'Unknown') {
      return 'RAW · $rawCount edges';
    }
    return proto;
  }

  String get addrCmd =>
      'addr 0x${address.toRadixString(16).toUpperCase()}'
      '  cmd 0x${command.toRadixString(16).toUpperCase()}';
}

/// A signal saved on the device's LittleFS storage.
class IrSavedEntry {
  IrSavedEntry({
    required this.name,
    required this.sizeBytes,
    required this.proto,
    required this.address,
    required this.command,
    required this.rawCount,
  });

  final String name;
  final int sizeBytes;
  final String proto;
  final int address;
  final int command;
  final int rawCount;

  String get subtitle {
    final p = (proto == '?' || proto.isEmpty) ? 'RAW' : proto;
    return '$p · $rawCount edges · $sizeBytes B';
  }
}
