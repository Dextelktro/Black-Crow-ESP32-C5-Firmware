import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';
import '../services/settings.dart';

const _kAccent = Color(0xFFFFB000);

class LedScreen extends StatefulWidget {
  const LedScreen({super.key});

  @override
  State<LedScreen> createState() => _LedScreenState();
}

class _LedScreenState extends State<LedScreen> {
  String _effect = 'solid';
  double _r = 0, _g = 255, _b = 128;
  double _brightness = 64;
  // Set from Settings at the top of build(); true for the XIAO's single-colour
  // USER_LED (intensity only), false for the DevKit's addressable WS2812.
  bool _monochrome = false;

  static const _effects = [
    ('auto', Icons.auto_mode),
    ('off', Icons.power_settings_new),
    ('solid', Icons.circle),
    ('blink', Icons.flash_on),
    ('breathe', Icons.air),
    ('strobe', Icons.bolt),
    ('rainbow', Icons.gradient),
  ];

  static const _swatches = [
    Color(0xFFFF0000), Color(0xFFFF7A00), Color(0xFFFFE000),
    Color(0xFF39FF14), Color(0xFF00E5FF), Color(0xFF2E7BFF),
    Color(0xFF9B5CFF), Color(0xFFFF00AA), Color(0xFFFFFFFF),
    Color(0xFF2EE6C5),
  ];

  bool get _usesColor =>
      _effect == 'solid' || _effect == 'blink' ||
      _effect == 'breathe' || _effect == 'strobe';

  void _apply() {
    final ctrl = context.read<DeviceController>();
    final monochrome = context.read<Settings>().board.ledIsMonochrome;
    Future<void> f;
    if (_effect == 'auto') {
      f = ctrl.ledAuto();
    } else if (_effect == 'off') {
      f = ctrl.ledEffect('off', 0, 0, 0);
    } else if (monochrome) {
      // Single-colour LED: drive all channels at full so the brightness
      // slider controls intensity (firmware collapses RGB -> max channel).
      f = ctrl.ledEffect(_effect, 255, 255, 255);
    } else {
      f = ctrl.ledEffect(_effect, _r.round(), _g.round(), _b.round());
    }
    unawaited(f.catchError((Object e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('LED command failed: $e')),
      );
    }));
  }

  void _applyBrightness() {
    final ctrl = context.read<DeviceController>();
    unawaited(ctrl.ledBrightness(_brightness.round()).catchError((Object e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('LED command failed: $e')),
      );
    }));
  }

  void _pickSwatch(Color c) {
    setState(() {
      _r = c.r * 255;
      _g = c.g * 255;
      _b = c.b * 255;
      if (!_usesColor) _effect = 'solid';
    });
    _apply();
  }

  Color get _previewColor {
    final scale = _brightness / 255.0;
    if (_effect == 'off') return Colors.black;
    if (_monochrome) {
      // Amber USER_LED scaled by brightness.
      return Color.fromARGB(255, (0xFF * scale).round(), (0xB0 * scale).round(), 0);
    }
    if (_effect == 'rainbow') return _kAccent;
    return Color.fromARGB(
      255,
      (_r * scale).round(),
      (_g * scale).round(),
      (_b * scale).round(),
    );
  }

  @override
  Widget build(BuildContext context) {
    _monochrome = context.watch<Settings>().board.ledIsMonochrome;
    return Scaffold(
      appBar: AppBar(
        iconTheme: const IconThemeData(color: _kAccent),
        title: const Text(
          'LED CONTROL',
          style: TextStyle(
            color: _kAccent,
            fontWeight: FontWeight.bold,
            letterSpacing: 2,
            fontSize: 16,
          ),
        ),
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          // ── Preview ────────────────────────────────────────
          Center(
            child: Container(
              width: 120,
              height: 120,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                gradient: (_effect == 'rainbow' && !_monochrome)
                    ? const SweepGradient(colors: [
                        Color(0xFFFF0000), Color(0xFFFFFF00), Color(0xFF00FF00),
                        Color(0xFF00FFFF), Color(0xFF0000FF), Color(0xFFFF00FF),
                        Color(0xFFFF0000),
                      ])
                    : null,
                color: (_effect == 'rainbow' && !_monochrome) ? null : _previewColor,
                boxShadow: [
                  BoxShadow(
                    color: _previewColor.withValues(alpha: 0.6),
                    blurRadius: 32,
                    spreadRadius: 2,
                  ),
                ],
                border: Border.all(color: Colors.white24),
              ),
              child: _effect == 'off'
                  ? const Icon(Icons.power_settings_new, color: Colors.white24)
                  : null,
            ),
          ),
          const SizedBox(height: 20),

          _label('EFFECT'),
          const SizedBox(height: 8),
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: _effects.map((e) {
              final sel = _effect == e.$1;
              return ChoiceChip(
                selected: sel,
                label: Text(e.$1.toUpperCase()),
                avatar: Icon(e.$2,
                    size: 16, color: sel ? Colors.black : _kAccent),
                labelStyle: TextStyle(
                  color: sel ? Colors.black : Colors.white70,
                  fontSize: 11,
                  fontWeight: FontWeight.bold,
                ),
                selectedColor: _kAccent,
                backgroundColor: Colors.white.withValues(alpha: 0.05),
                side: BorderSide(color: _kAccent.withValues(alpha: 0.3)),
                onSelected: (_) {
                  setState(() => _effect = e.$1);
                  _apply();
                },
              );
            }).toList(),
          ),
          const SizedBox(height: 20),

          if (_usesColor && !_monochrome) ...[
            _label('COLOR'),
            const SizedBox(height: 8),
            Wrap(
              spacing: 10,
              runSpacing: 10,
              children: _swatches.map((c) {
                return GestureDetector(
                  onTap: () => _pickSwatch(c),
                  child: Container(
                    width: 34,
                    height: 34,
                    decoration: BoxDecoration(
                      color: c,
                      shape: BoxShape.circle,
                      boxShadow: [
                        BoxShadow(
                            color: c.withValues(alpha: 0.5), blurRadius: 8),
                      ],
                      border: Border.all(color: Colors.white24),
                    ),
                  ),
                );
              }).toList(),
            ),
            const SizedBox(height: 16),
            _channel('R', _r, const Color(0xFFFF5252),
                (v) => setState(() => _r = v)),
            _channel('G', _g, const Color(0xFF69F0AE),
                (v) => setState(() => _g = v)),
            _channel('B', _b, const Color(0xFF448AFF),
                (v) => setState(() => _b = v)),
            const SizedBox(height: 12),
          ],

          _label('BRIGHTNESS'),
          Row(
            children: [
              const Icon(Icons.brightness_low, color: Colors.white38, size: 18),
              Expanded(
                child: Slider(
                  value: _brightness,
                  min: 1,
                  max: 255,
                  activeColor: _kAccent,
                  label: '${_brightness.round()}',
                  onChanged: (v) => setState(() => _brightness = v),
                  onChangeEnd: (_) => _applyBrightness(),
                ),
              ),
              const Icon(Icons.brightness_high, color: Colors.white70, size: 20),
            ],
          ),
          const SizedBox(height: 20),

          Container(
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(
              color: Colors.white.withValues(alpha: 0.03),
              borderRadius: BorderRadius.circular(10),
              border: Border.all(color: Colors.white12),
            ),
            child: Row(
              children: [
                const Icon(Icons.memory, color: Colors.white38, size: 16),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    _monochrome
                        ? 'Single-colour USER_LED · GPIO27 (XIAO ESP32-C5), driven '
                              'by PWM (intensity only, colour is ignored). "Auto" '
                              'shows status by pulse: idle slow-breathes, linked '
                              'fast-breathes, attack blinks.'
                        : 'On-board WS2812 RGB LED · GPIO27 (ESP32-C5-DevKitC-1). '
                              '"Auto" shows device status (green idle / blue linked / red attack).',
                    style: const TextStyle(color: Colors.white38, fontSize: 11),
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _label(String t) => Text(
        t,
        style: const TextStyle(
          color: _kAccent,
          fontSize: 11,
          letterSpacing: 2,
          fontWeight: FontWeight.bold,
        ),
      );

  Widget _channel(
      String name, double value, Color color, ValueChanged<double> onChanged) {
    return Row(
      children: [
        SizedBox(
          width: 16,
          child: Text(name,
              style: TextStyle(
                  color: color, fontWeight: FontWeight.bold, fontSize: 13)),
        ),
        Expanded(
          child: Slider(
            value: value,
            min: 0,
            max: 255,
            activeColor: color,
            label: '${value.round()}',
            onChanged: onChanged,
            onChangeEnd: (_) {
              if (_usesColor) _apply();
            },
          ),
        ),
        SizedBox(
          width: 34,
          child: Text('${value.round()}',
              textAlign: TextAlign.right,
              style: const TextStyle(
                  color: Colors.white70,
                  fontFamily: 'monospace',
                  fontSize: 12)),
        ),
      ],
    );
  }
}
