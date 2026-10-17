import 'dart:async';
import 'dart:collection';
import 'dart:convert';

import 'package:flutter/foundation.dart';

import '../models/acl_entry.dart';
import '../models/ap_entry.dart';
import '../models/ble_spam_type.dart';
import '../models/ir_models.dart';
import '../models/portal_models.dart';
import '../models/sta_entry.dart';
import 'nus_client.dart';

/// Satu frame deauth yang di-report oleh firmware.
class DeauthFrame {
  DeauthFrame({
    required this.channel,
    required this.dst,
    required this.src,
    required this.reason,
    required this.timestamp,
  });

  final int channel;
  final String dst;
  final String src;
  final int reason;
  final DateTime timestamp;
}

/// Satu BLE device yang terdeteksi oleh BLE scanner firmware.
class BleScanDevice {
  BleScanDevice({
    required this.addr,
    required this.rssi,
    required this.type,
    required this.addrType,
    required this.mfg,
    required this.name,
    required this.seen,
    required this.lastSeen,
  });

  final String addr;
  int rssi;
  final int type; // advertising event type (legacy PDU type)
  final int addrType; // 0 = public, 1 = random
  String mfg;
  String name;
  int seen;
  DateTime lastSeen;
}

/// Device seen by BLE wardriving (continuous, classified, live).
class BleWarDevice {
  BleWarDevice({
    required this.addr,
    required this.rssi,
    required this.seen,
    required this.addrType,
    required this.priv,
    required this.vendor,
    required this.dtype,
    required this.company,
    required this.name,
    required this.firstSeen,
    required this.lastSeen,
    this.isFindMyAirTagFormat = false,
  });

  final String addr;
  int rssi;
  int seen;
  final int addrType; // 0 = public, 1 = random
  final String priv; // pub | static | rpa | nrpa
  String vendor; // apple | google | ... | '-'
  String dtype; // airpods | findmy | fastpair | ... | '-'
  String company; // 4-hex company id, or '----'
  String name;
  final DateTime firstSeen;
  DateTime lastSeen;
  bool isFindMyAirTagFormat;
}

/// AP yang terlihat oleh WiFi live scan (wardriving).
class WlscanAp {
  WlscanAp({
    required this.bssid,
    required this.ssid,
    required this.channel,
    required this.rssi,
    required this.is5ghz,
    required this.lastSeen,
  });

  final String bssid;
  String ssid;
  int channel;
  int rssi;
  bool is5ghz;
  DateTime lastSeen;
}

/// Satu channel pada WiFi spectrum.
class SpecChannel {
  SpecChannel({
    required this.channel,
    required this.freq,
    required this.rssi,
    required this.peakRssi,
    required this.hasMeanRssi,
    required this.count,
    required this.is5ghz,
    required this.valid,
    required this.sampledAt,
  });

  final int channel;
  final int freq;

  /// Mean packet RSSI during this channel's dwell window.
  int rssi;
  final int peakRssi;
  final bool hasMeanRssi;
  int count;
  final bool is5ghz;
  final bool valid;
  final DateTime sampledAt;
}

/// Target AP dari hasil rescan deauth all.
class DeauthTarget {
  DeauthTarget({
    required this.idx,
    required this.ssid,
    required this.bssid,
    required this.channel,
    required this.rssi,
    required this.is5ghz,
  });

  final int idx;
  final String ssid;
  final String bssid;
  final int channel;
  final int rssi;
  final bool is5ghz;
}

/// One 2.4 GHz channel row from the nRF24 RPD scanner.
class NrfScanChannel {
  NrfScanChannel({
    required this.channel,
    required this.rpdDetections,
    required this.peak,
  });

  final int channel;
  int rpdDetections;
  bool peak;
}

/// One captured nRF24 packet (pseudo-promiscuous sniffer).
class NrfSniffPacket {
  NrfSniffPacket({
    required this.idx,
    required this.channel,
    required this.len,
    required this.hex,
    required this.at,
  });

  final int idx;
  final int channel;
  final int len;
  final String hex;
  final DateTime at;
}

/// One MouseJack target (wireless keyboard/mouse dongle).
class NrfMjTarget {
  NrfMjTarget({
    required this.idx,
    required this.addr,
    required this.channel,
    required this.hits,
    required this.isLogitech,
  });

  final int idx;
  final String addr;
  int channel;
  int hits;
  bool isLogitech;
}

/// One file on the device's LittleFS partition (Home file manager).
class FsFile {
  FsFile({required this.path, required this.size});

  final String path; // relative to mount root, e.g. "remote.ir", "portal/x"
  final int size; // bytes
}

class _ByteSample {
  const _ByteSample(this.timestamp, this.bytes);

  final DateTime timestamp;
  final int bytes;
}

class DeviceController extends ChangeNotifier {
  static const int _maxRawLogChars = 64 * 1024;
  static const int _minSecs = 1;
  static const int _maxSecs = 3600;
  static const int _maxFramesKept = 50;
  static const Duration _sendTimeout = Duration(seconds: 8);
  static const Duration _throughputWindow = Duration(seconds: 1);

  DeviceController(this.conn) {
    _sub = conn.incoming.listen(
      _onChunk,
      onError: (Object error) {
        if (_disposed) return;
        connectionIssue = 'BLE data stream error: $error';
        _scheduleNotify();
      },
    );
    conn.disconnected.then((_) {
      if (_disposed) return;
      connectionLost = true;
      connectionIssue = 'BLE connection lost. Return to scan and reconnect.';
      scanning = false;
      deviceScanning = false;
      attacking = false;
      nukeStartedAt = null;
      attackStartedAt = null;
      deauthAllStartedAt = null;
      deauthAllScanning = false;
      _tmpTargets.clear();
      bleSpamRunning = false;
      bleSpamStartedAt = null;
      bleScanRunning = false;
      bleWarRunning = false;
      monitorRunning = false;
      monitorFps = 0;
      chipTemperatureC = null;
      chipTemperatureUpdatedAt = null;
      sysCpuUsagePercent = null;
      sysUpdatedAt = null;
      fsFiles = const [];
      fsListing = false;
      wlscanRunning = false;
      spectrumRunning = false;
      bleSpectrumRunning = false;
      wspamRunning = false;
      portalRunning = false;
      nrfScanRunning = false;
      nrfJamRunning = false;
      nrfSniffRunning = false;
      nrfMjRunning = false;
      nrfReplayActive = false;
      nrfMjInjecting = false;
      irRxRunning = false;
      irSpectrumOn = false;
      irUnivRunning = false;
      irEmulateRunning = false;
      _notifyTimer?.cancel();
      _notifyTimer = null;
      _notifyListeners();
    });
    for (final command in const [
      'status',
      'ls',
      'sel ls',
      'wl ls',
      'bl ls',
      'dev ls',
      'nrf status',
      'ir status',
    ]) {
      unawaited(_sendCommand(command).catchError((Object _) {}));
    }
  }

  final NusConnection conn;
  StreamSubscription<String>? _sub;
  Timer? _notifyTimer;
  bool _disposed = false;
  bool _temperatureNotificationPending = false;
  bool _systemNotificationPending = false;

  List<ApEntry> aps = const [];
  List<AclEntry> whitelist = const [];
  List<AclEntry> blacklist = const [];
  Set<int> selectedIdxs = {};
  bool attacking = false;
  String attackMode = 'mixed';
  bool scanning = false;
  String rawLog = '';
  DateTime? _lastScanTime;

  // Regular attack
  DateTime? attackStartedAt;
  int attackAps = 0;

  // Nuke
  DateTime? nukeStartedAt;
  int nukeDurationSecs = 0;
  int nukeApCount = 0;
  int nukePkts = 0;

  // Deauth All
  DateTime? deauthAllStartedAt;
  int deauthAllDurationSecs = 0;
  int deauthAllApCount = 0;
  int deauthAllPkts = 0;
  int deauthAllCycles = 0;
  int deauthAllCurrentChannel = 0;
  bool deauthAllCurrentIs5ghz = false;
  int deauthAllPps = 0;
  String deauthAllMode = 'skip'; // 'skip' or 'unicast'
  final List<DeauthFrame> deauthAllFrames = [];
  List<DeauthTarget> deauthAllTargets = const [];
  bool deauthAllScanning = false;

  // BLE Spam
  BleSpamType? bleSpamType;
  DateTime? bleSpamStartedAt;
  int bleSpamDelayMs = 50;
  int bleSpamFrames = 0;
  int bleSpamFps = 0;
  int bleSpamDurationSecs = 0;
  bool bleSpamRunning = false;

  // BLE Scan
  bool bleScanRunning = false;
  bool bleScanActive = false; // active scan (gets names)
  int bleScanElapsed = 0;
  int bleScanFound = 0;
  final Map<String, BleScanDevice> _bleScanMap = {};
  List<BleScanDevice> bleScanDevices = const [];

  // Monitor (single-AP live profile)
  bool monitorRunning = false;
  int monitorChannel = 0;
  bool monitorIs5ghz = false;
  String? monitorBssid;
  int monitorRssi = 0;
  int monitorFps = 0;
  int monitorFrames = 0;
  int monitorMgmt = 0;
  int monitorData = 0;
  int monitorStas = 0;

  // Regular attack throughput.
  int attackPps = 0;

  // Device connection time (for Home uptime)
  final DateTime connectedAt = DateTime.now();

  double? chipTemperatureC;
  DateTime? chipTemperatureUpdatedAt;

  // System telemetry (firmware `sys:` line, 4 Hz).
  int? sysCpuUsagePercent;
  int sysHeapFree = 0; // internal SRAM heap, bytes
  int sysHeapTotal = 0;
  int sysPsramFree = 0; // external PSRAM heap, bytes (0 if absent)
  int sysPsramTotal = 0;
  int sysFlashFree = 0; // LittleFS `storage` partition, bytes
  int sysFlashTotal = 0;
  DateTime? sysUpdatedAt;

  // LittleFS file browser (Home "LittleFS Files" manager).
  List<FsFile> fsFiles = const [];
  int fsTotal = 0; // partition bytes
  int fsUsed = 0;
  bool fsListing = false;
  final List<FsFile> _fsTmp = [];

  // WiFi Live Scan (wardriving)
  bool wlscanRunning = false;
  String wlscanMode = 'fast';
  int wlscanCycles = 0;
  int wlscanUnique = 0;
  final Map<String, WlscanAp> _wlscanMap = {};
  List<WlscanAp> wlscanAps = const [];

  // Air monitor (Marauder-style sniffer) — populated by the `wlscan: AIR`
  // telemetry while the live scan runs under the Nuke radar. Per-second rates
  // derived from each window so the UI can show a real-time frame-rate monitor
  // and flag an over-the-air deauth/disassoc flood.
  int airPps = 0; // total frames/sec
  int airMgmtPps = 0;
  int airDataPps = 0;
  int airBeaconPps = 0;
  int airProbePps = 0;
  int airDeauthPps = 0;
  int airDisassocPps = 0;
  int airAuthPps = 0;
  int airEapolPps = 0;
  bool airAlert = false;
  DateTime? airUpdatedAt;
  int airRev = 0; // bumped on every AIR line (repaint key for the sparkline)
  static const int _airHistoryLen = 64;
  final List<int> airRateHistory = <int>[]; // total pps, oldest → newest
  final List<int> airThreatHistory = <int>[]; // deauth+disassoc pps

  void _resetAir() {
    airPps = airMgmtPps = airDataPps = airBeaconPps = airProbePps = 0;
    airDeauthPps = airDisassocPps = airAuthPps = airEapolPps = 0;
    airAlert = false;
    airUpdatedAt = null;
    airRateHistory.clear();
    airThreatHistory.clear();
  }

  // BLE Wardriving (continuous, classified)
  bool bleWarRunning = false;
  bool bleWarActive = false; // active scan (gets names)
  int bleWarCycles = 0;
  int bleWarUnique = 0;
  int bleFindMyObservations = 0;
  final Map<String, BleWarDevice> _bleWarMap = {};
  final Map<String, int> _bleFindMySeenByAddr = {};
  List<BleWarDevice> bleWarDevices = const [];
  Completer<String>? _bleFindMySaveCompleter;

  // WiFi Spectrum (channel energy)
  bool spectrumRunning = false;
  int spectrumSweep = 0;
  final Map<int, SpecChannel> _spectrumMap = {};
  final Map<int, SpecChannel> _spectrumPending = {};
  List<SpecChannel> spectrumChannels = const [];

  // BLE Spectrum (RSSI levels)
  bool bleSpectrumRunning = false;
  List<int> bleSpectrumBins = List<int>.filled(10, 0, growable: false);
  int? bleSpectrumMean;
  int bleSpectrumPeak = -100;
  int bleSpectrumTotal = 0;
  int bleSpectrumFrameId = 0;

  // WiFi Spam (beacon flood)
  bool wspamRunning = false;
  String wspamMode = 'funny';
  String wspamRate = 'aggressive';
  bool wspamDual = true;
  int wspamTotal = 0;
  int wspamFps = 0;
  int wspamChannel = 0;
  String wspamCurrentSsid = '';
  final List<String> wspamFeed = []; // most-recent-first live SSID feed
  static const int _wspamFeedMax = 40;
  List<String> wspamCustom = const [];
  final List<String> _wspamCustomTmp = [];

  // ── Evil Portal (captive portal) ────────────────────────────
  bool portalRunning = false;
  String portalTemplate = '';
  String portalSsid = '';
  int portalChannel = 1;
  int portalClients = 0;
  int portalHits = 0;
  int portalUp = 0; // uptime seconds reported by firmware
  int portalCredsCount = 0; // lifetime captures this run
  List<PortalTemplate> portalTemplates = const [];
  final List<PortalTemplate> _portalTemplatesTmp = [];
  final List<PortalCred> portalCreds = []; // most-recent-first
  static const int _portalCredsMax = 100;

  // ── nRF24 2.4 GHz radio ─────────────────────────────────────
  bool nrfModuleKnown = false; // true once firmware told us present/absent
  bool nrfPresent = false; // nRF24 detected & module ready

  /// Radio is in use by any app-initiated nRF feature. Derived so it can
  /// never get stuck out of sync with the individual running flags.
  bool get nrfRadioBusy =>
      nrfScanRunning ||
      nrfJamRunning ||
      nrfSniffRunning ||
      nrfMjRunning ||
      nrfReplayActive ||
      nrfMjInjecting;

  // nRF Scanner (RPD waterfall + one-shot peak)
  bool nrfScanRunning = false;
  int nrfScanSamples = 32; // strength denominator from firmware
  int nrfScanPeakCh = 0; // live waterfall peak
  int nrfScanPeakRpdDetections = 0;
  int nrfScanSweep = 0;
  int _nrfScanDumpSweep = 0;
  int? nrfPeakCh; // one-shot `nrf scan` result (null = none yet)
  int nrfPeakRpdDetections = 0;
  final List<NrfScanChannel> _nrfScanTmp = [];
  List<NrfScanChannel> nrfScanChannels = const [];

  // nRF Jammer
  bool nrfJamRunning = false;
  String nrfJamTarget = 'full';
  String nrfJamStrategy = 'cw';
  bool nrfJamWaterfall = false;
  int nrfJamFrames = 0;
  int nrfJamElapsed = 0;
  int nrfJamCurrentCh = 0;
  int nrfJamDurationSecs = 0;
  int nrfJamWfPeak = -1; // -1 = none

  // nRF Sniffer
  bool nrfSniffRunning = false;
  int nrfSniffChannel = 76;
  int nrfSniffCaptured = 0;
  int nrfSniffDropped = 0;
  int nrfSniffElapsed = 0;
  bool nrfReplayActive = false;
  static const int _nrfSniffMax = 64;
  final List<NrfSniffPacket> nrfSniffPackets = []; // most-recent-first
  final List<NrfSniffPacket> _nrfSniffLsTmp = [];
  bool _inNrfSniffLs = false;

  // nRF MouseJack
  bool nrfMjRunning = false;
  bool nrfMjInjecting = false;
  int nrfMjFound = 0;
  int nrfMjElapsed = 0;
  final Map<String, NrfMjTarget> _nrfMjMap = {};
  List<NrfMjTarget> nrfMjTargets = const [];

  // ── Infrared (RMT TX=GPIO4 / RX=GPIO5) ──────────────────────
  bool irModuleKnown = false;
  bool irReady = false;
  bool irRxRunning = false;
  bool irSpectrumOn = false;
  // Live spectrum envelope: mark-coverage bins (0..100) of the last burst.
  List<int> irSpectrumBins = const [];
  int irSpectrumTotalUs = 0;
  // Detected signals, most-recent first.
  final List<IrDetectedSignal> irDetected = [];
  static const int _irDetectedMax = 40;
  // Universal sweep progress.
  bool irUnivRunning = false;
  bool irUnivStopping = false;
  bool irUnivStopped = false;
  String irUnivCategory = '';
  String irUnivButton = '';
  int irUnivTotal = 0;
  int irUnivSent = 0;
  int irUnivFailed = 0;
  String irUnivBrand = '';
  String? irUnivError;
  // Emulate (replay) progress.
  bool irEmulateRunning = false;
  bool irEmulateStopping = false;
  String irEmulateName = '';
  int irEmulateSent = 0;
  // Saved library.
  List<IrSavedEntry> irSaved = const [];
  final List<IrSavedEntry> _irSavedTmp = [];
  int irFsUsed = 0;
  int irFsTotal = 0;

  int get cpuUsagePercent => sysCpuUsagePercent ?? 0;

  /// Short label for the active operation (Home dashboard).
  String get activity {
    if (portalRunning) return 'EVIL PORTAL';
    if (bleSpamRunning) return 'BLE SPAM';
    if (wspamRunning) return 'WIFI SPAM';
    if (attacking && attackMode == 'deauthall') return 'DEAUTH-ALL';
    if (attacking && attackMode == 'nuke') return 'NUKE';
    if (attacking) return 'DEAUTH';
    if (monitorRunning) return 'MONITOR';
    if (wlscanRunning) return 'LIVE SCAN';
    if (spectrumRunning) return 'WIFI SPECTRUM';
    if (bleSpectrumRunning) return 'BLE SPECTRUM';
    if (bleWarRunning) return 'BLE WARDRIVE';
    if (bleScanRunning) return 'BLE SCAN';
    if (nrfJamRunning) return 'NRF JAM';
    if (nrfSniffRunning) return 'NRF SNIFF';
    if (nrfMjRunning) return 'MOUSEJACK';
    if (nrfScanRunning) return 'NRF SCAN';
    if (irUnivRunning) return 'IR SEND';
    if (irEmulateRunning) return 'IR EMULATE';
    if (irRxRunning) return 'IR RX';
    if (scanning) return 'SCANNING';
    return 'IDLE';
  }

  List<StaEntry> stations = const [];
  bool deviceScanning = false;
  bool connectionLost = false;
  String? connectionIssue;
  final Queue<_ByteSample> _rxSamples = Queue<_ByteSample>();
  final Queue<_ByteSample> _txSamples = Queue<_ByteSample>();

  double get rxBytesPerSecond => _bytesPerSecond(_rxSamples);
  double get txBytesPerSecond => _bytesPerSecond(_txSamples);

  double _bytesPerSecond(Queue<_ByteSample> samples) {
    final cutoff = DateTime.now().subtract(_throughputWindow);
    while (samples.isNotEmpty && samples.first.timestamp.isBefore(cutoff)) {
      samples.removeFirst();
    }
    return samples.fold<int>(0, (sum, sample) => sum + sample.bytes).toDouble();
  }

  void _recordBytes(Queue<_ByteSample> samples, int bytes) {
    if (bytes > 0) samples.add(_ByteSample(DateTime.now(), bytes));
  }

  int _clampSecs(int secs) {
    if (secs < _minSecs) return _minSecs;
    if (secs > _maxSecs) return _maxSecs;
    return secs;
  }

  Future<void> _sendCommand(String command) async {
    try {
      await conn.send(command).timeout(_sendTimeout);
      _recordBytes(
        _txSamples,
        utf8.encode(command.endsWith('\n') ? command : '$command\n').length,
      );
    } on TimeoutException {
      if (!_disposed) {
        connectionIssue =
            'BLE command timeout ($command). Device might be busy.';
        _notifyListeners();
      }
      rethrow;
    } catch (e) {
      if (!_disposed) {
        connectionIssue = 'BLE command failed: $e';
        _notifyListeners();
      }
      rethrow;
    }
  }

  Future<void> send(String command) => _sendCommand(command);

  void _notifyListeners() {
    if (!_disposed) super.notifyListeners();
  }

  void _scheduleNotify() {
    if (_notifyTimer != null || _disposed) return;
    _notifyTimer = Timer(const Duration(milliseconds: 50), () {
      _notifyTimer = null;
      _notifyListeners();
    });
  }

  void dismissConnectionIssue() {
    if (connectionLost || connectionIssue == null) return;
    connectionIssue = null;
    _notifyListeners();
  }

  bool get scanIsStale {
    if (aps.isEmpty) return true;
    if (_lastScanTime == null) return true;
    return DateTime.now().difference(_lastScanTime!) >
        const Duration(minutes: 5);
  }

  // ── Parser state ─────────────────────────────────────────
  final StringBuffer _buf = StringBuffer();
  bool _inAps = false;
  bool _ignoreApList = false;
  bool _ignoreSelectionList = false;
  bool _inWl = false;
  bool _inBl = false;
  final List<ApEntry> _tmpAps = [];
  final List<AclEntry> _tmpWl = [];
  final List<AclEntry> _tmpBl = [];
  bool _inStas = false;
  final List<StaEntry> _tmpStations = [];
  final List<DeauthTarget> _tmpTargets = [];

  // ── Regex ────────────────────────────────────────────────
  static final _reNuke = RegExp(r'^nuke: (\d+)s\s+aps=(\d+)\s+pkts=(\d+)');
  static final _reAttack = RegExp(r'^attack: (\d+)s\s+aps=(\d+)');
  static final _reSelLine = RegExp(r'^sel:([ \d]*)$');
  static final _reSelToggle = RegExp(r'^sel: (\d+) (on|off)$');

  static final _reDeauthAllStart = RegExp(
    r'^deauthall:\s+START\s+duration=(\d+)s\s+channels=(\d+)\s+aps=(\d+)(?:\s+mode=(\w+))?',
  );
  static final _reDeauthAllProgress = RegExp(
    r'^deauthall:\s+(\d+)s\s+ch=(\d+)\(([^)]+)\)\s+cycles=(\d+)\s+total=(\d+)\s+pps=(\d+)',
  );
  static final _reDeauthAllStop = RegExp(r'^deauthall:\s+STOPPED');
  static final _reDeauthAllFrame = RegExp(
    r'^deauthall:\s+FRAME\s+ch=(\d+)\s+dst=([0-9A-Fa-f:]{17})\s+src=([0-9A-Fa-f:]{17})\s+reason=0x([0-9A-Fa-f]+)',
  );
  static final _reDeauthAllScanAp = RegExp(
    r'^deauthall:\s+SCAN_AP\s+idx=(\d+)\s+ssid=(.*?)\s+bssid=([0-9A-Fa-f:]{17})\s+ch=(\d+)\s+rssi=(-?\d+)\s+band=(\S+)',
  );
  static final _reDeauthAllScanDone = RegExp(
    r'^deauthall:\s+SCAN_DONE\s+total=(\d+)\s+took=(\d+)',
  );

  static final _reBleSpamStart = RegExp(
    r'^blespam:\s+START\s+type=(\w+)\s+delay=(\d+)ms\s+dur=(\d+)s',
  );
  static final _reBleSpamProgress = RegExp(
    r'^blespam:\s+(\d+)s\s+type=(\w+)\s+delay=(\d+)ms\s+frames=(\d+)\s+fps=(\d+)',
  );
  static final _reBleSpamStatus = RegExp(
    r'^blespam:\s+running\s+type=(\w+)\s+delay=(\d+)ms\s+frames=(\d+)',
  );
  static final _reBleSpamStop = RegExp(
    r'^blespam:\s+(?:STOPPED|stopped)',
    caseSensitive: false,
  );

  static final _reBleScanStart = RegExp(
    r'^blescan:\s+START\s+dur=(\d+)s\s+active=(\d+)',
  );
  static final _reBleScanDev = RegExp(
    r'^blescan:\s+DEV\s+addr=([0-9A-Fa-f:]{17})\s+rssi=(-?\d+)\s+type=(\d+)\s+atype=(\d+)\s+mfg=(\S+)\s+name="(.*)"',
  );
  static final _reBleScanLs = RegExp(
    r'^blescan:\s+LS\s+idx=\d+\s+addr=([0-9A-Fa-f:]{17})\s+rssi=(-?\d+)\s+type=(\d+)\s+atype=(\d+)\s+mfg=(\S+)\s+seen=(\d+)\s+name="(.*)"',
  );
  static final _reBleScanProgress = RegExp(
    r'^blescan:\s+PROGRESS\s+(\d+)s\s+found=(\d+)',
  );
  static final _reBleScanStopped = RegExp(
    r'^blescan:\s+STOPPED\s+total=(\d+)\s+took=(\d+)',
  );

  static final _reMonitorStart = RegExp(
    r'^monitor:\s+START\s+ch=(\d+)\s+bssid=([0-9A-Fa-f:]{17})\s+band=(\S+)',
  );
  static final _reMonitorStat = RegExp(
    r'^monitor:\s+STAT\s+rssi=(-?\d+)\s+fps=(\d+)\s+frames=(\d+)\s+mgmt=(\d+)\s+data=(\d+)\s+stas=(\d+)',
  );

  static final _reWlscanStart = RegExp(r'^wlscan:\s+START\s+mode=(\w+)');
  static final _reWlscanNew = RegExp(
    r'^wlscan:\s+NEW\s+ssid=(.*?)\s+bssid=([0-9A-Fa-f:]{17})\s+ch=(\d+)\s+rssi=(-?\d+)\s+band=(\S+)',
  );
  static final _reWlscanUpd = RegExp(
    r'^wlscan:\s+UPD\s+bssid=([0-9A-Fa-f:]{17})\s+rssi=(-?\d+)->(-?\d+)',
  );
  static final _reWlscanGone = RegExp(
    r'^wlscan:\s+GONE\s+bssid=([0-9A-Fa-f:]{17})',
  );
  static final _reWlscanCycle = RegExp(
    r'^wlscan:\s+CYCLE\s+(\d+)\s+total=(\d+)\s+new=(\d+)\s+upd=(\d+)\s+gone=(\d+)',
  );
  static final _reWlscanAir = RegExp(
    r'^wlscan:\s+AIR\s+win=(\d+)\s+tot=(\d+)\s+mgmt=(\d+)\s+data=(\d+)'
    r'\s+beacon=(\d+)\s+probe=(\d+)\s+deauth=(\d+)\s+disassoc=(\d+)'
    r'\s+auth=(\d+)\s+eapol=(\d+)\s+dpps=(\d+)\s+alert=(\d+)',
  );

  static final _reBleWarStart = RegExp(
    r'^blewar:\s+START\s+active=(\d+)\s+dur=',
  );
  static final _reBleWarNew = RegExp(
    r'^blewar:\s+NEW\s+addr=([0-9A-Fa-f:]{17})\s+rssi=(-?\d+)\s+atype=(\d+)\s+priv=(\S+)\s+vendor=(\S+)\s+dtype=(\S+)\s+company=(\S+)\s+name="(.*)"',
  );
  static final _reBleWarUpd = RegExp(
    r'^blewar:\s+UPD\s+addr=([0-9A-Fa-f:]{17})\s+rssi=(-?\d+)->(-?\d+)',
  );
  static final _reBleWarGone = RegExp(
    r'^blewar:\s+GONE\s+addr=([0-9A-Fa-f:]{17})',
  );
  static final _reBleWarCycle = RegExp(
    r'^blewar:\s+CYCLE\s+(\d+)\s+total=(\d+)\s+new=(\d+)\s+upd=(\d+)\s+gone=(\d+)',
  );
  static final _reBleWarMeta = RegExp(
    r'^blewar:\s+META\s+addr=([0-9A-Fa-f:]{17})\s+vendor=(\S+)\s+dtype=(\S+)\s+company=(\S+)\s+name="(.*)"',
  );
  static final _reBleFindMyObs = RegExp(
    r'^blefind:\s+OBS\s+addr=([0-9A-Fa-f:]{17})\s+seen=(\d+)\s+rssi=(-?\d+)\s+format=airtag-v1$',
  );
  static final _reBleFindMySaved = RegExp(
    r'^blefind:\s+SAVED\s+path="([^"]+)"',
  );
  static final _reBleFindMySaveError = RegExp(
    r'^blefind:\s+SAVE_ERROR\s+code=(-?\d+)',
  );

  static final _reSpecCh = RegExp(
    r'^spectrum:\s+CH\s+ch=(\d+)\s+freq=(\d+)\s+rssi=(-?\d+)\s+count=(\d+)\s+band=(\S+)\s+valid=(\d)(?:\s+peak=(-?\d+))?',
  );
  static final _reSpecSweep = RegExp(r'^spectrum:\s+SWEEP\s+n=(\d+)');

  static final _reBleSpecFrame = RegExp(
    r'^blespectrum:\s+FRAME\s+(?:mean=(-?\d+)\s+)?peak=(-?\d+)\s+total=(\d+)\s+bins=([\d,]+)',
  );

  static final _reWspamStart = RegExp(
    r'^wspam:\s+START\s+mode=(\w+)\s+rate=(\w+)\s+delay=\d+ms\s+band=(\S+)',
  );
  static final _reWspamStat = RegExp(
    r'^wspam:\s+STAT\s+(\d+)s\s+total=(\d+)\s+fps=(\d+)\s+ch=(\d+)\s+ssid="(.*)"',
  );
  static final _reWspamSsid = RegExp(
    r'^wspam:\s+SSID\s+ch=(\d+)\s+name="(.*)"',
  );
  static final _reWspamStop = RegExp(r'^wspam:\s+STOPPED\s+total=(\d+)');
  static final _reWspamLs = RegExp(r'^wspam:\s+LS\s+idx=\d+\s+ssid="(.*)"');

  // ── Evil Portal ───────────────────────────────────────────
  static final _rePortalStart = RegExp(
    r'^portal:\s+START\s+ssid="(.*)"\s+tmpl=(\w+)\s+ch=(\d+)',
  );
  static final _rePortalStat = RegExp(
    r'^portal:\s+STAT\s+up=(\d+)\s+clients=(\d+)\s+hits=(\d+)\s+creds=(\d+)',
  );
  static final _rePortalStatus = RegExp(
    r'^portal:\s+STATUS\s+running\s+ssid="(.*)"\s+tmpl=(\w+)\s+ch=(\d+)\s+up=(\d+)\s+clients=(\d+)\s+hits=(\d+)\s+creds=(\d+)',
  );
  static final _rePortalStop = RegExp(
    r'^portal:\s+STOPPED\s+up=(\d+)\s+clients=(\d+)\s+creds=(\d+)',
  );
  static final _rePortalTmpl = RegExp(
    r'^portal:\s+TMPL\s+id=(\w+)\s+label="(.*)"\s+ssid="(.*)"\s+custom=(\d)',
  );
  static final _rePortalCred = RegExp(
    r'^portal:\s+CRED\s+tmpl=(\w+)\s+ip=(\S+)\s+u="(.*)"\s+p="(.*)"',
  );

  // ── nRF24 ─────────────────────────────────────────────────
  static final _reNrfScanPeak = RegExp(
    r'^nrfscan:\s+peak\s+sweep=(\d+)\s+ch=(\d+)\s+rpd=(\d+)/(\d+)',
  );
  static final _reNrfScanCh = RegExp(
    r'^nrfscan:\s+CH\s+(\d+)\s+rpd=(\d+)(\s+PEAK)?\s*$',
  );
  static final _reNrfScanListStart = RegExp(r'^nrfscan:\s+LS\s+sweep=(\d+)');
  static final _reNrfPeakCli = RegExp(
    r'^nrf:\s+peak\s+ch=(\d+)\s+rpd=(-?\d+)/(\d+)',
  );
  static final _reNrfStatus = RegExp(
    r'^nrf:\s+wf=(\d+)\s+jam=(\d+)\s+sniff=(\d+)\s+mj=(\d+)\s+radio_busy=(\d+)',
  );

  static final _reNrfJamStart = RegExp(
    r'^nrfjam:\s+START\s+target=(\w+)\s+strategy=(\w+)\s+rate=(-?\d+)\s+pa=(-?\d+)\s+dwell=(\d+)us\s+burst=(\d+)\s+dur=(\d+)s\s+wf=(\d+)',
  );
  static final _reNrfJamProg = RegExp(
    r'^nrfjam:\s+(\d+)s\s+ch=(\d+)\s+frames=(\d+)',
  );
  static final _reNrfJamWf = RegExp(
    r'^nrfjam:\s+wf\s+peak\s+ch=(\d+)\s+strength=(-?\d+)',
  );
  static final _reNrfJamStop = RegExp(
    r'^nrfjam:\s+STOPPED\s+frames=(\d+)\s+took=(\d+)s',
  );

  static final _reNrfSniffStart = RegExp(
    r'^nrf_sniff:\s+START\s+ch=(\d+)\s+dur=(\d+)s',
  );
  static final _reNrfSniffPkt = RegExp(
    r'^nrf_sniff:\s+PKT\s+idx=(\d+)\s+len=(-?\d+)\s+data=(.*)$',
  );
  static final _reNrfSniffStat = RegExp(
    r'^nrf_sniff:\s+STAT\s+(\d+)s\s+captured=(\d+)\s+dropped=(\d+)',
  );
  static final _reNrfSniffStop = RegExp(
    r'^nrf_sniff:\s+STOPPED\s+total=(\d+)\s+dropped=(\d+)',
  );
  static final _reNrfSniffLs = RegExp(
    r'^nrf_sniff:\s+LS\s+idx=(\d+)\s+ch=(\d+)\s+len=(\d+)\s+t=(\d+)ms\s+data=(.*)$',
  );

  static final _reNrfMjStart = RegExp(r'^nrf_mj:\s+scan\s+START\s+dur=(\d+)s');
  static final _reNrfMjTarget = RegExp(
    r'^nrf_mj:\s+TARGET\s+idx=(\d+)\s+addr=([0-9A-Fa-f:]+)\s+ch=(\d+)\s+(\w+)',
  );
  static final _reNrfMjStat = RegExp(r'^nrf_mj:\s+STAT\s+(\d+)s\s+found=(\d+)');
  static final _reNrfMjStop = RegExp(
    r'^nrf_mj:\s+scan\s+STOPPED\s+targets=(\d+)',
  );
  static final _reNrfMjLs = RegExp(
    r'^nrf_mj:\s+LS\s+idx=(\d+)\s+addr=([0-9A-Fa-f:]+)\s+ch=(\d+)\s+hits=(\d+)\s+type=(\w+)',
  );

  // ── Infrared ──────────────────────────────────────────────
  static final _reIrSig = RegExp(
    r'^ir_rx:\s+SIG\s+proto=(\S+)\s+addr=0x([0-9A-Fa-f]+)\s+cmd=0x([0-9A-Fa-f]+)\s+bits=(\d+)\s+n=(\d+)\s+bytes=(\S+)\s+id=(\d+)',
  );
  static final _reIrRaw = RegExp(
    r'^ir_rx:\s+RAW\s+n=(\d+)\s+off=(\d+)\s+d=(.*)$',
  );
  static final _reIrWave = RegExp(
    r'^ir_spec:\s+WAVE\s+nb=(\d+)\s+total=(\d+)\s+d=(.*)$',
  );
  static final _reIrUniv = RegExp(
    r'^ir:\s+UNIV\s+cat=(\S+)\s+btn=(\S+)\s+total=(\d+)',
  );
  // TX logs are emitted only after the RMT transaction completes successfully.
  static final _reIrTx = RegExp(
    r'^ir:\s+TX\s+i=(\d+)/(\d+)\s+brand=(?:"(.*?)"|(\S+))\s+proto=(\S+)',
  );
  static final _reIrTxFail = RegExp(
    r'^ir:\s+TX_FAIL\s+i=(\d+)/(\d+)\s+brand="(.*?)"\s+err=(-?\d+)',
  );
  static final _reIrUnivDone = RegExp(
    r'^ir:\s+UNIV_DONE\s+cat=(\S+)\s+btn=(\S+)\s+sent=(\d+)(?:\s+failed=(\d+))?(?:\s+stopped=(\d+))?',
  );
  static final _reIrEmit = RegExp(
    r'^ir:\s+EMIT\s+name=(.+?)\s+count=(-?\d+)\s+n=(\d+)',
  );
  static final _reIrEmitDone = RegExp(
    r'^ir:\s+EMIT_DONE\s+name=(.+?)\s+sent=(\d+)(?:\s+failed=(\d+))?',
  );
  static final _reIrSavedCount = RegExp(r'^ir_saved:\s+LS\s+count=(\d+)');
  static final _reIrSavedEnt = RegExp(
    r'^ir_saved:\s+ENT\s+name=(.+?)\s+size=(\d+)\s+proto=(\S+)\s+addr=0x([0-9A-Fa-f]+)\s+cmd=0x([0-9A-Fa-f]+)\s+n=(\d+)',
  );
  static final _reIrStatus = RegExp(
    r'^ir:\s+status\s+ready=(\d+)\s+rx=(\d+)\s+spec=(\d+)\s+tx=(\d+)\s+emulate=(\d+)\s+saved=(\d+)\s+fs=(\d+)/(\d+)',
  );
  static final _reIrSaved = RegExp(
    r'^ir:\s+SAVED\s+name=(.+?)\s+timings=(\d+)',
  );

  static final _reAp = RegExp(
    r'^\s*(\d+)\s+(\d+)\s+(2\.4GHz|5GHz)\s+(-?\d+)\s+'
    r'([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\s+(.*?)\s*$',
  );
  static final _reWl = RegExp(
    r'^wl\s+([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\s+(auto|bssid|sta)',
  );
  static final _reBl = RegExp(
    r'^bl\s+([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\s+(auto|bssid|sta)',
  );
  static final _reMode = RegExp(r'^mode: (\w+)$');
  static final _reChipTemperature = RegExp(
    r'^temperature: (-?\d+(?:\.\d+)?) C$',
  );
  static final _reSys = RegExp(
    r'^sys:\s+cpu=(\d+)\s+heap=(\d+)/(\d+)\s+psram=(\d+)/(\d+)\s+flash=(\d+)/(\d+)',
  );
  static final _reFsLs = RegExp(r'^fs:\s+LS\s+total=(\d+)\s+used=(\d+)');
  static final _reFsEnt = RegExp(r'^fs:\s+ENT\s+path="(.*)"\s+size=(\d+)');
  static final _reFsInfo = RegExp(
    r'^fs:\s+INFO\s+total=(\d+)\s+used=(\d+)\s+free=(\d+)',
  );
  static final _reFsRm = RegExp(r'^fs:\s+RM\s+path="(.*)"\s+(ok|notfound|bad)');
  static final _reStaLine = RegExp(
    r'^sta:\s+([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\s+(-?\d+)\s+(\d+)\s+([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})',
  );

  void _onChunk(String chunk) {
    _recordBytes(_rxSamples, utf8.encode(chunk).length);
    final combinedLog = rawLog + chunk;
    if (combinedLog.length <= _maxRawLogChars) {
      rawLog = combinedLog;
    } else {
      var start = combinedLog.length - _maxRawLogChars;
      final lineBreak = combinedLog.indexOf('\n', start);
      if (lineBreak >= 0) {
        start = lineBreak + 1;
      } else if (start > 0) {
        final firstCodeUnit = combinedLog.codeUnitAt(start);
        if (firstCodeUnit >= 0xDC00 && firstCodeUnit <= 0xDFFF) start--;
      }
      rawLog = combinedLog.substring(start);
    }
    _buf.write(chunk);
    final s = _buf.toString();
    final parts = s.split('\n');
    _buf.clear();
    if (!s.endsWith('\n')) {
      _buf.write(parts.removeLast());
    } else {
      parts.removeLast();
    }
    for (final p in parts) {
      _parseLine(p.replaceAll('\r', '').trim());
    }
    if (_temperatureNotificationPending || _systemNotificationPending) {
      _temperatureNotificationPending = false;
      _systemNotificationPending = false;
      _notifyTimer?.cancel();
      _notifyTimer = null;
      _notifyListeners();
    } else {
      _scheduleNotify();
    }
  }

  void _commitAps() {
    aps = List.unmodifiable(_tmpAps.toList());
    _tmpAps.clear();
    _inAps = false;
  }

  void _commitWl() {
    whitelist = List.unmodifiable(_tmpWl.toList());
    _tmpWl.clear();
    _inWl = false;
  }

  void _commitBl() {
    blacklist = List.unmodifiable(_tmpBl.toList());
    _tmpBl.clear();
    _inBl = false;
  }

  void _commitStas() {
    stations = List.unmodifiable(_tmpStations.toList());
    _tmpStations.clear();
    _inStas = false;
  }

  void _commitTargets() {
    deauthAllTargets = List.unmodifiable(_tmpTargets.toList());
    _tmpTargets.clear();
    deauthAllScanning = false;
  }

  void _upsertBleScan({
    required String addr,
    required int rssi,
    required int type,
    required int addrType,
    required String mfg,
    required String name,
    int? seen,
  }) {
    final existing = _bleScanMap[addr];
    if (existing != null) {
      existing.rssi = rssi;
      existing.lastSeen = DateTime.now();
      if (seen != null) {
        existing.seen = seen;
      } else {
        existing.seen += 1;
      }
      if (name.isNotEmpty) existing.name = name;
      if (mfg != '----') existing.mfg = mfg;
    } else {
      _bleScanMap[addr] = BleScanDevice(
        addr: addr,
        rssi: rssi,
        type: type,
        addrType: addrType,
        mfg: mfg,
        name: name,
        seen: seen ?? 1,
        lastSeen: DateTime.now(),
      );
    }
  }

  void _commitBleScan() {
    final list = _bleScanMap.values.toList()
      ..sort((a, b) => b.rssi.compareTo(a.rssi));
    bleScanDevices = List.unmodifiable(list);
  }

  void _commitWlscan() {
    final list = _wlscanMap.values.toList()
      ..sort((a, b) => b.rssi.compareTo(a.rssi));
    wlscanAps = List.unmodifiable(list);
    // NEW/UPD/GONE mutate the visible AP list, so the UI must be told. Without
    // this, a screen with no other repaint source (the standalone WiFi Live
    // Scan tool) only refreshes on the ~1 Hz AIR telemetry tick. Coalesced.
    _scheduleNotify();
  }

  void _commitBleWar() {
    final list = _bleWarMap.values.toList()
      ..sort((a, b) => b.rssi.compareTo(a.rssi));
    bleWarDevices = List.unmodifiable(list);
  }

  void _commitSpectrum() {
    final list = _spectrumMap.values.toList()
      ..sort((a, b) => a.freq.compareTo(b.freq));
    spectrumChannels = List.unmodifiable(list);
  }

  void _commitNrfMj() {
    final list = _nrfMjMap.values.toList()
      ..sort((a, b) => a.idx.compareTo(b.idx));
    nrfMjTargets = List.unmodifiable(list);
  }

  void _parseLine(String line) {
    if (line.isEmpty) return;
    final temperatureMatch = _reChipTemperature.firstMatch(line);
    if (temperatureMatch != null) {
      final temperature = double.tryParse(temperatureMatch.group(1)!);
      if (temperature == null || !temperature.isFinite) {
        connectionIssue = 'Invalid chip temperature reading: $line';
        return;
      }
      chipTemperatureC = temperature;
      chipTemperatureUpdatedAt = DateTime.now();
      _temperatureNotificationPending = true;
      return;
    }
    final sysMatch = _reSys.firstMatch(line);
    if (sysMatch != null) {
      final cpuPercent = int.tryParse(sysMatch.group(1)!);
      if (cpuPercent != null) {
        sysCpuUsagePercent = cpuPercent.clamp(0, 100).toInt();
      }
      sysHeapFree = int.tryParse(sysMatch.group(2)!) ?? sysHeapFree;
      sysHeapTotal = int.tryParse(sysMatch.group(3)!) ?? sysHeapTotal;
      sysPsramFree = int.tryParse(sysMatch.group(4)!) ?? sysPsramFree;
      sysPsramTotal = int.tryParse(sysMatch.group(5)!) ?? sysPsramTotal;
      sysFlashFree = int.tryParse(sysMatch.group(6)!) ?? sysFlashFree;
      sysFlashTotal = int.tryParse(sysMatch.group(7)!) ?? sysFlashTotal;
      sysUpdatedAt = DateTime.now();
      _systemNotificationPending = true;
      return;
    }
    final fsLs = _reFsLs.firstMatch(line);
    if (fsLs != null) {
      fsTotal = int.tryParse(fsLs.group(1)!) ?? fsTotal;
      fsUsed = int.tryParse(fsLs.group(2)!) ?? fsUsed;
      _fsTmp.clear();
      fsListing = true;
      return;
    }
    final fsEnt = _reFsEnt.firstMatch(line);
    if (fsEnt != null) {
      _fsTmp.add(
        FsFile(path: fsEnt.group(1)!, size: int.tryParse(fsEnt.group(2)!) ?? 0),
      );
      return;
    }
    if (line == 'fs: LS end') {
      final list = _fsTmp.toList()..sort((a, b) => a.path.compareTo(b.path));
      fsFiles = List.unmodifiable(list);
      _fsTmp.clear();
      fsListing = false;
      return;
    }
    final fsInfo = _reFsInfo.firstMatch(line);
    if (fsInfo != null) {
      fsTotal = int.tryParse(fsInfo.group(1)!) ?? fsTotal;
      fsUsed = int.tryParse(fsInfo.group(2)!) ?? fsUsed;
      return;
    }
    final fsRm = _reFsRm.firstMatch(line);
    if (fsRm != null) {
      final result = fsRm.group(2)!;
      if (result != 'ok') {
        connectionIssue = 'fs delete ${fsRm.group(1)}: $result';
      }
      // On success the controller's fsDelete() re-requests `fs ls`.
      return;
    }
    if (line == 'fs: not ready') {
      connectionIssue = 'LittleFS not ready on device';
      return;
    }
    if (line == 'cli: command queue full; input dropped' ||
        line.startsWith('unknown: ') ||
        line == 'mode: unknown' ||
        line == 'sel: bad index' ||
        line.startsWith('t24: bad index') ||
        line.startsWith('t5: bad index') ||
        line.startsWith('sta: bad index') ||
        line.startsWith('sniff: bad ap_idx') ||
        line.startsWith('wl: bad mac') ||
        line.startsWith('wl: list full') ||
        line.startsWith('wl: already present') ||
        line.startsWith('wl: not found') ||
        line.startsWith('wl: unknown subcommand') ||
        line.startsWith('bl: bad mac') ||
        line.startsWith('bl: list full') ||
        line.startsWith('bl: already present') ||
        line.startsWith('bl: not found') ||
        line.startsWith('bl: unknown subcommand') ||
        line.contains('failed (storage)')) {
      connectionIssue = line;
      return;
    }
    // "attack: warning — task did not exit in 1s" is benign: the task does
    // exit once an in-flight rescan's blocking scan completes. Keep it in the
    // console log but don't raise a scary banner.
    if (line.startsWith('attack: warning — task did not exit')) {
      return;
    }
    if (line == 'attack: already running') {
      attacking = true;
      connectionIssue = line;
      return;
    }

    // ── nRF24 2.4 GHz radio ───────────────────────────────
    if (line.startsWith('nrf24: module ready') ||
        line == 'main: nrf24 module ready') {
      nrfPresent = true;
      nrfModuleKnown = true;
      return;
    }
    if (line.startsWith('nrf24: module not detected') ||
        line.startsWith('nrf24: driver init failed') ||
        line == 'main: nrf24 not available' ||
        line == 'nrf: module not available') {
      // Absence is not an error — no banner, just reflect it in the nRF UI.
      // Nothing can be running without the module, so clear optimistic flags.
      nrfPresent = false;
      nrfModuleKnown = true;
      nrfScanRunning = false;
      nrfJamRunning = false;
      nrfSniffRunning = false;
      nrfMjRunning = false;
      return;
    }
    final nrfStatus = _reNrfStatus.firstMatch(line);
    if (nrfStatus != null) {
      nrfPresent = true;
      nrfModuleKnown = true;
      nrfScanRunning = nrfStatus.group(1) == '1';
      nrfJamRunning = nrfStatus.group(2) == '1';
      nrfSniffRunning = nrfStatus.group(3) == '1';
      nrfMjRunning = nrfStatus.group(4) == '1';
      // group(5) radio_busy is derived locally via the nrfRadioBusy getter.
      return;
    }

    // nRF Scanner
    if (line == 'nrfscan: waterfall start') {
      nrfScanRunning = true;
      return;
    }
    if (line == 'nrfscan: waterfall stop') {
      nrfScanRunning = false;
      return;
    }
    final nsPeak = _reNrfScanPeak.firstMatch(line);
    if (nsPeak != null) {
      nrfScanRunning = true;
      nrfScanPeakCh = int.tryParse(nsPeak.group(2)!) ?? nrfScanPeakCh;
      nrfScanPeakRpdDetections = int.tryParse(nsPeak.group(3)!) ?? 0;
      nrfScanSamples = int.tryParse(nsPeak.group(4)!) ?? nrfScanSamples;
      return;
    }
    final nsListStart = _reNrfScanListStart.firstMatch(line);
    if (nsListStart != null) {
      _nrfScanTmp.clear();
      _nrfScanDumpSweep = int.tryParse(nsListStart.group(1)!) ?? 0;
      return;
    }
    final nsCh = _reNrfScanCh.firstMatch(line);
    if (nsCh != null) {
      _nrfScanTmp.add(
        NrfScanChannel(
          channel: int.tryParse(nsCh.group(1)!) ?? 0,
          rpdDetections: int.tryParse(nsCh.group(2)!) ?? 0,
          peak: nsCh.group(3) != null,
        ),
      );
      return;
    }
    if (line == 'nrfscan: LS end') {
      nrfScanChannels = List.unmodifiable(_nrfScanTmp.toList());
      nrfScanSweep = _nrfScanDumpSweep;
      _nrfScanTmp.clear();
      return;
    }
    final nPeakCli = _reNrfPeakCli.firstMatch(line);
    if (nPeakCli != null) {
      nrfPeakCh = int.tryParse(nPeakCli.group(1)!) ?? 0;
      nrfPeakRpdDetections = int.tryParse(nPeakCli.group(2)!) ?? 0;
      nrfScanSamples = int.tryParse(nPeakCli.group(3)!) ?? nrfScanSamples;
      return;
    }
    if (line == 'nrf: scan failed') {
      connectionIssue = line;
      return;
    }
    if (line == 'nrfscan: radio busy' || line == 'nrf: radio busy') {
      // Start was rejected — no scanner task is running.
      nrfScanRunning = false;
      connectionIssue = 'nRF24 radio busy';
      return;
    }

    // nRF Jammer
    final njStart = _reNrfJamStart.firstMatch(line);
    if (njStart != null) {
      nrfJamRunning = true;
      nrfJamTarget = njStart.group(1)!;
      nrfJamStrategy = njStart.group(2)!;
      nrfJamDurationSecs = int.tryParse(njStart.group(7)!) ?? 0;
      nrfJamWaterfall = njStart.group(8) == '1';
      nrfJamFrames = 0;
      nrfJamElapsed = 0;
      nrfJamCurrentCh = 0;
      nrfJamWfPeak = -1;
      return;
    }
    final njProg = _reNrfJamProg.firstMatch(line);
    if (njProg != null) {
      nrfJamRunning = true;
      nrfJamElapsed = int.tryParse(njProg.group(1)!) ?? nrfJamElapsed;
      nrfJamCurrentCh = int.tryParse(njProg.group(2)!) ?? nrfJamCurrentCh;
      nrfJamFrames = int.tryParse(njProg.group(3)!) ?? nrfJamFrames;
      return;
    }
    final njWf = _reNrfJamWf.firstMatch(line);
    if (njWf != null) {
      nrfJamWfPeak = int.tryParse(njWf.group(1)!) ?? nrfJamWfPeak;
      return;
    }
    final njStop = _reNrfJamStop.firstMatch(line);
    if (njStop != null) {
      nrfJamRunning = false;
      nrfJamFrames = int.tryParse(njStop.group(1)!) ?? nrfJamFrames;
      nrfJamElapsed = int.tryParse(njStop.group(2)!) ?? nrfJamElapsed;
      return;
    }
    if (line.startsWith('nrfjam: radio busy')) {
      // Start was rejected (radio owned elsewhere) — no jam task was created.
      nrfJamRunning = false;
      connectionIssue = line;
      return;
    }
    if (line == 'nrfjam: already running') {
      nrfJamRunning = true; // a jam task really is running
      connectionIssue = line;
      return;
    }
    if (line == 'nrfjam: not running') {
      nrfJamRunning = false;
      return;
    }

    // nRF Sniffer
    final nsfStart = _reNrfSniffStart.firstMatch(line);
    if (nsfStart != null) {
      nrfSniffRunning = true;
      nrfSniffChannel = int.tryParse(nsfStart.group(1)!) ?? nrfSniffChannel;
      nrfSniffCaptured = 0;
      nrfSniffDropped = 0;
      nrfSniffElapsed = 0;
      nrfSniffPackets.clear();
      return;
    }
    final nsfPkt = _reNrfSniffPkt.firstMatch(line);
    if (nsfPkt != null) {
      final idx = int.tryParse(nsfPkt.group(1)!) ?? 0;
      nrfSniffPackets.insert(
        0,
        NrfSniffPacket(
          idx: idx,
          channel: nrfSniffChannel,
          len: int.tryParse(nsfPkt.group(2)!) ?? 0,
          hex: nsfPkt.group(3)!.trim(),
          at: DateTime.now(),
        ),
      );
      if (nrfSniffPackets.length > _nrfSniffMax) nrfSniffPackets.removeLast();
      if (idx + 1 > nrfSniffCaptured) nrfSniffCaptured = idx + 1;
      return;
    }
    final nsfStat = _reNrfSniffStat.firstMatch(line);
    if (nsfStat != null) {
      nrfSniffElapsed = int.tryParse(nsfStat.group(1)!) ?? nrfSniffElapsed;
      nrfSniffCaptured = int.tryParse(nsfStat.group(2)!) ?? nrfSniffCaptured;
      nrfSniffDropped = int.tryParse(nsfStat.group(3)!) ?? nrfSniffDropped;
      return;
    }
    final nsfStop = _reNrfSniffStop.firstMatch(line);
    if (nsfStop != null) {
      nrfSniffRunning = false;
      nrfSniffCaptured = int.tryParse(nsfStop.group(1)!) ?? nrfSniffCaptured;
      nrfSniffDropped = int.tryParse(nsfStop.group(2)!) ?? nrfSniffDropped;
      return;
    }
    if (line.startsWith('nrf_sniff: LS count=')) {
      _inNrfSniffLs = true;
      _nrfSniffLsTmp.clear();
      return;
    }
    final nsfLs = _reNrfSniffLs.firstMatch(line);
    if (nsfLs != null) {
      _nrfSniffLsTmp.add(
        NrfSniffPacket(
          idx: int.tryParse(nsfLs.group(1)!) ?? 0,
          channel: int.tryParse(nsfLs.group(2)!) ?? 0,
          len: int.tryParse(nsfLs.group(3)!) ?? 0,
          hex: nsfLs.group(5)!.trim(),
          at: DateTime.now(),
        ),
      );
      return;
    }
    if (line == 'nrf_sniff: LS end') {
      if (_inNrfSniffLs) {
        nrfSniffPackets
          ..clear()
          ..addAll(_nrfSniffLsTmp.reversed); // newest idx first
        if (nrfSniffPackets.length > _nrfSniffMax) {
          nrfSniffPackets.removeRange(_nrfSniffMax, nrfSniffPackets.length);
        }
        _nrfSniffLsTmp.clear();
        _inNrfSniffLs = false;
      }
      return;
    }
    if (line == 'nrf_sniff: cleared') {
      nrfSniffPackets.clear();
      nrfSniffCaptured = 0;
      nrfSniffDropped = 0;
      return;
    }
    if (line.startsWith('nrf_sniff: replay start')) {
      nrfReplayActive = true;
      return;
    }
    if (line.startsWith('nrf_sniff: replay done')) {
      nrfReplayActive = false;
      return;
    }
    if (line.startsWith('nrf_sniff: replay fail') ||
        line == 'nrf_sniff: replay already active' ||
        line == 'nrf_sniff: replay invalid idx' ||
        line == 'nrf_sniff: radio busy') {
      // Firmware emits "radio busy" for a rejected sniffer start AND a rejected
      // replay; neither is actually running on our behalf, so clear both.
      nrfReplayActive = false;
      nrfSniffRunning = false;
      connectionIssue = line;
      return;
    }

    // nRF MouseJack
    final mjStart = _reNrfMjStart.firstMatch(line);
    if (mjStart != null) {
      nrfMjRunning = true;
      nrfMjElapsed = 0;
      nrfMjFound = 0;
      _nrfMjMap.clear();
      nrfMjTargets = const [];
      return;
    }
    final mjTgt = _reNrfMjTarget.firstMatch(line);
    if (mjTgt != null) {
      final addr = mjTgt.group(2)!.toUpperCase();
      _nrfMjMap[addr] = NrfMjTarget(
        idx: int.tryParse(mjTgt.group(1)!) ?? 0,
        addr: addr,
        channel: int.tryParse(mjTgt.group(3)!) ?? 0,
        hits: 1,
        isLogitech: mjTgt.group(4) == 'logitech',
      );
      nrfMjFound = _nrfMjMap.length;
      _commitNrfMj();
      return;
    }
    final mjStat = _reNrfMjStat.firstMatch(line);
    if (mjStat != null) {
      nrfMjElapsed = int.tryParse(mjStat.group(1)!) ?? nrfMjElapsed;
      nrfMjFound = int.tryParse(mjStat.group(2)!) ?? nrfMjFound;
      return;
    }
    final mjStop = _reNrfMjStop.firstMatch(line);
    if (mjStop != null) {
      nrfMjRunning = false;
      nrfMjFound = int.tryParse(mjStop.group(1)!) ?? nrfMjFound;
      return;
    }
    if (line.startsWith('nrf_mj: LS count=')) {
      _nrfMjMap.clear();
      return;
    }
    final mjLs = _reNrfMjLs.firstMatch(line);
    if (mjLs != null) {
      final addr = mjLs.group(2)!.toUpperCase();
      _nrfMjMap[addr] = NrfMjTarget(
        idx: int.tryParse(mjLs.group(1)!) ?? 0,
        addr: addr,
        channel: int.tryParse(mjLs.group(3)!) ?? 0,
        hits: int.tryParse(mjLs.group(4)!) ?? 0,
        isLogitech: mjLs.group(5) == 'logitech',
      );
      nrfMjFound = _nrfMjMap.length;
      _commitNrfMj();
      return;
    }
    if (line == 'nrf_mj: LS end') {
      _commitNrfMj();
      return;
    }
    if (line == 'nrf_mj: cleared') {
      _nrfMjMap.clear();
      nrfMjTargets = const [];
      nrfMjFound = 0;
      return;
    }
    if (line.startsWith('nrf_mj: inject target=')) {
      nrfMjInjecting = true;
      return;
    }
    if (line.startsWith('nrf_mj: inject done')) {
      nrfMjInjecting = false;
      return;
    }
    if (line == 'nrf_mj: radio busy') {
      // Rejected scan start or inject — neither is running on our behalf.
      nrfMjInjecting = false;
      nrfMjRunning = false;
      connectionIssue = line;
      return;
    }

    // ── Infrared ──────────────────────────────────────────
    if (line.startsWith('ir: module ready') ||
        line == 'main: ir module ready') {
      irReady = true;
      irModuleKnown = true;
      return;
    }
    if (line == 'ir: module not available' ||
        line == 'main: ir module init failed' ||
        line.startsWith('ir: rmt init failed') ||
        line == 'ir: not ready') {
      irReady = false;
      irModuleKnown = true;
      return;
    }
    if (line == 'ir_rx: START') {
      irRxRunning = true;
      return;
    }
    if (line == 'ir_rx: STOPPED') {
      irRxRunning = false;
      return;
    }
    if (line == 'ir_rx: CLEARED') {
      irDetected.clear();
      irSpectrumBins = const [];
      irSpectrumTotalUs = 0;
      return;
    }
    if (line == 'ir_spec: ON') {
      irSpectrumOn = true;
      return;
    }
    if (line == 'ir_spec: OFF') {
      irSpectrumOn = false;
      return;
    }
    final irSig = _reIrSig.firstMatch(line);
    if (irSig != null) {
      final sig = IrDetectedSignal(
        proto: irSig.group(1)!,
        address: int.tryParse(irSig.group(2)!, radix: 16) ?? 0,
        command: int.tryParse(irSig.group(3)!, radix: 16) ?? 0,
        bits: int.tryParse(irSig.group(4)!) ?? 0,
        rawCount: int.tryParse(irSig.group(5)!) ?? 0,
        bytes: irSig.group(6)!,
        at: DateTime.now(),
        captureId: int.tryParse(irSig.group(7)!) ?? 0,
      );
      irDetected.insert(0, sig);
      while (irDetected.length > _irDetectedMax) {
        irDetected.removeLast();
      }
      return;
    }
    final irRaw = _reIrRaw.firstMatch(line);
    if (irRaw != null) {
      final off = int.tryParse(irRaw.group(2)!) ?? 0;
      final values = irRaw
          .group(3)!
          .trim()
          .split(RegExp(r'\s+'))
          .map(int.tryParse)
          .whereType<int>()
          .toList();
      if (irDetected.isNotEmpty) {
        final sig = irDetected.first;
        if (off == 0) sig.raw.clear();
        sig.raw.addAll(values);
        sig.rawCount = int.tryParse(irRaw.group(1)!) ?? sig.rawCount;
      }
      return;
    }
    final irWave = _reIrWave.firstMatch(line);
    if (irWave != null) {
      irSpectrumTotalUs = int.tryParse(irWave.group(2)!) ?? 0;
      irSpectrumBins = irWave
          .group(3)!
          .trim()
          .split(RegExp(r'\s+'))
          .map(int.tryParse)
          .whereType<int>()
          .toList(growable: false);
      return;
    }
    final irUniv = _reIrUniv.firstMatch(line);
    if (irUniv != null) {
      irUnivRunning = true;
      irUnivStopped = false;
      irUnivCategory = irUniv.group(1)!;
      irUnivButton = irUniv.group(2)!;
      irUnivTotal = int.tryParse(irUniv.group(3)!) ?? 0;
      irUnivSent = 0;
      irUnivFailed = 0;
      irUnivBrand = '';
      irUnivError = null;
      if (irUnivTotal == 0) irUnivRunning = false;
      return;
    }
    final irTx = _reIrTx.firstMatch(line);
    if (irTx != null) {
      irUnivRunning = true;
      irUnivSent++;
      irUnivTotal = int.tryParse(irTx.group(2)!) ?? irUnivTotal;
      irUnivBrand = irTx.group(3) ?? irTx.group(4)!;
      return;
    }
    final irTxFail = _reIrTxFail.firstMatch(line);
    if (irTxFail != null) {
      irUnivTotal = int.tryParse(irTxFail.group(2)!) ?? irUnivTotal;
      irUnivFailed++;
      irUnivError =
          'IR transmission failed for ${irTxFail.group(3)} '
          '(error ${irTxFail.group(4)}).';
      return;
    }
    final irUnivDone = _reIrUnivDone.firstMatch(line);
    if (irUnivDone != null) {
      irUnivRunning = false;
      irUnivStopping = false;
      irUnivStopped = irUnivDone.group(5) == '1';
      irUnivSent = int.tryParse(irUnivDone.group(3)!) ?? irUnivSent;
      irUnivFailed = int.tryParse(irUnivDone.group(4) ?? '') ?? irUnivFailed;
      return;
    }
    if (line.startsWith('ir: no built-in codes')) {
      irUnivError =
          'No built-in codes for this device — use the Receiver to capture & save it.';
      irUnivRunning = false;
      irUnivStopping = false;
      return;
    }
    if (line.startsWith('ir: universal start failed')) {
      irUnivRunning = false;
      irUnivStopping = false;
      irUnivError = line;
      connectionIssue = line;
      return;
    }
    if (line == 'ir: univ stop requested') {
      irUnivStopping = true;
      return;
    }
    if (line == 'ir: univ stop pending') {
      irUnivStopping = true;
      return;
    }
    if (line == 'ir: univ stop idle') {
      irUnivRunning = false;
      irUnivStopping = false;
      return;
    }
    final irEmit = _reIrEmit.firstMatch(line);
    if (irEmit != null) {
      irEmulateRunning = true;
      irEmulateName = irEmit.group(1)!;
      irEmulateSent = 0;
      return;
    }
    final irEmitDone = _reIrEmitDone.firstMatch(line);
    if (irEmitDone != null) {
      irEmulateRunning = false;
      irEmulateStopping = false;
      irEmulateSent = int.tryParse(irEmitDone.group(2)!) ?? irEmulateSent;
      return;
    }
    if (line.startsWith('ir: EMIT_FAIL')) {
      connectionIssue = line;
      return;
    }
    if (line.startsWith('ir: emit start failed')) {
      irEmulateRunning = false;
      irEmulateStopping = false;
      connectionIssue = line;
      return;
    }
    if (line == 'ir_emit: STOPPED') {
      irEmulateStopping = true;
      return;
    }
    if (line == 'ir_emit: STOP_PENDING') {
      irEmulateStopping = true;
      return;
    }
    if (line == 'ir_emit: STOP_IDLE') {
      irEmulateRunning = false;
      irEmulateStopping = false;
      return;
    }
    if (line.startsWith('ir: emit load failed')) {
      irEmulateRunning = false;
      irEmulateStopping = false;
      connectionIssue = 'IR emulate failed: saved signal could not be loaded.';
      return;
    }
    final irSavedMatch = _reIrSaved.firstMatch(line);
    if (irSavedMatch != null) {
      connectionIssue = null;
      unawaited(_sendCommand('ir ls').catchError((Object _) {}));
      return;
    }
    if (line == 'ir: NOCAP') {
      connectionIssue = 'No captured IR signal yet — start the receiver first.';
      return;
    }
    final irSavedCount = _reIrSavedCount.firstMatch(line);
    if (irSavedCount != null) {
      _irSavedTmp.clear();
      return;
    }
    final irSavedEnt = _reIrSavedEnt.firstMatch(line);
    if (irSavedEnt != null) {
      _irSavedTmp.add(
        IrSavedEntry(
          name: irSavedEnt.group(1)!,
          sizeBytes: int.tryParse(irSavedEnt.group(2)!) ?? 0,
          proto: irSavedEnt.group(3)!,
          address: int.tryParse(irSavedEnt.group(4)!, radix: 16) ?? 0,
          command: int.tryParse(irSavedEnt.group(5)!, radix: 16) ?? 0,
          rawCount: int.tryParse(irSavedEnt.group(6)!) ?? 0,
        ),
      );
      return;
    }
    if (line == 'ir_saved: LS end') {
      irSaved = List.unmodifiable(_irSavedTmp.toList());
      _irSavedTmp.clear();
      return;
    }
    final irStatus = _reIrStatus.firstMatch(line);
    if (irStatus != null) {
      irModuleKnown = true;
      irReady = irStatus.group(1) == '1';
      irRxRunning = irStatus.group(2) == '1';
      irSpectrumOn = irStatus.group(3) == '1';
      irUnivRunning = irStatus.group(4) == '1';
      irEmulateRunning = irStatus.group(5) == '1';
      if (!irUnivRunning) irUnivStopping = false;
      if (!irEmulateRunning) irEmulateStopping = false;
      irFsUsed = int.tryParse(irStatus.group(7)!) ?? irFsUsed;
      irFsTotal = int.tryParse(irStatus.group(8)!) ?? irFsTotal;
      return;
    }

    // ── BLE Spam ──────────────────────────────────────────
    final bsStart = _reBleSpamStart.firstMatch(line);
    if (bsStart != null) {
      bleSpamRunning = true;
      bleSpamType = BleSpamType.fromId(bsStart.group(1)!);
      bleSpamDelayMs = int.tryParse(bsStart.group(2)!) ?? 50;
      bleSpamDurationSecs = int.tryParse(bsStart.group(3)!) ?? 0;
      bleSpamStartedAt = DateTime.now();
      bleSpamFrames = 0;
      bleSpamFps = 0;
      return;
    }
    final bsStatus = _reBleSpamStatus.firstMatch(line);
    if (bsStatus != null) {
      bleSpamRunning = true;
      bleSpamType = BleSpamType.fromId(bsStatus.group(1)!);
      bleSpamDelayMs = int.tryParse(bsStatus.group(2)!) ?? bleSpamDelayMs;
      bleSpamFrames = int.tryParse(bsStatus.group(3)!) ?? bleSpamFrames;
      bleSpamStartedAt ??= DateTime.now();
      return;
    }
    final bsProg = _reBleSpamProgress.firstMatch(line);
    if (bsProg != null) {
      bleSpamDelayMs = int.tryParse(bsProg.group(3)!) ?? bleSpamDelayMs;
      bleSpamFrames = int.tryParse(bsProg.group(4)!) ?? bleSpamFrames;
      bleSpamFps = int.tryParse(bsProg.group(5)!) ?? bleSpamFps;
      return;
    }
    if (_reBleSpamStop.hasMatch(line)) {
      bleSpamRunning = false;
      bleSpamStartedAt = null;
      return;
    }
    if (line.startsWith('blespam: address resolution failed')) {
      connectionIssue = line;
      bleSpamRunning = false;
      bleSpamStartedAt = null;
      return;
    }

    // ── BLE Scan ──────────────────────────────────────────
    final bscStart = _reBleScanStart.firstMatch(line);
    if (bscStart != null) {
      bleScanRunning = true;
      bleScanActive = bscStart.group(2) == '1';
      bleScanElapsed = 0;
      bleScanFound = 0;
      _bleScanMap.clear();
      bleScanDevices = const [];
      return;
    }
    final bscDev = _reBleScanDev.firstMatch(line);
    if (bscDev != null) {
      _upsertBleScan(
        addr: bscDev.group(1)!.toUpperCase(),
        rssi: int.tryParse(bscDev.group(2)!) ?? 0,
        type: int.tryParse(bscDev.group(3)!) ?? 0,
        addrType: int.tryParse(bscDev.group(4)!) ?? 0,
        mfg: bscDev.group(5)!,
        name: bscDev.group(6)!,
      );
      bleScanFound = _bleScanMap.length;
      _commitBleScan();
      return;
    }
    final bscLs = _reBleScanLs.firstMatch(line);
    if (bscLs != null) {
      _upsertBleScan(
        addr: bscLs.group(1)!.toUpperCase(),
        rssi: int.tryParse(bscLs.group(2)!) ?? 0,
        type: int.tryParse(bscLs.group(3)!) ?? 0,
        addrType: int.tryParse(bscLs.group(4)!) ?? 0,
        mfg: bscLs.group(5)!,
        name: bscLs.group(7)!,
        seen: int.tryParse(bscLs.group(6)!),
      );
      bleScanFound = _bleScanMap.length;
      _commitBleScan();
      return;
    }
    final bscProg = _reBleScanProgress.firstMatch(line);
    if (bscProg != null) {
      bleScanElapsed = int.tryParse(bscProg.group(1)!) ?? bleScanElapsed;
      bleScanFound = int.tryParse(bscProg.group(2)!) ?? bleScanFound;
      return;
    }
    final bscStop = _reBleScanStopped.firstMatch(line);
    if (bscStop != null) {
      bleScanRunning = false;
      bleScanFound = int.tryParse(bscStop.group(1)!) ?? bleScanFound;
      return;
    }
    if (line == 'blescan: cleared') {
      _bleScanMap.clear();
      bleScanDevices = const [];
      bleScanFound = 0;
      return;
    }
    if (line == 'blescan: LS (empty)') {
      _bleScanMap.clear();
      bleScanDevices = const [];
      bleScanFound = 0;
      return;
    }
    if (line.startsWith('blescan: LS count=') || line == 'blescan: LS end') {
      return;
    }
    if (line.startsWith('blescan: BLE not synced') ||
        line.startsWith('blescan: ble_gap_disc') ||
        line.startsWith('blescan: already running')) {
      connectionIssue = line;
      return;
    }

    // ── Monitor ───────────────────────────────────────────
    final monStart = _reMonitorStart.firstMatch(line);
    if (monStart != null) {
      monitorRunning = true;
      monitorChannel = int.tryParse(monStart.group(1)!) ?? 0;
      monitorBssid = monStart.group(2)!.toUpperCase();
      monitorIs5ghz = monStart.group(3) == '5';
      monitorFps = 0;
      monitorFrames = 0;
      monitorMgmt = 0;
      monitorData = 0;
      monitorStas = 0;
      return;
    }
    final monStat = _reMonitorStat.firstMatch(line);
    if (monStat != null) {
      monitorRunning = true;
      monitorRssi = int.tryParse(monStat.group(1)!) ?? monitorRssi;
      monitorFps = int.tryParse(monStat.group(2)!) ?? 0;
      monitorFrames = int.tryParse(monStat.group(3)!) ?? monitorFrames;
      monitorMgmt = int.tryParse(monStat.group(4)!) ?? monitorMgmt;
      monitorData = int.tryParse(monStat.group(5)!) ?? monitorData;
      monitorStas = int.tryParse(monStat.group(6)!) ?? monitorStas;
      return;
    }
    if (line == 'monitor: STOPPED') {
      monitorRunning = false;
      monitorFps = 0;
      return;
    }
    if (line.startsWith('monitor: bad ap_idx') ||
        line.startsWith('monitor: cannot tune') ||
        line.startsWith('monitor: already running') ||
        line.startsWith('monitor: stop the active attack') ||
        line.startsWith('monitor: task create failed')) {
      connectionIssue = line;
      monitorRunning = false;
      return;
    }

    // ── WiFi Live Scan ────────────────────────────────────
    final wlStart = _reWlscanStart.firstMatch(line);
    if (wlStart != null) {
      wlscanRunning = true;
      wlscanMode = wlStart.group(1)!;
      wlscanCycles = 0;
      wlscanUnique = 0;
      _wlscanMap.clear();
      wlscanAps = const [];
      _resetAir();
      return;
    }
    final wlAir = _reWlscanAir.firstMatch(line);
    if (wlAir != null) {
      final winMs = int.tryParse(wlAir.group(1)!) ?? 1000;
      final win = winMs <= 0 ? 1000 : winMs;
      int rate(int grp) {
        final n = int.tryParse(wlAir.group(grp)!) ?? 0;
        return (n * 1000 / win).round();
      }

      airPps = rate(2);
      airMgmtPps = rate(3);
      airDataPps = rate(4);
      airBeaconPps = rate(5);
      airProbePps = rate(6);
      airDeauthPps = rate(7);
      airDisassocPps = rate(8);
      airAuthPps = rate(9);
      airEapolPps = rate(10);
      airAlert = (wlAir.group(12) ?? '0') != '0';
      airUpdatedAt = DateTime.now();
      airRev++;

      airRateHistory.add(airPps);
      airThreatHistory.add(airDeauthPps + airDisassocPps);
      while (airRateHistory.length > _airHistoryLen) {
        airRateHistory.removeAt(0);
      }
      while (airThreatHistory.length > _airHistoryLen) {
        airThreatHistory.removeAt(0);
      }

      wlscanRunning = true;
      _scheduleNotify();
      return;
    }
    final wlNew = _reWlscanNew.firstMatch(line);
    if (wlNew != null) {
      final bssid = wlNew.group(2)!.toUpperCase();
      final ssid = wlNew.group(1)!.trim();
      _wlscanMap[bssid] = WlscanAp(
        bssid: bssid,
        ssid: ssid == '(hidden)' ? '' : ssid,
        channel: int.tryParse(wlNew.group(3)!) ?? 0,
        rssi: int.tryParse(wlNew.group(4)!) ?? 0,
        is5ghz: wlNew.group(5) == '5',
        lastSeen: DateTime.now(),
      );
      _commitWlscan();
      return;
    }
    final wlUpd = _reWlscanUpd.firstMatch(line);
    if (wlUpd != null) {
      final ap = _wlscanMap[wlUpd.group(1)!.toUpperCase()];
      if (ap != null) {
        ap.rssi = int.tryParse(wlUpd.group(3)!) ?? ap.rssi;
        ap.lastSeen = DateTime.now();
        _commitWlscan();
      }
      return;
    }
    final wlGone = _reWlscanGone.firstMatch(line);
    if (wlGone != null) {
      if (_wlscanMap.remove(wlGone.group(1)!.toUpperCase()) != null) {
        _commitWlscan();
      }
      return;
    }
    final wlCycle = _reWlscanCycle.firstMatch(line);
    if (wlCycle != null) {
      wlscanRunning = true;
      wlscanCycles = int.tryParse(wlCycle.group(1)!) ?? wlscanCycles;
      wlscanUnique = _wlscanMap.length;
      return;
    }
    if (line.startsWith('wlscan: STOPPED')) {
      wlscanRunning = false;
      return;
    }
    if (line.startsWith('wlscan: already running') ||
        line.startsWith('wlscan: cycle error')) {
      connectionIssue = line;
      return;
    }

    // ── BLE Wardriving ────────────────────────────────────
    final bwStart = _reBleWarStart.firstMatch(line);
    if (bwStart != null) {
      bleWarRunning = true;
      bleWarActive = bwStart.group(1) == '1';
      bleWarCycles = 0;
      bleWarUnique = 0;
      bleFindMyObservations = 0;
      _bleWarMap.clear();
      _bleFindMySeenByAddr.clear();
      bleWarDevices = const [];
      return;
    }
    final bwNew = _reBleWarNew.firstMatch(line);
    if (bwNew != null) {
      final addr = bwNew.group(1)!.toUpperCase();
      _bleWarMap[addr] = BleWarDevice(
        addr: addr,
        rssi: int.tryParse(bwNew.group(2)!) ?? 0,
        seen: 0,
        addrType: int.tryParse(bwNew.group(3)!) ?? 1,
        priv: bwNew.group(4)!,
        vendor: bwNew.group(5)!,
        dtype: bwNew.group(6)!,
        company: bwNew.group(7)!,
        name: bwNew.group(8)!.trim(),
        firstSeen: DateTime.now(),
        lastSeen: DateTime.now(),
      );
      _bleFindMySeenByAddr[addr] = 0;
      _commitBleWar();
      return;
    }
    final bwMeta = _reBleWarMeta.firstMatch(line);
    if (bwMeta != null) {
      final dev = _bleWarMap[bwMeta.group(1)!.toUpperCase()];
      if (dev != null) {
        dev.vendor = bwMeta.group(2)!;
        dev.dtype = bwMeta.group(3)!;
        dev.company = bwMeta.group(4)!;
        dev.name = bwMeta.group(5)!.trim();
        _commitBleWar();
      }
      return;
    }
    final bwUpd = _reBleWarUpd.firstMatch(line);
    if (bwUpd != null) {
      final dev = _bleWarMap[bwUpd.group(1)!.toUpperCase()];
      if (dev != null) {
        dev.rssi = int.tryParse(bwUpd.group(3)!) ?? dev.rssi;
        dev.lastSeen = DateTime.now();
        _commitBleWar();
      }
      return;
    }
    final findMyObs = _reBleFindMyObs.firstMatch(line);
    if (findMyObs != null) {
      final addr = findMyObs.group(1)!.toUpperCase();
      final dev = _bleWarMap[addr];
      if (dev != null &&
          dev.vendor == 'apple' &&
          dev.company.toUpperCase() == '004C' &&
          dev.dtype == 'findmy') {
        final seen = int.tryParse(findMyObs.group(2)!) ?? dev.seen;
        final previous = _bleFindMySeenByAddr[addr] ?? 0;
        bleFindMyObservations += seen >= previous ? seen - previous : seen;
        _bleFindMySeenByAddr[addr] = seen;
        dev.seen = seen;
        dev.rssi = int.tryParse(findMyObs.group(3)!) ?? dev.rssi;
        dev.lastSeen = DateTime.now();
        dev.isFindMyAirTagFormat = true;
        _commitBleWar();
      }
      return;
    }
    final findMySaved = _reBleFindMySaved.firstMatch(line);
    if (findMySaved != null) {
      final completer = _bleFindMySaveCompleter;
      if (completer != null && !completer.isCompleted) {
        completer.complete(findMySaved.group(1)!);
      }
      return;
    }
    final findMySaveError = _reBleFindMySaveError.firstMatch(line);
    if (findMySaveError != null) {
      final message =
          'Firmware could not save the Find My observation '
          '(error ${findMySaveError.group(1)}).';
      final error = StateError(message);
      connectionIssue = message;
      final completer = _bleFindMySaveCompleter;
      if (completer != null && !completer.isCompleted) {
        completer.completeError(error, StackTrace.current);
      }
      return;
    }
    final bwGone = _reBleWarGone.firstMatch(line);
    if (bwGone != null) {
      final address = bwGone.group(1)!.toUpperCase();
      _bleFindMySeenByAddr.remove(address);
      if (_bleWarMap.remove(address) != null) {
        _commitBleWar();
      }
      return;
    }
    final bwCycle = _reBleWarCycle.firstMatch(line);
    if (bwCycle != null) {
      bleWarRunning = true;
      bleWarCycles = int.tryParse(bwCycle.group(1)!) ?? bleWarCycles;
      bleWarUnique = _bleWarMap.length;
      return;
    }
    if (line.startsWith('blewar: STOPPED')) {
      bleWarRunning = false;
      return;
    }
    if (line.startsWith('blewar: already running') ||
        line.startsWith('blewar: BLE not synced') ||
        line.startsWith('blewar: ble_gap_disc')) {
      connectionIssue = line;
      return;
    }

    // ── WiFi Spectrum ─────────────────────────────────────
    if (line.startsWith('spectrum: START')) {
      spectrumRunning = true;
      spectrumSweep = 0;
      _spectrumMap.clear();
      _spectrumPending.clear();
      spectrumChannels = const [];
      return;
    }
    final specCh = _reSpecCh.firstMatch(line);
    if (specCh != null) {
      final ch = int.tryParse(specCh.group(1)!) ?? 0;
      _spectrumPending[ch] = SpecChannel(
        channel: ch,
        freq: int.tryParse(specCh.group(2)!) ?? 0,
        rssi: int.tryParse(specCh.group(3)!) ?? -128,
        peakRssi:
            int.tryParse(specCh.group(7)!) ??
            int.tryParse(specCh.group(3)!) ??
            -128,
        hasMeanRssi: specCh.group(7) != null,
        count: int.tryParse(specCh.group(4)!) ?? 0,
        is5ghz: specCh.group(5) == '5',
        valid: specCh.group(6) == '1',
        sampledAt: DateTime.now(),
      );
      _spectrumMap[ch] = _spectrumPending[ch]!;
      _commitSpectrum();
      return;
    }
    final specSweep = _reSpecSweep.firstMatch(line);
    if (specSweep != null) {
      spectrumRunning = true;
      spectrumSweep = int.tryParse(specSweep.group(1)!) ?? spectrumSweep;
      _spectrumMap
        ..clear()
        ..addAll(_spectrumPending);
      _spectrumPending.clear();
      _commitSpectrum();
      return;
    }
    if (line.startsWith('spectrum: STOPPED')) {
      spectrumRunning = false;
      return;
    }
    if (line.startsWith('spectrum: already running') ||
        line.startsWith('spectrum: task create failed')) {
      connectionIssue = line;
      return;
    }

    // ── BLE Spectrum ──────────────────────────────────────
    if (line.startsWith('blespectrum: START')) {
      bleSpectrumRunning = true;
      bleSpectrumBins = List<int>.filled(10, 0, growable: false);
      bleSpectrumMean = null;
      bleSpectrumPeak = -100;
      bleSpectrumTotal = 0;
      bleSpectrumFrameId = 0;
      return;
    }
    final bsf = _reBleSpecFrame.firstMatch(line);
    if (bsf != null) {
      bleSpectrumRunning = true;
      bleSpectrumPeak = int.tryParse(bsf.group(2)!) ?? bleSpectrumPeak;
      bleSpectrumMean = int.tryParse(bsf.group(1) ?? '');
      bleSpectrumTotal = int.tryParse(bsf.group(3)!) ?? 0;
      final parts = bsf.group(4)!.split(',');
      final bins = List<int>.filled(10, 0, growable: false);
      for (var i = 0; i < parts.length && i < 10; i++) {
        bins[i] = int.tryParse(parts[i]) ?? 0;
      }
      bleSpectrumBins = bins;
      bleSpectrumFrameId++;
      return;
    }
    if (line.startsWith('blespectrum: STOPPED')) {
      bleSpectrumRunning = false;
      return;
    }
    if (line.startsWith('blespectrum: BLE not synced') ||
        line.startsWith('blespectrum: ble_gap_disc') ||
        line.startsWith('blespectrum: already running') ||
        line.startsWith('blespectrum: task create failed')) {
      connectionIssue = line;
      bleSpectrumRunning = false;
      return;
    }

    // ── WiFi Spam ─────────────────────────────────────────
    final wsStart = _reWspamStart.firstMatch(line);
    if (wsStart != null) {
      wspamRunning = true;
      wspamMode = wsStart.group(1)!;
      wspamRate = wsStart.group(2)!;
      wspamDual = wsStart.group(3) == 'dual';
      wspamTotal = 0;
      wspamFps = 0;
      wspamCurrentSsid = '';
      wspamFeed.clear();
      return;
    }
    final wsStat = _reWspamStat.firstMatch(line);
    if (wsStat != null) {
      wspamRunning = true;
      wspamTotal = int.tryParse(wsStat.group(2)!) ?? wspamTotal;
      wspamFps = int.tryParse(wsStat.group(3)!) ?? wspamFps;
      wspamChannel = int.tryParse(wsStat.group(4)!) ?? wspamChannel;
      wspamCurrentSsid = wsStat.group(5)!;
      return;
    }
    final wsSsid = _reWspamSsid.firstMatch(line);
    if (wsSsid != null) {
      wspamChannel = int.tryParse(wsSsid.group(1)!) ?? wspamChannel;
      final name = wsSsid.group(2)!;
      wspamCurrentSsid = name;
      if (wspamFeed.isEmpty || wspamFeed.first != name) {
        wspamFeed.insert(0, name);
        if (wspamFeed.length > _wspamFeedMax) wspamFeed.removeLast();
      }
      return;
    }
    final wsStop = _reWspamStop.firstMatch(line);
    if (wsStop != null) {
      wspamRunning = false;
      wspamTotal = int.tryParse(wsStop.group(1)!) ?? wspamTotal;
      wspamFps = 0;
      return;
    }
    if (line.startsWith('wspam: LS count=')) {
      _wspamCustomTmp.clear();
      return;
    }
    final wsLs = _reWspamLs.firstMatch(line);
    if (wsLs != null) {
      _wspamCustomTmp.add(wsLs.group(1)!);
      return;
    }
    if (line == 'wspam: LS end') {
      wspamCustom = List.unmodifiable(_wspamCustomTmp.toList());
      _wspamCustomTmp.clear();
      return;
    }
    if (line.startsWith('wspam: already running') ||
        line.startsWith('wspam: custom mode but no SSIDs') ||
        line.startsWith('wspam: add failed')) {
      connectionIssue = line;
      return;
    }
    if (line.startsWith('wspam: added ') ||
        line.contains('already exists') && line.startsWith('wspam:')) {
      unawaited(_sendCommand('wspam ls').catchError((Object _) {}));
      return;
    }

    // ── Evil Portal ───────────────────────────────────────
    final pStart = _rePortalStart.firstMatch(line);
    if (pStart != null) {
      portalRunning = true;
      portalSsid = pStart.group(1)!;
      portalTemplate = pStart.group(2)!;
      portalChannel = int.tryParse(pStart.group(3)!) ?? portalChannel;
      portalClients = 0;
      portalHits = 0;
      portalUp = 0;
      portalCredsCount = 0;
      return;
    }
    final pStatRun = _rePortalStatus.firstMatch(line);
    if (pStatRun != null) {
      portalRunning = true;
      portalSsid = pStatRun.group(1)!;
      portalTemplate = pStatRun.group(2)!;
      portalChannel = int.tryParse(pStatRun.group(3)!) ?? portalChannel;
      portalUp = int.tryParse(pStatRun.group(4)!) ?? portalUp;
      portalClients = int.tryParse(pStatRun.group(5)!) ?? portalClients;
      portalHits = int.tryParse(pStatRun.group(6)!) ?? portalHits;
      portalCredsCount = int.tryParse(pStatRun.group(7)!) ?? portalCredsCount;
      return;
    }
    if (line.startsWith('portal: STATUS idle')) {
      portalRunning = false;
      return;
    }
    final pStat = _rePortalStat.firstMatch(line);
    if (pStat != null) {
      portalRunning = true;
      portalUp = int.tryParse(pStat.group(1)!) ?? portalUp;
      portalClients = int.tryParse(pStat.group(2)!) ?? portalClients;
      portalHits = int.tryParse(pStat.group(3)!) ?? portalHits;
      portalCredsCount = int.tryParse(pStat.group(4)!) ?? portalCredsCount;
      return;
    }
    final pStop = _rePortalStop.firstMatch(line);
    if (pStop != null) {
      portalRunning = false;
      portalUp = int.tryParse(pStop.group(1)!) ?? portalUp;
      portalClients = 0;
      portalCredsCount = int.tryParse(pStop.group(3)!) ?? portalCredsCount;
      return;
    }
    if (line.startsWith('portal: TMPL ls')) {
      _portalTemplatesTmp.clear();
      return;
    }
    final pTmpl = _rePortalTmpl.firstMatch(line);
    if (pTmpl != null) {
      final id = pTmpl.group(1)!;
      final isCustom = pTmpl.group(4) == '1';
      _portalTemplatesTmp.add(
        PortalTemplate(
          id,
          isCustom ? 'Custom: $id' : pTmpl.group(2)!,
          pTmpl.group(3)!,
          PortalTemplate.iconFor(isCustom ? 'custom' : id),
          isCustom: isCustom,
        ),
      );
      return;
    }
    if (line == 'portal: TMPL end') {
      portalTemplates = List.unmodifiable(_portalTemplatesTmp.toList());
      _portalTemplatesTmp.clear();
      return;
    }
    if (line.startsWith('portal: CREDS count=')) {
      // Start of a re-dump: reset the local list so it mirrors the device ring.
      portalCreds.clear();
      return;
    }
    final pCred = _rePortalCred.firstMatch(line);
    if (pCred != null) {
      portalCreds.insert(
        0,
        PortalCred(
          template: pCred.group(1)!,
          ip: pCred.group(2)!,
          user: pCred.group(3)!,
          pass: pCred.group(4)!,
          at: DateTime.now(),
        ),
      );
      if (portalCreds.length > _portalCredsMax) portalCreds.removeLast();
      return;
    }
    if (line == 'portal: CREDS cleared' || line == 'portal: CREDS end') {
      return;
    }
    if (line.startsWith('portal: ERR ')) {
      connectionIssue = line.replaceFirst('portal: ', 'Portal: ');
      portalRunning = false; // start failed
      return;
    }
    if (line.startsWith('portal: already running')) {
      portalRunning = true;
      return;
    }

    // ── Deauth All ────────────────────────────────────────
    final daStart = _reDeauthAllStart.firstMatch(line);
    if (daStart != null) {
      attacking = true;
      attackMode = 'deauthall';
      deauthAllStartedAt = DateTime.now();
      deauthAllDurationSecs = int.tryParse(daStart.group(1)!) ?? 0;
      deauthAllApCount = int.tryParse(daStart.group(3)!) ?? 0;
      if (daStart.group(4) != null) deauthAllMode = daStart.group(4)!;
      deauthAllPkts = 0;
      deauthAllCycles = 0;
      deauthAllFrames.clear();
      return;
    }
    if (_reDeauthAllStop.hasMatch(line)) {
      attacking = false;
      deauthAllStartedAt = null;
      deauthAllPkts = 0;
      deauthAllCycles = 0;
      return;
    }
    final daProg = _reDeauthAllProgress.firstMatch(line);
    if (daProg != null) {
      deauthAllCurrentChannel = int.tryParse(daProg.group(2)!) ?? 0;
      deauthAllCurrentIs5ghz = daProg.group(3) == '5G';
      deauthAllCycles = int.tryParse(daProg.group(4)!) ?? deauthAllCycles;
      deauthAllPkts = int.tryParse(daProg.group(5)!) ?? deauthAllPkts;
      deauthAllPps = int.tryParse(daProg.group(6)!) ?? 0;
      return;
    }
    final daFrame = _reDeauthAllFrame.firstMatch(line);
    if (daFrame != null) {
      deauthAllFrames.insert(
        0,
        DeauthFrame(
          channel: int.tryParse(daFrame.group(1)!) ?? 0,
          dst: daFrame.group(2)!.toUpperCase(),
          src: daFrame.group(3)!.toUpperCase(),
          reason: int.tryParse(daFrame.group(4)!, radix: 16) ?? 0,
          timestamp: DateTime.now(),
        ),
      );
      if (deauthAllFrames.length > _maxFramesKept) {
        deauthAllFrames.removeLast();
      }
      return;
    }
    final daScanAp = _reDeauthAllScanAp.firstMatch(line);
    if (daScanAp != null) {
      _tmpTargets.add(
        DeauthTarget(
          idx: int.tryParse(daScanAp.group(1)!) ?? 0,
          ssid: daScanAp.group(2)!.trim(),
          bssid: daScanAp.group(3)!.toUpperCase(),
          channel: int.tryParse(daScanAp.group(4)!) ?? 0,
          rssi: int.tryParse(daScanAp.group(5)!) ?? 0,
          is5ghz: daScanAp.group(6) == '5',
        ),
      );
      return;
    }
    if (_reDeauthAllScanDone.hasMatch(line)) {
      _commitTargets();
      return;
    }
    if (line.startsWith('deauthall: RESCAN')) {
      deauthAllScanning = true;
      _tmpTargets.clear();
      return;
    }

    // ── AP table ──────────────────────────────────────────
    if (line.startsWith('idx  ch  band')) {
      if (_ignoreApList) return;
      _commitAps();
      _inAps = true;
      return;
    }
    if (_inAps) {
      final m = _reAp.firstMatch(line);
      if (m != null) {
        final idx = int.tryParse(m.group(1)!);
        final ch = int.tryParse(m.group(2)!);
        final rssi = int.tryParse(m.group(4)!);
        if (idx != null && ch != null && rssi != null) {
          _tmpAps.add(
            ApEntry(
              idx: idx,
              channel: ch,
              is5ghz: m.group(3) == '5GHz',
              rssi: rssi,
              bssid: m.group(5)!.toUpperCase(),
              ssid: m.group(6)!,
            ),
          );
        }
        return;
      }
      _commitAps();
    }

    // ── WL / BL ───────────────────────────────────────────
    final wm = _reWl.firstMatch(line);
    if (wm != null) {
      if (!_inWl) {
        _tmpWl.clear();
        _inWl = true;
      }
      _tmpWl.add(AclEntry(mac: wm.group(1)!.toUpperCase(), kind: wm.group(2)!));
      return;
    }
    final bm = _reBl.firstMatch(line);
    if (bm != null) {
      if (!_inBl) {
        _tmpBl.clear();
        _inBl = true;
      }
      _tmpBl.add(AclEntry(mac: bm.group(1)!.toUpperCase(), kind: bm.group(2)!));
      return;
    }
    if (line == '(wl empty)') {
      _tmpWl.clear();
      _commitWl();
      return;
    }
    if (line == '(bl empty)') {
      _tmpBl.clear();
      _commitBl();
      return;
    }

    // ── STA ───────────────────────────────────────────────
    final sm = _reStaLine.firstMatch(line);
    if (sm != null) {
      final rssi = int.tryParse(sm.group(2)!);
      final ch = int.tryParse(sm.group(3)!);
      if (rssi != null && ch != null) {
        if (!_inStas) {
          _tmpStations.clear();
          _inStas = true;
        }
        _tmpStations.add(
          StaEntry(
            mac: sm.group(1)!.toUpperCase(),
            rssi: rssi,
            channel: ch,
            bssid: sm.group(4)!.toUpperCase(),
          ),
        );
      }
      return;
    }
    if (line == '(sta end)') {
      _commitStas();
      return;
    }
    if (line == '(no devices)') {
      stations = const [];
      _inStas = false;
      return;
    }

    // ── Dev scan ──────────────────────────────────────────
    if (line.startsWith('dev: start')) {
      deviceScanning = true;
      return;
    }
    if (line.startsWith('dev: scan failed') ||
        (line.startsWith('dev: ') && line.contains('devices found'))) {
      deviceScanning = false;
      if (line.startsWith('dev: scan failed')) {
        connectionIssue = line;
      }
      if (line.contains('devices found')) {
        unawaited(_sendCommand('dev ls').catchError((Object _) {}));
      }
      return;
    }

    if (_inWl) _commitWl();
    if (_inBl) _commitBl();
    if (_inStas) _commitStas();

    // ── Sel ───────────────────────────────────────────────
    final st = _reSelToggle.firstMatch(line);
    if (st != null) {
      if (_ignoreSelectionList) return;
      final idx = int.tryParse(st.group(1)!);
      if (idx != null) {
        if (st.group(2) == 'on') {
          selectedIdxs = {...selectedIdxs, idx};
        } else {
          selectedIdxs = selectedIdxs.where((i) => i != idx).toSet();
        }
      }
      return;
    }
    if (line == 'sel: (empty)') {
      if (!_ignoreSelectionList) selectedIdxs = {};
      return;
    }
    final sl = _reSelLine.firstMatch(line);
    if (sl != null) {
      if (_ignoreSelectionList) return;
      final nums = sl
          .group(1)!
          .trim()
          .split(RegExp(r'\s+'))
          .where((s) => s.isNotEmpty)
          .map((s) => int.tryParse(s))
          .whereType<int>()
          .toSet();
      selectedIdxs = nums;
      return;
    }

    // ── Scan lifecycle ────────────────────────────────────
    if (line.startsWith('scan: start')) {
      // A deauth-all rescan drives wifi_ctrl_scan() internally and emits the
      // same generic lines; its own SCAN_AP/SCAN_DONE handle targets, so don't
      // treat these as a user scan (which would clear selection / show spinner).
      if (attacking && attackMode == 'deauthall') return;
      // Firmware emits "scan: start (2.4 GHz)" then "scan: 5 GHz..." — match
      // the prefix so console-initiated scans also light up the UI.
      scanning = true;
      aps = const [];
      _tmpAps.clear();
      _inAps = false;
      _ignoreApList = true;
      _ignoreSelectionList = true;
      selectedIdxs = {};
      _lastScanTime = null;
      return;
    }
    if (line == 'scan: stopped') {
      scanning = false;
      return;
    }
    if (line == 'scan: cleared') {
      clearNetworkScanState();
      return;
    }
    if (line.startsWith('scan: stop failed') ||
        line.startsWith('scan: clear failed')) {
      connectionIssue = line;
      return;
    }
    if (line.startsWith('scan: ') &&
        (RegExp(r'scan: \d+ APs').hasMatch(line) ||
            line.startsWith('scan: no APs') ||
            line.startsWith('scan: failed'))) {
      // Ignore the generic completion line from a deauth-all internal rescan.
      if (attacking && attackMode == 'deauthall') return;
      scanning = false;
      selectedIdxs = {};
      if (line.startsWith('scan: failed')) {
        connectionIssue = line;
      }
      if (RegExp(r'scan: \d+ APs').hasMatch(line)) {
        _ignoreApList = false;
        _ignoreSelectionList = false;
        _lastScanTime = DateTime.now();
        for (final command in const ['ls', 'sel ls', 'dev ls']) {
          unawaited(_sendCommand(command).catchError((Object _) {}));
        }
      }
      return;
    }
    if (line == '(no APs — run `scan` first)') {
      if (!_ignoreApList) aps = const [];
      return;
    }
    if (line == 'scan: stop the attack first') {
      scanning = false;
      connectionIssue = line;
      return;
    }

    if (line.startsWith('t24:') || line.startsWith('t5:')) return;
    if (line == 'clear: selection reset') {
      selectedIdxs = {};
      return;
    }

    // ── Nuke / attack ─────────────────────────────────────
    if (line.startsWith('nuke: no APs') ||
        line == 'attack: no APs selected' ||
        line == 'attack: not initialized' ||
        ((line.startsWith('attack: ') || line.startsWith('nuke: ')) &&
            line.contains('task create failed'))) {
      connectionIssue = line;
      attacking = false;
      nukeStartedAt = null;
      attackStartedAt = null;
      deauthAllStartedAt = null;
      return;
    }
    if (line.startsWith('deauthall: not running')) {
      connectionIssue = line;
      deauthAllScanning = false;
      return;
    }
    final nm = _reNuke.firstMatch(line);
    if (nm != null) {
      nukeApCount = int.tryParse(nm.group(2)!) ?? nukeApCount;
      nukePkts = int.tryParse(nm.group(3)!) ?? nukePkts;
      return;
    }
    final am = _reAttack.firstMatch(line);
    if (am != null) {
      attackAps = int.tryParse(am.group(2)!) ?? attackAps;
      final pm = RegExp(r'\bpps=(\d+)').firstMatch(line);
      if (pm != null) attackPps = int.tryParse(pm.group(1)!) ?? attackPps;
      return;
    }

    if (line.startsWith('attack: start mode=')) {
      attacking = true;
      final mm = RegExp(r'mode=(\w+)').firstMatch(line);
      if (mm != null) attackMode = mm.group(1)!;
      if (attackMode == 'nuke') {
        nukeStartedAt = DateTime.now();
        final dm = RegExp(r'duration=(\d+)s').firstMatch(line);
        final dv = dm != null ? int.tryParse(dm.group(1)!) : null;
        if (dv != null) nukeDurationSecs = dv;
      } else if (attackMode == 'deauthall') {
        deauthAllStartedAt ??= DateTime.now();
      } else {
        attackStartedAt = DateTime.now();
      }
      return;
    }
    if (line.startsWith('attack: stopped') ||
        line == 'attack: stop requested' ||
        line == 'attack: not running') {
      attacking = false;
      nukeStartedAt = null;
      attackStartedAt = null;
      nukePkts = 0;
      deauthAllStartedAt = null;
      deauthAllPkts = 0;
      deauthAllCycles = 0;
      return;
    }
    if (line.startsWith('status: idle')) {
      // Wi-Fi status is independent from the separately reported BLE task.
      attacking = false;
      nukeStartedAt = null;
      attackStartedAt = null;
      nukePkts = 0;
      deauthAllStartedAt = null;
      deauthAllPkts = 0;
      deauthAllCycles = 0;
      return;
    }
    if (line.startsWith('status: running')) {
      attacking = true;
      final mm = RegExp(r'mode=(\w+)').firstMatch(line);
      if (mm != null) attackMode = mm.group(1)!;
      if (attackMode == 'nuke') {
        nukeStartedAt ??= DateTime.now();
      } else if (attackMode == 'deauthall') {
        deauthAllStartedAt ??= DateTime.now();
      } else if (attackStartedAt == null) {
        attackStartedAt = DateTime.now();
      }
      return;
    }

    final mm = _reMode.firstMatch(line);
    if (mm != null) {
      attackMode = mm.group(1)!;
      return;
    }
  }

  // ── Public API ──────────────────────────────────────────────

  Future<void> scanWifi() async {
    scanning = true;
    aps = const [];
    _tmpAps.clear();
    _inAps = false;
    _ignoreApList = true;
    _ignoreSelectionList = true;
    selectedIdxs = {};
    _lastScanTime = null;
    _notifyListeners();
    try {
      await _sendCommand('scan');
    } catch (_) {
      scanning = false;
      _notifyListeners();
      rethrow;
    }
  }

  void clearNetworkScanState() {
    scanning = false;
    aps = const [];
    _tmpAps.clear();
    _inAps = false;
    _ignoreApList = true;
    _ignoreSelectionList = true;
    selectedIdxs = {};
    _lastScanTime = null;
    _notifyListeners();
  }

  Future<void> resetNetworkScanForScreen() async {
    clearNetworkScanState();
    Object? stopError;
    Object? clearError;
    try {
      await _sendCommand('scan stop');
    } catch (error) {
      stopError = error;
    }
    try {
      await _sendCommand('scan clear');
    } catch (error) {
      clearError = error;
    }
    if (stopError != null || clearError != null) {
      throw StateError(
        'Network scan cleanup failed'
        '${stopError == null ? '' : ' (stop: $stopError)'}'
        '${clearError == null ? '' : ' (clear: $clearError)'}',
      );
    }
  }

  Future<void> scanDevices() async {
    deviceScanning = true;
    _notifyListeners();
    try {
      await _sendCommand('dev scan');
    } catch (_) {
      deviceScanning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> toggleAp(int idx) => _sendCommand('sel $idx');
  Future<void> clearSelection() => _sendCommand('clear');
  Future<void> startAttack() => _sendCommand('start');

  /// Stop regular / nuke / deauth-all attack (WiFi).
  /// TIDAK menyentuh BLE spam.
  Future<void> stopAttack() async {
    if (!attacking) return;
    attacking = false;
    _notifyListeners();
    try {
      await _sendCommand('stop');
    } catch (_) {
      attacking = true;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> nuke(int secs) async {
    secs = _clampSecs(secs);
    final prevStartedAt = nukeStartedAt;
    final prevDuration = nukeDurationSecs;
    nukeDurationSecs = secs;
    nukeStartedAt = DateTime.now();
    _notifyListeners();
    try {
      await _sendCommand('nuke $secs');
    } catch (_) {
      nukeStartedAt = prevStartedAt;
      nukeDurationSecs = prevDuration;
      _notifyListeners();
      rethrow;
    }
  }

  // ── Deauth All ─────────────────────────────────────────────
  /// [mode] is 'skip' (default — skip APs with a whitelisted STA) or
  /// 'unicast' (downgrade to per-STA unicast; needs a sniffed STA list).
  Future<void> deauthAll(int secs, {String mode = 'skip'}) async {
    secs = _clampSecs(secs);
    deauthAllDurationSecs = secs;
    deauthAllMode = mode;
    deauthAllStartedAt = DateTime.now();
    deauthAllPkts = 0;
    deauthAllCycles = 0;
    deauthAllFrames.clear();
    deauthAllScanning = false;
    _tmpTargets.clear();
    _notifyListeners();
    try {
      await _sendCommand('deauthall --$mode $secs');
    } catch (_) {
      deauthAllStartedAt = null;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> deauthAllRescan() async {
    if (deauthAllScanning) return;
    deauthAllScanning = true;
    _tmpTargets.clear();
    _notifyListeners();
    try {
      await _sendCommand('deauthall rescan');
    } catch (_) {
      deauthAllScanning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> deauthAllStop() => stopAttack();

  // ── BLE Spam ────────────────────────────────────────────────
  Future<void> bleSpamStart(
    BleSpamType type,
    int delayMs,
    int durationSecs,
  ) async {
    delayMs = delayMs.clamp(10, 1000);
    durationSecs = _clampSecs(durationSecs);
    bleSpamType = type;
    bleSpamDelayMs = delayMs;
    bleSpamDurationSecs = durationSecs;
    bleSpamStartedAt = DateTime.now();
    bleSpamFrames = 0;
    bleSpamFps = 0;
    bleSpamRunning = true;
    _notifyListeners();
    try {
      await _sendCommand('blespam ${type.id} $delayMs $durationSecs');
    } catch (_) {
      bleSpamRunning = false;
      bleSpamStartedAt = null;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> bleSpamStop() async {
    if (!bleSpamRunning) return;
    bleSpamRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('blespam stop');
    } catch (_) {
      bleSpamRunning = true;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> bleSpamSetDelay(int delayMs) async {
    delayMs = delayMs.clamp(10, 1000);
    final previousDelay = bleSpamDelayMs;
    bleSpamDelayMs = delayMs;
    _notifyListeners();
    try {
      await _sendCommand('blespam delay $delayMs');
    } catch (_) {
      bleSpamDelayMs = previousDelay;
      _notifyListeners();
      rethrow;
    }
  }

  // ── BLE Scan ────────────────────────────────────────────────
  Future<void> bleScanStart({bool active = false, int durationSec = 30}) async {
    durationSec = durationSec.clamp(5, 600);
    bleScanRunning = true;
    bleScanActive = active;
    bleScanElapsed = 0;
    bleScanFound = 0;
    _bleScanMap.clear();
    bleScanDevices = const [];
    _notifyListeners();
    final cmd = active
        ? 'blescan start active $durationSec'
        : 'blescan start $durationSec';
    try {
      await _sendCommand(cmd);
    } catch (_) {
      bleScanRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> bleScanStop() async {
    if (!bleScanRunning) return;
    bleScanRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('blescan stop');
    } catch (_) {
      bleScanRunning = true;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> bleScanClear() async {
    _bleScanMap.clear();
    bleScanDevices = const [];
    bleScanFound = 0;
    bleScanElapsed = 0;
    _notifyListeners();
    await _sendCommand('blescan clear');
  }

  void clearBleScanState() {
    bleScanRunning = false;
    bleScanActive = false;
    bleScanElapsed = 0;
    bleScanFound = 0;
    _bleScanMap.clear();
    bleScanDevices = const [];
    _notifyListeners();
  }

  Future<void> resetBleScanForScreen() async {
    final wasRunning = bleScanRunning;
    clearBleScanState();
    Object? stopError;
    Object? clearError;
    if (wasRunning) {
      try {
        await _sendCommand('blescan stop');
      } catch (error) {
        stopError = error;
      }
    }
    try {
      await _sendCommand('blescan clear');
    } catch (error) {
      clearError = error;
    }
    if (stopError != null || clearError != null) {
      throw StateError(
        'BLE scan cleanup failed'
        '${stopError == null ? '' : ' (stop: $stopError)'}'
        '${clearError == null ? '' : ' (clear: $clearError)'}',
      );
    }
  }

  // ── Monitor ─────────────────────────────────────────────────
  Future<void> monitorStart(int apIdx, {int? channel, bool? is5ghz}) async {
    monitorRunning = true;
    if (channel != null) monitorChannel = channel;
    if (is5ghz != null) monitorIs5ghz = is5ghz;
    monitorRssi = 0;
    monitorFps = 0;
    monitorFrames = 0;
    monitorMgmt = 0;
    monitorData = 0;
    monitorStas = 0;
    _notifyListeners();
    try {
      await _sendCommand('monitor $apIdx');
    } catch (_) {
      monitorRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> monitorStop() async {
    if (!monitorRunning) return;
    monitorRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('monitor stop');
    } catch (_) {
      // Best-effort stop; keep UI in stopped state regardless.
      _notifyListeners();
      rethrow;
    }
  }

  // ── WiFi Live Scan ──────────────────────────────────────────
  Future<void> wlscanStart({
    String mode = 'fast',
    int intervalMs = 4000,
    int durationSec = 0,
  }) async {
    wlscanRunning = true;
    wlscanMode = mode;
    wlscanCycles = 0;
    wlscanUnique = 0;
    _wlscanMap.clear();
    wlscanAps = const [];
    _notifyListeners();
    try {
      await _sendCommand('wlscan start $mode $intervalMs $durationSec');
    } catch (_) {
      wlscanRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> wlscanStop() async {
    // Always send the stop best-effort — never gate on wlscanRunning. The
    // firmware scan can still be alive while this flag is false (a BLE blip
    // clears it, a stale CYCLE can race it), and skipping the command then
    // leaves an orphaned scan holding the radio. Restore the real prior state
    // on failure instead of blindly assuming it was running.
    final was = wlscanRunning;
    wlscanRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('wlscan stop');
    } catch (_) {
      wlscanRunning = was;
      _notifyListeners();
      rethrow;
    }
  }

  // ── BLE Wardriving ──────────────────────────────────────────
  Future<void> bleWarStart({bool active = true, int durationSec = 0}) async {
    bleWarRunning = true;
    bleWarActive = active;
    bleWarCycles = 0;
    bleWarUnique = 0;
    bleFindMyObservations = 0;
    _bleWarMap.clear();
    _bleFindMySeenByAddr.clear();
    bleWarDevices = const [];
    _notifyListeners();
    final cmd = active
        ? 'blewar start active $durationSec'
        : 'blewar start $durationSec';
    try {
      await _sendCommand(cmd);
    } catch (_) {
      bleWarRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> bleWarStop() async {
    if (!bleWarRunning) return;
    bleWarRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('blewar stop');
    } catch (_) {
      bleWarRunning = true;
      _notifyListeners();
      rethrow;
    }
  }

  Future<String> bleFindMySave(String address) async {
    if (!RegExp(r'^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}$').hasMatch(address)) {
      throw ArgumentError.value(address, 'address', 'Invalid BLE address.');
    }
    if (_bleFindMySaveCompleter != null) {
      throw StateError('A Find My save is already in progress.');
    }

    final completer = Completer<String>();
    _bleFindMySaveCompleter = completer;
    final result = completer.future.timeout(
      const Duration(seconds: 8),
      onTimeout: () => throw TimeoutException(
        'Firmware did not confirm saving the Find My observation.',
      ),
    );
    try {
      await _sendCommand('blefind save $address');
      return await result;
    } finally {
      if (identical(_bleFindMySaveCompleter, completer)) {
        _bleFindMySaveCompleter = null;
      }
    }
  }

  // ── WiFi Spectrum ───────────────────────────────────────────
  Future<void> spectrumStart({bool include5ghz = true}) async {
    spectrumRunning = true;
    spectrumSweep = 0;
    _spectrumMap.clear();
    spectrumChannels = const [];
    _notifyListeners();
    try {
      await _sendCommand(include5ghz ? 'spectrum start 5' : 'spectrum start');
    } catch (_) {
      spectrumRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> spectrumStop() async {
    if (!spectrumRunning) return;
    spectrumRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('spectrum stop');
    } catch (_) {
      spectrumRunning = true;
      _notifyListeners();
      rethrow;
    }
  }

  // ── BLE Spectrum ────────────────────────────────────────────
  Future<void> bleSpectrumStart() async {
    bleSpectrumRunning = true;
    bleSpectrumBins = List<int>.filled(10, 0, growable: false);
    bleSpectrumMean = null;
    bleSpectrumPeak = -100;
    bleSpectrumTotal = 0;
    bleSpectrumFrameId = 0;
    _notifyListeners();
    try {
      await _sendCommand('blespectrum start');
    } catch (_) {
      bleSpectrumRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> bleSpectrumStop() async {
    if (!bleSpectrumRunning) return;
    bleSpectrumRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('blespectrum stop');
    } catch (_) {
      bleSpectrumRunning = true;
      _notifyListeners();
      rethrow;
    }
  }

  // ── WiFi Spam ───────────────────────────────────────────────
  Future<void> wspamStart({
    required String mode,
    String rate = 'aggressive',
    bool dual = true,
    int durationSec = 0,
  }) async {
    wspamRunning = true;
    wspamMode = mode;
    wspamRate = rate;
    wspamDual = dual;
    wspamTotal = 0;
    wspamFps = 0;
    wspamCurrentSsid = '';
    wspamFeed.clear();
    _notifyListeners();
    final band = dual ? 'dual' : '2g';
    try {
      await _sendCommand('wspam start $mode $rate $band $durationSec');
    } catch (_) {
      wspamRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> wspamStop() async {
    if (!wspamRunning) return;
    wspamRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('wspam stop');
    } catch (_) {
      wspamRunning = true;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> wspamListCustom() => _sendCommand('wspam ls');

  Future<void> wspamAddCustom(String ssid) async {
    await _sendCommand('wspam add $ssid');
    await _sendCommand('wspam ls');
  }

  Future<void> wspamClearCustom() async {
    await _sendCommand('wspam clear');
    wspamCustom = const [];
    _notifyListeners();
    await _sendCommand('wspam ls');
  }

  // ── Evil Portal ─────────────────────────────────────────────
  Future<void> portalListTemplates() => _sendCommand('portal ls');

  Future<void> portalStatus() => _sendCommand('portal status');

  Future<void> portalStart(String template, {String? ssid}) async {
    portalRunning = true;
    portalTemplate = template;
    portalSsid = ssid ?? '';
    portalClients = 0;
    portalHits = 0;
    portalUp = 0;
    portalCredsCount = 0;
    _notifyListeners();
    final arg = (ssid != null && ssid.trim().isNotEmpty)
        ? ' ${ssid.trim()}'
        : '';
    try {
      await _sendCommand('portal start $template$arg');
    } catch (_) {
      portalRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> portalStop() async {
    if (!portalRunning) return;
    portalRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('portal stop');
    } catch (_) {
      portalRunning = true;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> portalRefreshCreds() => _sendCommand('portal creds');

  Future<void> portalClearCreds() async {
    portalCreds.clear();
    _notifyListeners();
    await _sendCommand('portal clear');
  }

  /// Upload operator-supplied HTML as a custom template, chunked as hex so
  /// each line stays within the BLE/line limit. Refreshes the template list.
  Future<void> portalUploadHtml(String name, String html) async {
    final safe = name.trim().isEmpty ? 'custom' : name.trim();
    final bytes = utf8.encode(html);
    await _sendCommand('portal html begin $safe');
    // 100 bytes -> 200 hex chars; + "portal html data " (17) = 217-char line,
    // safely under the firmware CLI_LINE_MAX (256, resets on overflow) and
    // within one BLE write (MTU 247 -> 244-byte payload).
    const chunk = 100;
    for (var i = 0; i < bytes.length; i += chunk) {
      final end = (i + chunk < bytes.length) ? i + chunk : bytes.length;
      final hex = StringBuffer();
      for (var j = i; j < end; j++) {
        hex.write(bytes[j].toRadixString(16).padLeft(2, '0'));
      }
      await _sendCommand('portal html data $hex');
    }
    await _sendCommand('portal html end');
    await _sendCommand('portal ls'); // pick up the new custom template
  }

  Future<void> portalRemoveHtml(String name) async {
    await _sendCommand('portal html rm $name');
    await _sendCommand('portal ls');
  }

  // ── nRF24 Scanner ───────────────────────────────────────────
  Future<void> nrfStatus() => _sendCommand('nrf status');

  /// One-shot peak scan (synchronous on firmware; returns via `nrf: peak …`).
  Future<void> nrfScanPeak() => _sendCommand('nrf scan');

  Future<void> nrfWaterfallStart() async {
    nrfScanRunning = true;
    nrfScanChannels = const [];
    nrfScanSweep = 0;
    _nrfScanDumpSweep = 0;
    nrfScanPeakRpdDetections = 0;
    _nrfScanTmp.clear();
    _notifyListeners();
    try {
      await _sendCommand('nrf waterfall');
    } catch (_) {
      nrfScanRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> nrfWaterfallStop() async {
    if (!nrfScanRunning) return;
    nrfScanRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('nrf waterfall stop');
    } catch (_) {
      nrfScanRunning = true;
      _notifyListeners();
      rethrow;
    }
  }

  /// Refresh the per-channel table (firmware dumps its stored results).
  Future<void> nrfScanRefresh() => _sendCommand('nrf ls');

  // ── nRF24 Jammer ────────────────────────────────────────────
  Future<void> nrfJamAuto(String target, int durationSec) async {
    durationSec = durationSec.clamp(0, 3600);
    nrfJamRunning = true;
    nrfJamTarget = target;
    nrfJamWaterfall = false;
    nrfJamFrames = 0;
    nrfJamElapsed = 0;
    nrfJamDurationSecs = durationSec;
    _notifyListeners();
    try {
      await _sendCommand('nrf jam auto $target $durationSec');
    } catch (_) {
      nrfJamRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> nrfJamManual({
    required String target,
    required String strategy,
    required String rate,
    required String pa,
    required int dwellUs,
    required int burst,
    int durationSec = 0,
  }) async {
    durationSec = durationSec.clamp(0, 3600);
    dwellUs = dwellUs.clamp(50, 50000);
    burst = burst.clamp(1, 20);
    nrfJamRunning = true;
    nrfJamTarget = target;
    nrfJamStrategy = strategy;
    nrfJamWaterfall = false;
    nrfJamFrames = 0;
    nrfJamElapsed = 0;
    nrfJamDurationSecs = durationSec;
    _notifyListeners();
    try {
      await _sendCommand(
        'nrf jam manual $target $strategy $rate $pa $dwellUs $burst $durationSec',
      );
    } catch (_) {
      nrfJamRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> nrfJamWaterfallStart(int durationSec) async {
    durationSec = durationSec.clamp(0, 3600);
    nrfJamRunning = true;
    nrfJamWaterfall = true;
    nrfJamFrames = 0;
    nrfJamElapsed = 0;
    nrfJamDurationSecs = durationSec;
    _notifyListeners();
    try {
      await _sendCommand('nrf wfjam $durationSec');
    } catch (_) {
      nrfJamRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> nrfJamStop() async {
    if (!nrfJamRunning) return;
    nrfJamRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('nrf jam stop');
    } catch (_) {
      nrfJamRunning = true;
      _notifyListeners();
      rethrow;
    }
  }

  // ── nRF24 Sniffer ───────────────────────────────────────────
  Future<void> nrfSniffStart(int channel, int durationSec) async {
    channel = channel.clamp(0, 125);
    durationSec = durationSec.clamp(0, 600);
    nrfSniffRunning = true;
    nrfSniffChannel = channel;
    nrfSniffCaptured = 0;
    nrfSniffDropped = 0;
    nrfSniffElapsed = 0;
    nrfSniffPackets.clear();
    _notifyListeners();
    try {
      await _sendCommand('nrf sniff start $channel $durationSec');
    } catch (_) {
      nrfSniffRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> nrfSniffStop() async {
    if (!nrfSniffRunning) return;
    nrfSniffRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('nrf sniff stop');
    } catch (_) {
      nrfSniffRunning = true;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> nrfSniffClear() async {
    nrfSniffPackets.clear();
    nrfSniffCaptured = 0;
    nrfSniffDropped = 0;
    _notifyListeners();
    await _sendCommand('nrf sniff clear');
  }

  Future<void> nrfSniffRefresh() => _sendCommand('nrf sniff ls');

  Future<void> nrfReplay(int idx, {int count = 1, int delayUs = 1000}) async {
    count = count.clamp(1, 1000);
    delayUs = delayUs.clamp(0, 1000000);
    nrfReplayActive = true;
    _notifyListeners();
    try {
      await _sendCommand('nrf replay $idx $count $delayUs');
    } catch (_) {
      nrfReplayActive = false;
      _notifyListeners();
      rethrow;
    }
  }

  // ── nRF24 MouseJack ─────────────────────────────────────────
  Future<void> nrfMjScanStart(int durationSec) async {
    durationSec = durationSec.clamp(5, 600);
    nrfMjRunning = true;
    nrfMjElapsed = 0;
    nrfMjFound = 0;
    _nrfMjMap.clear();
    nrfMjTargets = const [];
    _notifyListeners();
    try {
      await _sendCommand('nrf mj scan $durationSec');
    } catch (_) {
      nrfMjRunning = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> nrfMjScanStop() async {
    if (!nrfMjRunning) return;
    nrfMjRunning = false;
    _notifyListeners();
    try {
      await _sendCommand('nrf mj stop');
    } catch (_) {
      nrfMjRunning = true;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> nrfMjClear() async {
    _nrfMjMap.clear();
    nrfMjTargets = const [];
    nrfMjFound = 0;
    _notifyListeners();
    await _sendCommand('nrf mj clear');
  }

  Future<void> nrfMjRefresh() => _sendCommand('nrf mj ls');

  Future<void> nrfMjInject(int idx, String text) async {
    nrfMjInjecting = true;
    _notifyListeners();
    try {
      await _sendCommand('nrf mj inject $idx $text');
    } catch (_) {
      nrfMjInjecting = false;
      _notifyListeners();
      rethrow;
    }
  }

  /// Stop any running nRF feature (best-effort, used when leaving the hub).
  Future<void> nrfStopAll() => _sendCommand('nrf stopall');

  // ── Infrared ────────────────────────────────────────────────
  Future<void> irStatus() => _sendCommand('ir status');

  Future<void> irUniversalSend(String category, String button) async {
    if (irUnivRunning || irEmulateRunning) {
      throw StateError('An IR transmission is already running.');
    }
    irUnivRunning = true;
    irUnivStopping = false;
    irUnivStopped = false;
    irUnivCategory = category;
    irUnivButton = button;
    irUnivSent = 0;
    irUnivFailed = 0;
    irUnivBrand = '';
    irUnivError = null;
    connectionIssue = null;
    _notifyListeners();
    try {
      await _sendCommand('ir univ $category $button');
    } catch (_) {
      irUnivRunning = false;
      irUnivStopping = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> irUniversalStop() async {
    if (!irUnivRunning) return;
    if (irUnivStopping) return;
    irUnivStopping = true;
    _notifyListeners();
    try {
      await _sendCommand('ir univ stop');
    } catch (_) {
      irUnivStopping = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> irRxStart() async {
    final previousSpectrumState = irSpectrumOn;
    irSpectrumOn = true;
    irRxRunning = true;
    _notifyListeners();
    try {
      if (!previousSpectrumState) await _sendCommand('ir spectrum on');
      await _sendCommand('ir rx start');
    } catch (_) {
      irRxRunning = false;
      irSpectrumOn = previousSpectrumState;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> irRxStop() async {
    if (!irRxRunning && !irSpectrumOn) return;
    final wasRunning = irRxRunning;
    irRxRunning = false;
    irSpectrumOn = false;
    irSpectrumBins = const [];
    irSpectrumTotalUs = 0;
    _notifyListeners();
    try {
      if (wasRunning) await _sendCommand('ir rx stop');
      await _sendCommand('ir spectrum off');
    } catch (_) {
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> irSpectrumSet(bool on) async {
    if (on) {
      if (!irRxRunning) {
        await irRxStart();
      } else if (!irSpectrumOn) {
        irSpectrumOn = true;
        _notifyListeners();
        try {
          await _sendCommand('ir spectrum on');
        } catch (_) {
          irSpectrumOn = false;
          _notifyListeners();
          rethrow;
        }
      }
      return;
    }

    irSpectrumOn = on;
    irSpectrumBins = const [];
    irSpectrumTotalUs = 0;
    _notifyListeners();
    try {
      await _sendCommand(on ? 'ir spectrum on' : 'ir spectrum off');
    } catch (_) {
      irSpectrumOn = !on;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> irSaveCapture(String name, int captureId) async {
    await _sendCommand('ir save $name $captureId');
    await _sendCommand('ir ls');
  }

  Future<void> irSaveLast(String name) => irSaveCapture(name, 0);

  Future<void> irReceiverReset() async {
    irRxRunning = false;
    irSpectrumOn = false;
    irDetected.clear();
    irSpectrumBins = const [];
    irSpectrumTotalUs = 0;
    _notifyListeners();
    await _sendCommand('ir rx stop');
    await _sendCommand('ir spectrum off');
    await _sendCommand('ir rx clear');
  }

  Future<void> irReceiverClose() async {
    try {
      await irReceiverReset();
    } catch (error) {
      connectionIssue = 'Failed to reset IR receiver on exit: $error';
      _notifyListeners();
    }
  }

  Future<void> irSavedRefresh() => _sendCommand('ir ls');

  Future<void> irSavedDelete(String name) async {
    irSaved = irSaved.where((e) => e.name != name).toList(growable: false);
    _notifyListeners();
    await _sendCommand('ir rm $name');
    await _sendCommand('ir ls');
  }

  // ── LittleFS file browser ───────────────────────────────────
  Future<void> fsRefresh() => _sendCommand('fs ls');

  Future<void> fsDelete(String path) async {
    // Optimistic removal so the list updates immediately; the trailing
    // `fs ls` reconciles with the device's actual state.
    fsFiles = fsFiles.where((f) => f.path != path).toList(growable: false);
    _notifyListeners();
    await _sendCommand('fs rm $path');
    await _sendCommand('fs ls');
  }

  Future<void> irEmulateStart(String name, {int count = 0}) async {
    if (irUnivRunning || irEmulateRunning) {
      throw StateError('An IR transmission is already running.');
    }
    irEmulateRunning = true;
    irEmulateStopping = false;
    irEmulateName = name;
    irEmulateSent = 0;
    _notifyListeners();
    try {
      await _sendCommand('ir emulate $name $count');
    } catch (_) {
      irEmulateRunning = false;
      irEmulateStopping = false;
      _notifyListeners();
      rethrow;
    }
  }

  Future<void> irEmulateStop() async {
    if (!irEmulateRunning) return;
    if (irEmulateStopping) return;
    irEmulateStopping = true;
    _notifyListeners();
    try {
      await _sendCommand('ir emulate stop');
    } catch (_) {
      irEmulateStopping = false;
      _notifyListeners();
      rethrow;
    }
  }

  void irClearDetected() {
    irDetected.clear();
    _notifyListeners();
  }

  // ── LED control ─────────────────────────────────────────────
  Future<void> ledAuto() => _sendCommand('led auto');

  Future<void> ledEffect(String effect, int r, int g, int b) =>
      _sendCommand('led $effect $r $g $b');

  Future<void> ledBrightness(int value) =>
      _sendCommand('led brightness ${value.clamp(0, 255)}');

  // ── ACL ────────────────────────────────────────────────────
  Future<void> addToWhitelist(String mac, [String kind = 'auto']) async {
    await _sendCommand('wl add $mac $kind');
    _tmpWl.clear();
    await _sendCommand('wl ls');
  }

  Future<void> removeFromWhitelist(String mac) async {
    await _sendCommand('wl rm $mac');
    _tmpWl.clear();
    await _sendCommand('wl ls');
  }

  Future<void> addToBlacklist(String mac, [String kind = 'auto']) async {
    await _sendCommand('bl add $mac $kind');
    _tmpBl.clear();
    await _sendCommand('bl ls');
  }

  Future<void> removeFromBlacklist(String mac) async {
    await _sendCommand('bl rm $mac');
    _tmpBl.clear();
    await _sendCommand('bl ls');
  }

  void clearLog() {
    rawLog = '';
    _notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    _notifyTimer?.cancel();
    _notifyTimer = null;
    _sub?.cancel();
    super.dispose();
  }
}
