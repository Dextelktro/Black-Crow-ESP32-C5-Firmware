import 'package:flutter/foundation.dart';

/// Data contract for the Sub-GHz (CC1101) toolkit. The UI depends only on these
/// typed models — never on raw firmware telemetry strings or controller
/// internals — so the screens can be built and reasoned about independently of
/// how `device_controller` fills them.

/// Radio modulation. Ansonic is the only FM protocol we decode; everything else
/// is AM/OOK. A preset carries its modulation so the UI can label it correctly.
enum SubghzModulation { am, fm }

extension SubghzModulationX on SubghzModulation {
  String get label => this == SubghzModulation.fm ? 'FM' : 'AM';
}

/// A CC1101 modulation preset the firmware understands. [id] is the short CLI
/// token sent in `subghz preset <id>`.
@immutable
class SubghzPreset {
  const SubghzPreset(this.id, this.label, this.modulation, this.rxBandwidthKhz);

  final String id; // ook270 / ook650 / 2fsk238 / 2fsk476
  final String label;
  final SubghzModulation modulation;
  final double rxBandwidthKhz;

  /// The four presets baked into the firmware (subghz_presets.c), in order.
  static const List<SubghzPreset> all = [
    SubghzPreset('ook270', 'OOK 270 kHz', SubghzModulation.am, 270),
    SubghzPreset('ook650', 'OOK 650 kHz', SubghzModulation.am, 650),
    SubghzPreset('2fsk238', '2-FSK 2.38 kHz', SubghzModulation.fm, 270),
    SubghzPreset('2fsk476', '2-FSK 47.6 kHz', SubghzModulation.fm, 270),
  ];

  static SubghzPreset byId(String id) {
    for (final p in all) {
      if (p.id == id) return p;
    }
    return all[1]; // default ook650
  }
}

/// Where `.sub` files are saved. `unset` means the user has not chosen yet, and
/// save/emulate are refused until they do (manual, no silent fallback).
enum SubghzStorageTarget { unset, littlefs, sd }

extension SubghzStorageTargetX on SubghzStorageTarget {
  String get id => switch (this) {
    SubghzStorageTarget.littlefs => 'littlefs',
    SubghzStorageTarget.sd => 'sd',
    SubghzStorageTarget.unset => 'unset',
  };

  String get label => switch (this) {
    SubghzStorageTarget.littlefs => 'Internal flash (LittleFS)',
    SubghzStorageTarget.sd => 'SD card',
    SubghzStorageTarget.unset => 'Not selected',
  };

  static SubghzStorageTarget parse(String s) => switch (s) {
    'littlefs' => SubghzStorageTarget.littlefs,
    'sd' => SubghzStorageTarget.sd,
    _ => SubghzStorageTarget.unset,
  };
}

/// One focused spectrum band the firmware offers (1-based index in CLI).
@immutable
class SubghzBand {
  const SubghzBand({
    required this.index,
    required this.label,
    required this.loHz,
    required this.hiHz,
  });

  final int index;
  final String label;
  final int loHz;
  final int hiHz;

  int get centerHz => (loHz + hiHz) ~/ 2;
}

/// A previous Frequency Analyzer detection, newest entries shown first.
@immutable
class SubghzAnalyzerDetection {
  const SubghzAnalyzerDetection({
    required this.freqHz,
    required this.rssi,
    required this.at,
  });

  final int freqHz;
  final int rssi;
  final DateTime at;
}

/// A decoded protocol frame (Read mode).
@immutable
class SubghzDecoded {
  const SubghzDecoded({
    required this.proto,
    required this.bits,
    required this.keyHex,
    required this.te,
    required this.freqHz,
    required this.rssi,
    required this.at,
    this.keyRawHex = '',
  });

  final String proto; // "unknown" if not decoded
  final int bits;
  final String keyHex; // grouped hex of the meaningful bits, e.g. "AB CD EF"
  final String keyRawHex; // raw ungrouped hex from telemetry (for save), e.g. "ABCDEF"
  final int te; // base Te in us (0 if n/a)
  final int freqHz;
  final int rssi;
  final DateTime at;

  bool get isKnown => proto.isNotEmpty && proto.toLowerCase() != 'unknown';
}

/// A raw OOK burst (Read RAW mode). [durations] is the signed-us pulse train
/// (+high / -low), accumulated from the chunked RAWBIN telemetry.
class SubghzRawBurst {
  SubghzRawBurst({
    required this.seq,
    required this.count,
    required this.freqHz,
    required this.preset,
    required this.rssi,
    required this.truncated,
    required this.at,
  });

  final int seq;
  final int count; // declared pulse count from the RAW header
  final int freqHz;
  final String preset;
  final int rssi;
  final bool truncated;
  final DateTime at;
  final List<int> durations = []; // filled by RAWBIN chunks
}

/// One stored `.sub` file in the library.
@immutable
class SubghzSavedFile {
  const SubghzSavedFile({required this.name, required this.bytes});

  final String name;
  final int bytes;
}

/// Decoded fields of one stored file (`subghz info <name>`).
@immutable
class SubghzFileInfo {
  const SubghzFileInfo({
    required this.name,
    required this.isKey,
    this.proto = '',
    this.bits = 0,
    this.keyHex = '',
    this.te = 0,
    this.count = 0,
    this.freqHz = 0,
    this.preset = '',
    this.readable = true,
  });

  final String name;
  final bool isKey; // true = protocol Key File, false = RAW file
  final String proto;
  final int bits;
  final String keyHex;
  final int te;
  final int count; // RAW pulse count
  final int freqHz;
  final String preset;
  final bool readable;
}

/// One spectrum sweep frame (Spectrum mode). [bins] are dBm values, low → high
/// frequency across [centerHz] ± [spanHz]/2.
@immutable
class SubghzSpecFrame {
  const SubghzSpecFrame({
    required this.seq,
    required this.centerHz,
    required this.spanHz,
    required this.stepHz,
    required this.peakDbm,
    required this.peakFreqHz,
    required this.bins,
  });

  final int seq;
  final int centerHz;
  final int spanHz;
  final int stepHz;
  final int peakDbm;
  final int peakFreqHz;
  final List<int> bins;
}

/// Live status snapshot from `subghz status`.
@immutable
class SubghzStatus {
  const SubghzStatus({
    required this.present,
    required this.version,
    required this.part,
    required this.freqHz,
    required this.preset,
    required this.rssi,
    required this.at,
  });

  final bool present;
  final int version;
  final int part;
  final int freqHz;
  final String preset;
  final int rssi;
  final DateTime at;
}
