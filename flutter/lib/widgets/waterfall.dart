import 'dart:math' as math;
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

/// Shared, high-precision spectrum-waterfall toolkit used by every RF spectrum
/// screen (WiFi / BLE / nRF24) so they read as one instrument.
///
/// The waterfall is drawn as a real bitmap (one pixel per frequency-bin × time
/// row) that is scaled to the widget with high-quality filtering. Intensity is mapped
/// through a perceptual "turbo"-style colormap that starts near-black (an empty
/// band stays clean) and climbs blue → cyan → green → yellow → orange → red →
/// white-hot.
///
/// Orientation follows the SDR convention: the **newest row is at the top** and
/// history flows **downward** like a real waterfall. A ~60 fps ticker slides the
/// whole field down by a fraction of a row between data commits, so even slow
/// sources (a WiFi sweep can be ~1 s apart) read as a smooth continuous flow
/// rather than discrete stacked blocks.

// ───────────────────────── colormap ─────────────────────────

class _Lut {
  _Lut() {
    // (position 0..1, R, G, B)
    const stops = <List<double>>[
      [0.00, 0, 4, 18],
      [0.10, 10, 24, 74],
      [0.22, 18, 58, 158],
      [0.36, 13, 111, 208],
      [0.50, 18, 179, 179],
      [0.62, 47, 208, 106],
      [0.74, 150, 224, 42],
      [0.84, 255, 210, 30],
      [0.92, 255, 122, 24],
      [0.97, 255, 42, 42],
      [1.00, 255, 233, 214],
    ];
    for (var i = 0; i < 256; i++) {
      final t = i / 255.0;
      var s = 0;
      while (s < stops.length - 2 && t > stops[s + 1][0]) {
        s++;
      }
      final a = stops[s];
      final c = stops[s + 1];
      final span = c[0] - a[0];
      final f = span <= 0 ? 0.0 : ((t - a[0]) / span).clamp(0.0, 1.0);
      r[i] = (a[1] + (c[1] - a[1]) * f).round();
      g[i] = (a[2] + (c[2] - a[2]) * f).round();
      b[i] = (a[3] + (c[3] - a[3]) * f).round();
    }
  }

  final Uint8List r = Uint8List(256);
  final Uint8List g = Uint8List(256);
  final Uint8List b = Uint8List(256);
}

final _lut = _Lut();

/// Map a normalized intensity (0..1) to the shared waterfall colormap.
Color waterfallColor(double v) {
  final x = v.isNaN ? 0.0 : v.clamp(0.0, 1.0);
  final i = (x * 255.0).round().clamp(0, 255);
  return Color.fromARGB(255, _lut.r[i], _lut.g[i], _lut.b[i]);
}

int _u8(double channel01) => (channel01 * 255.0).round().clamp(0, 255);

// ───────────────────────── waterfall ─────────────────────────

/// A scrolling time × frequency heatmap. [rows] is ordered oldest → newest;
/// each row has [bins] values in 0..1. The newest row sits flush at the **top**
/// and history flows **down**. Bump [rev] whenever [rows] changes so the bitmap
/// is regenerated (and a new flow step begins).
class WaterfallView extends StatefulWidget {
  const WaterfallView({
    super.key,
    required this.rows,
    required this.bins,
    required this.rev,
    this.maxRows = 140,
    this.markers = const [],
    this.validRows,
    this.markerColor = const Color(0x33FFFFFF),
    this.background = const Color(0xFF00030A),
    this.smooth = true,
    this.gamma = 1.0,
  });

  /// Oldest → newest. Each entry length should equal [bins].
  final List<List<double>> rows;
  final int bins;

  /// Changes signal that [rows] was mutated and the bitmap must rebuild.
  final int rev;

  /// Vertical history depth in rows (fixes the time scale).
  final int maxRows;

  /// Bin indices to draw faint vertical guide lines at.
  final List<int> markers;

  /// Optional per-row validity masks, ordered oldest → newest like [rows].
  final List<List<bool>>? validRows;
  final Color markerColor;
  final Color background;

  /// Bilinearly smooth the scaled bitmap. Leave off for a few wide bins.
  final bool smooth;

  /// Intensity-shaping exponent applied before the colormap. 1.0 = linear.
  /// Values below 1 lift weak-but-real bursts out of the dark-blue noise floor
  /// so a thin BLE/nRF packet snaps to a hot colour instead of staying dim.
  final double gamma;

  @override
  State<WaterfallView> createState() => _WaterfallViewState();
}

class _WaterfallViewState extends State<WaterfallView>
    with SingleTickerProviderStateMixin {
  ui.Image? _img;
  int _builtRev = -1;
  int _reqId = 0;

  late final Ticker _ticker;
  final ValueNotifier<int> _repaint = ValueNotifier<int>(0);

  // Timing for the smooth inter-row slide (wall clock, ms).
  double _lastRowAtMs = 0;
  double _rowIntervalMs = 300; // EMA of the gap between committed rows

  double _nowMs() => DateTime.now().microsecondsSinceEpoch / 1000.0;

  @override
  void initState() {
    super.initState();
    _lastRowAtMs = _nowMs();
    _ticker = createTicker((_) => _repaint.value++)..start();
    _rebuild();
  }

  @override
  void didUpdateWidget(WaterfallView old) {
    super.didUpdateWidget(old);
    if (widget.rev != _builtRev ||
        widget.bins != old.bins ||
        widget.maxRows != old.maxRows) {
      // A new row (or a reset) arrived — learn the cadence and restart the slide.
      final now = _nowMs();
      final dt = now - _lastRowAtMs;
      if (dt > 15 && dt < 4000) {
        _rowIntervalMs = _rowIntervalMs * 0.7 + dt * 0.3;
      }
      _lastRowAtMs = now;
      _rebuild();
    }
  }

  @override
  void dispose() {
    _ticker.dispose();
    _repaint.dispose();
    _img?.dispose();
    super.dispose();
  }

  void _rebuild() {
    final bins = widget.bins;
    final maxRows = widget.maxRows;
    _builtRev = widget.rev;
    if (bins <= 0 || maxRows <= 0) return;

    final rows = widget.rows;
    final n = rows.length;
    final buf = Uint8List(bins * maxRows * 4);

    final bg = widget.background;
    final bgR = _u8(bg.r), bgG = _u8(bg.g), bgB = _u8(bg.b);
    final r = _lut.r, g = _lut.g, b = _lut.b;

    // Precompute the gamma contrast curve once per rebuild (cheap 256-entry LUT)
    // so the hot-burst mapping costs nothing per pixel.
    final useGamma = widget.gamma != 1.0 && widget.gamma > 0;
    Uint8List? shape;
    if (useGamma) {
      shape = Uint8List(256);
      for (var i = 0; i < 256; i++) {
        shape[i] = (math.pow(i / 255.0, widget.gamma) * 255.0).round().clamp(
          0,
          255,
        );
      }
    }

    for (var y = 0; y < maxRows; y++) {
      final src = (n - 1) - y; // y == 0 is the newest row (top); older below
      final rowBase = y * bins * 4;
      if (src >= 0 && src < n) {
        final row = rows[src];
        final rl = row.length;
        final validity = widget.validRows;
        final mask = validity == null
            ? null
            : (src < validity.length ? validity[src] : const <bool>[]);
        for (var x = 0; x < bins; x++) {
          final valid = mask == null || (x < mask.length && mask[x]);
          final v = valid && x < rl ? row[x] : 0.0;
          var idx =
              (v.isNaN ? 0.0 : (v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v)) * 255.0)
                  .round();
          if (shape != null) idx = shape[idx];
          final o = rowBase + x * 4;
          buf[o] = valid && x < rl ? r[idx] : bgR;
          buf[o + 1] = valid && x < rl ? g[idx] : bgG;
          buf[o + 2] = valid && x < rl ? b[idx] : bgB;
          buf[o + 3] = 255;
        }
      } else {
        for (var x = 0; x < bins; x++) {
          final o = rowBase + x * 4;
          buf[o] = bgR;
          buf[o + 1] = bgG;
          buf[o + 2] = bgB;
          buf[o + 3] = 255;
        }
      }
    }

    final req = ++_reqId;
    ui.decodeImageFromPixels(buf, bins, maxRows, ui.PixelFormat.rgba8888, (
      img,
    ) {
      if (!mounted || req != _reqId) {
        img.dispose();
        return;
      }
      _img?.dispose();
      setState(() => _img = img);
    });
  }

  @override
  Widget build(BuildContext context) {
    // The painter repaints at ~60 fps via [_repaint]; the RepaintBoundary
    // confines that to this layer so the surrounding chrome/cards don't
    // re-raster each frame (and a page transition composites it as a cached
    // texture instead of repainting it into a saveLayer).
    return RepaintBoundary(
      child: CustomPaint(
        painter: _WaterfallPainter(
          repaint: _repaint,
          img: _img,
          markers: widget.markers,
          bins: widget.bins,
          maxRows: widget.maxRows,
          markerColor: widget.markerColor,
          bg: widget.background,
          smooth: widget.smooth,
          lastRowAtMs: _lastRowAtMs,
          rowIntervalMs: _rowIntervalMs,
          nowMs: _nowMs,
        ),
        child: const SizedBox.expand(),
      ),
    );
  }
}

class _WaterfallPainter extends CustomPainter {
  _WaterfallPainter({
    required Listenable repaint,
    required this.img,
    required this.markers,
    required this.bins,
    required this.maxRows,
    required this.markerColor,
    required this.bg,
    required this.smooth,
    required this.lastRowAtMs,
    required this.rowIntervalMs,
    required this.nowMs,
  }) : super(repaint: repaint);

  final ui.Image? img;
  final List<int> markers;
  final int bins;
  final int maxRows;
  final Color markerColor;
  final Color bg;
  final bool smooth;
  final double lastRowAtMs;
  final double rowIntervalMs;
  final double Function() nowMs;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.clipRect(Offset.zero & size);
    canvas.drawRect(Offset.zero & size, Paint()..color = bg);

    final im = img;
    if (im != null) {
      // Fraction (0..1) of the way through the current inter-row interval; the
      // whole field slides down by one row-height over that interval so the
      // newest line eases in from the top instead of snapping.
      final interval = rowIntervalMs < 40 ? 40.0 : rowIntervalMs;
      final frac = ((nowMs() - lastRowAtMs) / interval).clamp(0.0, 1.0);
      final rowH = size.height / maxRows;
      final dy = (frac - 1.0) * rowH;

      canvas.drawImageRect(
        im,
        Rect.fromLTWH(0, 0, im.width.toDouble(), im.height.toDouble()),
        Rect.fromLTWH(0, dy, size.width, size.height),
        Paint()
          ..filterQuality = smooth ? FilterQuality.high : FilterQuality.none
          ..isAntiAlias = false,
      );
    }

    if (bins > 0) {
      final grid = Paint()
        ..color = const Color(0x20FFFFFF)
        ..strokeWidth = 1;
      for (var i = 1; i < 4; i++) {
        final y = size.height * i / 4;
        canvas.drawLine(Offset(0, y), Offset(size.width, y), grid);
      }
    }

    if (markers.isNotEmpty && bins > 0) {
      final mp = Paint()
        ..color = markerColor
        ..strokeWidth = 1;
      for (final c in markers) {
        if (c < 0 || c >= bins) continue;
        final x = (c + 0.5) / bins * size.width;
        canvas.drawLine(Offset(x, 0), Offset(x, size.height), mp);
      }
    }
  }

  @override
  bool shouldRepaint(_WaterfallPainter old) => true;
}

// ─────────────────────── live spectrum strip ───────────────────────

/// Instantaneous spectrum trace. [values] are 0..1 per bin; bump [rev] to
/// repaint.
///
/// With `stepped == false`, valid sample centers are joined by a smooth,
/// bounded curve; missing bins break the line. This is interpolation for
/// rendering, not additional measured samples.
///
/// With `maxHold == true` the widget keeps a per-bin peak and draws it as a
/// bright held line over the live trace. The peak eases down with a wall-clock
/// half-life ([holdHalfLife]) — independent of how often the parent rebuilds,
/// so the hold behaves identically whether the source emits 2 Hz (WiFi sweep)
/// or redraws many times a second. Ideal for catching intermittent, hopping or
/// bursty transmitters: a brief burst leaves a mark that lingers, then fades.
class SpectrumStrip extends StatefulWidget {
  const SpectrumStrip({
    super.key,
    required this.values,
    required this.rev,
    required this.accent,
    this.markers = const [],
    this.valid,
    this.averageValues,
    this.averageValid,
    this.averageColor = const Color(0xFFFFD166),
    this.markerColor = const Color(0x33FFFFFF),
    this.showGrid = false,
    this.linearTrace = false,
    this.stepped = false,
    this.bridgeGaps = false,
    this.maxHold = false,
    this.holdColor,
    this.holdHalfLife = const Duration(milliseconds: 1500),
  });

  final List<double> values;
  final int rev;
  final Color accent;
  final List<int> markers;

  /// Optional mask for bins that contain a real measurement.
  final List<bool>? valid;
  final List<double>? averageValues;
  final List<bool>? averageValid;
  final Color averageColor;
  final Color markerColor;
  final bool showGrid;
  final bool linearTrace;

  /// Draw per-bin sample-and-hold levels instead of joining sample centers.
  final bool stepped;

  /// Interpolate missing display bins between valid samples.
  ///
  /// Missing values remain invalid for statistics and max-hold accumulation;
  /// this only affects how the trace path is drawn.
  final bool bridgeGaps;

  /// Overlay a decaying per-bin max-hold peak line.
  final bool maxHold;

  /// Colour of the max-hold line; defaults to a bright tint of [accent].
  final Color? holdColor;

  /// Wall-clock time for a held peak to fall to half. Only used with [maxHold].
  final Duration holdHalfLife;

  @override
  State<SpectrumStrip> createState() => _SpectrumStripState();
}

class _SpectrumStripState extends State<SpectrumStrip>
    with SingleTickerProviderStateMixin {
  List<double> _hold = const [];
  Ticker? _ticker;
  double _lastDecayMs = 0;
  final ValueNotifier<int> _repaint = ValueNotifier<int>(0);

  double _nowMs() => DateTime.now().microsecondsSinceEpoch / 1000.0;

  @override
  void initState() {
    super.initState();
    _foldValues();
    if (widget.maxHold) _startTicker();
  }

  @override
  void didUpdateWidget(SpectrumStrip old) {
    super.didUpdateWidget(old);
    if (widget.maxHold && _ticker == null) {
      _startTicker();
    } else if (!widget.maxHold && _ticker != null) {
      _ticker!.dispose();
      _ticker = null;
      _hold = const [];
    }
    // A new frame → fold the fresh values into the held peaks (max only).
    if (widget.rev != old.rev || widget.values.length != old.values.length) {
      _foldValues();
    }
  }

  @override
  void dispose() {
    _ticker?.dispose();
    _repaint.dispose();
    super.dispose();
  }

  void _startTicker() {
    _lastDecayMs = _nowMs();
    _ticker = createTicker(_onTick)..start();
  }

  /// Continuous wall-clock decay of the held peaks, decoupled from data cadence.
  void _onTick(Duration _) {
    if (_hold.isEmpty) return;
    final now = _nowMs();
    final dt = now - _lastDecayMs;
    if (dt <= 0) return;
    _lastDecayMs = now;
    final halfMs = widget.holdHalfLife.inMilliseconds.toDouble();
    final k = halfMs <= 0 ? 0.0 : math.pow(0.5, dt / halfMs).toDouble();
    var changed = false;
    for (var i = 0; i < _hold.length; i++) {
      final nv = _hold[i] * k;
      if (nv != _hold[i]) {
        _hold[i] = nv;
        changed = true;
      }
    }
    if (changed) _repaint.value++;
  }

  /// Raise each held bin to the latest value (never lowers — decay does that).
  void _foldValues() {
    if (!widget.maxHold) {
      if (_hold.isNotEmpty) _hold = const [];
      return;
    }
    final n = widget.values.length;
    if (_hold.length != n) {
      final next = List<double>.filled(n, 0);
      for (var i = 0; i < n && i < _hold.length; i++) {
        next[i] = _hold[i];
      }
      _hold = next;
    }
    for (var i = 0; i < n; i++) {
      if (widget.valid != null &&
          (i >= widget.valid!.length || !widget.valid![i])) {
        continue;
      }
      final v = widget.values[i].isNaN ? 0.0 : widget.values[i].clamp(0.0, 1.0);
      if (v > _hold[i]) _hold[i] = v;
    }
    _repaint.value++;
  }

  @override
  Widget build(BuildContext context) {
    // Isolate the strip's repaints (continuous while max-hold decays) from the
    // surrounding layer — same reason as the waterfall above.
    return RepaintBoundary(
      child: CustomPaint(
        painter: _StripPainter(
          _repaint,
          widget.values,
          widget.rev,
          widget.accent,
          widget.markers,
          widget.valid,
          widget.averageValues,
          widget.averageValid,
          widget.averageColor,
          widget.markerColor,
          widget.showGrid,
          widget.linearTrace,
          widget.stepped,
          widget.bridgeGaps,
          widget.maxHold ? _hold : const [],
          widget.holdColor ?? Color.lerp(widget.accent, Colors.white, 0.65)!,
        ),
        child: const SizedBox.expand(),
      ),
    );
  }
}

class _StripPainter extends CustomPainter {
  _StripPainter(
    Listenable repaint,
    this.values,
    this.rev,
    this.accent,
    this.markers,
    this.valid,
    this.averageValues,
    this.averageValid,
    this.averageColor,
    this.markerColor,
    this.showGrid,
    this.linearTrace,
    this.stepped,
    this.bridgeGaps,
    this.hold,
    this.holdColor,
  ) : super(repaint: repaint);

  final List<double> values;
  final int rev;
  final Color accent;
  final List<int> markers;
  final List<bool>? valid;
  final List<double>? averageValues;
  final List<bool>? averageValid;
  final Color averageColor;
  final Color markerColor;
  final bool showGrid;
  final bool linearTrace;
  final bool stepped;
  final bool bridgeGaps;
  final List<double> hold;
  final Color holdColor;

  /// Interpolates between measured samples without overshooting their range.
  Path _trace(
    List<double> vals,
    double w,
    double h, {
    List<bool>? validity,
    bool linear = false,
  }) {
    final n = vals.length;
    final displayValues = bridgeGaps && validity != null
        ? _interpolateGaps(vals, validity)
        : vals;
    final displayValidity = displayValues == null ? validity : null;
    final traceValues = displayValues ?? vals;
    double displayVAt(int i) =>
        traceValues[i].isNaN ? 0.0 : traceValues[i].clamp(0.0, 1.0);
    double yAt(int i) => h - displayVAt(i) * (h - 2);
    bool validAt(int i) =>
        displayValidity == null ||
        (i < displayValidity.length && displayValidity[i]);

    final line = Path();
    if (stepped) {
      // Exact per-bin level across the bin's full width — no interpolation.
      for (var i = 0; i < n; i++) {
        if (!validAt(i)) continue;
        final x0 = i / n * w;
        final x1 = (i + 1) / n * w;
        final y = yAt(i);
        if (i == 0 || !validAt(i - 1)) {
          line.moveTo(x0, y);
        } else {
          line.lineTo(x0, y);
        }
        line.lineTo(x1, y);
      }
    } else if (linear) {
      var drawing = false;
      for (var i = 0; i < n; i++) {
        if (!validAt(i)) {
          drawing = false;
          continue;
        }
        final x = n == 1 ? w / 2 : (i + 0.5) / n * w;
        final y = yAt(i);
        if (drawing) {
          line.lineTo(x, y);
        } else {
          line.moveTo(x, y);
          drawing = true;
        }
      }
    } else {
      double xAt(int i) => n == 1 ? w / 2 : (i + 0.5) / n * w;
      var start = 0;
      while (start < n) {
        while (start < n && !validAt(start)) {
          start++;
        }
        if (start == n) break;

        var end = start;
        while (end + 1 < n && validAt(end + 1)) {
          end++;
        }
        line.moveTo(xAt(start), yAt(start));
        for (var i = start; i < end; i++) {
          final x0 = xAt(i), y0 = yAt(i);
          final x1 = xAt(i + 1), y1 = yAt(i + 1);
          final previousY = i == start ? y0 : yAt(i - 1);
          final nextY = i + 1 == end ? y1 : yAt(i + 2);
          final minY = math.min(y0, y1);
          final maxY = math.max(y0, y1);
          final control1Y = (y0 + (y1 - previousY) / 6)
              .clamp(minY, maxY)
              .toDouble();
          final control2Y = (y1 - (nextY - y0) / 6)
              .clamp(minY, maxY)
              .toDouble();
          line.cubicTo(
            x0 + (x1 - x0) / 3,
            control1Y,
            x1 - (x1 - x0) / 3,
            control2Y,
            x1,
            y1,
          );
        }
        start = end + 1;
      }
    }
    return line;
  }

  List<double>? _interpolateGaps(List<double> vals, List<bool> validity) {
    final validIndices = <int>[
      for (var i = 0; i < vals.length && i < validity.length; i++)
        if (validity[i]) i,
    ];
    if (validIndices.length < 2) return null;

    final result = List<double>.filled(vals.length, 0);
    for (var i = 0; i < validIndices.first; i++) {
      result[i] = vals[validIndices.first];
    }
    for (var pair = 0; pair + 1 < validIndices.length; pair++) {
      final left = validIndices[pair];
      final right = validIndices[pair + 1];
      final leftValue = vals[left];
      final delta = vals[right] - leftValue;
      for (var i = left; i <= right; i++) {
        result[i] = leftValue + delta * (i - left) / (right - left);
      }
    }
    for (var i = validIndices.last + 1; i < vals.length; i++) {
      result[i] = vals[validIndices.last];
    }
    return result;
  }

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height;
    final n = values.length;

    // guide markers behind the trace
    if (markers.isNotEmpty && n > 0) {
      final mp = Paint()
        ..color = markerColor
        ..strokeWidth = 1;
      for (final c in markers) {
        if (c < 0 || c >= n) continue;
        final x = (c + 0.5) / n * w;
        canvas.drawLine(Offset(x, 0), Offset(x, h), mp);
      }
    }

    if (n == 0) return;

    if (showGrid) {
      final grid = Paint()
        ..color = const Color(0x24FFFFFF)
        ..strokeWidth = 1;
      for (var i = 1; i < 4; i++) {
        final y = h * i / 4;
        canvas.drawLine(Offset(0, y), Offset(w, y), grid);
      }
    }

    final line = _trace(values, w, h, validity: valid, linear: linearTrace);

    if (!linearTrace && valid == null) {
      final fill = Path.from(line)
        ..lineTo(w, h)
        ..lineTo(0, h)
        ..close();
      canvas.drawPath(
        fill,
        Paint()
          ..shader = ui.Gradient.linear(Offset(0, 0), Offset(0, h), [
            accent.withValues(alpha: 0.34),
            accent.withValues(alpha: 0.02),
          ]),
      );
    }

    // Soft glow + crisp envelope line.
    canvas.drawPath(
      line,
      Paint()
        ..color = accent.withValues(alpha: 0.5)
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2.4
        ..strokeJoin = StrokeJoin.round
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 2.4),
    );
    canvas.drawPath(
      line,
      Paint()
        ..color = accent
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.4
        ..strokeJoin = StrokeJoin.round,
    );

    final average = averageValues;
    if (average != null && average.isNotEmpty) {
      final averageLine = _trace(
        average,
        w,
        h,
        validity: averageValid ?? valid,
        linear: linearTrace,
      );
      canvas.drawPath(
        averageLine,
        Paint()
          ..color = averageColor
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1.8
          ..strokeJoin = StrokeJoin.round,
      );
    }

    // Max-hold peak line on top — bright, thin, no fill.
    if (hold.isNotEmpty) {
      final holdLine = _trace(hold, w, h, validity: valid, linear: linearTrace);
      canvas.drawPath(
        holdLine,
        Paint()
          ..color = holdColor
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1.4
          ..strokeJoin = StrokeJoin.round,
      );
    }

    canvas.drawLine(
      Offset(0, h - 0.5),
      Offset(w, h - 0.5),
      Paint()
        ..color = Colors.white.withValues(alpha: 0.12)
        ..strokeWidth = 1,
    );
  }

  @override
  bool shouldRepaint(_StripPainter old) =>
      old.rev != rev ||
      old.accent != accent ||
      old.stepped != stepped ||
      old.showGrid != showGrid ||
      old.linearTrace != linearTrace ||
      old.bridgeGaps != bridgeGaps ||
      !identical(old.averageValues, averageValues) ||
      !identical(old.averageValid, averageValid) ||
      old.averageColor != averageColor ||
      !identical(old.valid, valid) ||
      !identical(old.hold, hold);
}

class SpectrumYAxis extends StatelessWidget {
  const SpectrumYAxis({super.key, required this.labels});

  /// Labels ordered top (full scale) to bottom (zero).
  final List<String> labels;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 48,
      child: Column(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        crossAxisAlignment: CrossAxisAlignment.end,
        children: [
          for (final label in labels)
            Text(
              label,
              maxLines: 1,
              style: const TextStyle(
                color: Colors.white54,
                fontSize: 8,
                fontFamily: 'monospace',
              ),
            ),
        ],
      ),
    );
  }
}

// ─────────────────────── colorbar legend ───────────────────────

/// A thin gradient bar explaining the waterfall colormap (low → high), with
/// optional end labels. Shared so every spectrum screen explains itself the
/// same way.
class WaterfallLegend extends StatelessWidget {
  const WaterfallLegend({super.key, this.low = 'low', this.high = 'high'});

  final String low;
  final String high;

  @override
  Widget build(BuildContext context) {
    final colors = <Color>[for (var i = 0; i <= 8; i++) waterfallColor(i / 8)];
    const style = TextStyle(
      color: Colors.white54,
      fontSize: 10,
      fontFamily: 'monospace',
    );
    return Row(
      children: [
        Text(low, style: style),
        const SizedBox(width: 8),
        Expanded(
          child: Container(
            height: 8,
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(4),
              gradient: LinearGradient(colors: colors),
              border: Border.all(color: Colors.white12),
            ),
          ),
        ),
        const SizedBox(width: 8),
        Text(high, style: style),
      ],
    );
  }
}
