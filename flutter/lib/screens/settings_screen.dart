import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../services/api_server.dart';
import '../services/settings.dart';

class SettingsScreen extends StatefulWidget {
  const SettingsScreen({super.key});

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  static const MethodChannel _platform = MethodChannel('deauther/platform');

  late final TextEditingController _portCtrl;
  late final Future<Map<String, String>> _appInfo;

  @override
  void initState() {
    super.initState();
    _portCtrl = TextEditingController(
      text: '${context.read<Settings>().apiPort}',
    );
    _appInfo = Platform.isAndroid ? _loadAppInfo() : Future.value(const {});
  }

  Future<Map<String, String>> _loadAppInfo() async {
    final info = await _platform.invokeMapMethod<String, String>('appInfo');
    if (info == null) {
      throw StateError('App version information is unavailable');
    }
    return info;
  }

  @override
  void dispose() {
    _portCtrl.dispose();
    super.dispose();
  }

  Future<void> _applyPort() async {
    final settings = context.read<Settings>();
    final port = int.tryParse(_portCtrl.text);
    if (port == null || port < 1024 || port > 65535) {
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(const SnackBar(content: Text('Port must be 1024–65535')));
      return;
    }
    try {
      await settings.setApiPort(port);
    } catch (error) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Unable to save API port: $error')),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    final settings = context.watch<Settings>();
    final api = context.watch<ApiServer>();

    return Scaffold(
      appBar: AppBar(title: const Text('Settings')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          // ── API server ─────────────────────────────────────────────
          const Text(
            'HTTP API',
            style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16),
          ),
          if (settings.loadIssue != null) ...[
            const SizedBox(height: 8),
            Text(
              settings.loadIssue!,
              style: TextStyle(color: Theme.of(context).colorScheme.error),
            ),
          ],
          const SizedBox(height: 8),
          SwitchListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('Enable API server'),
            subtitle: Text(
              api.error ??
                  (settings.apiEnabled
                      ? (api.isRunning
                            ? 'Listening on 127.0.0.1:${api.boundPort}'
                            : 'Starting on 127.0.0.1:${settings.apiPort}…')
                      : 'Disabled'),
              style: api.error == null
                  ? null
                  : TextStyle(color: Theme.of(context).colorScheme.error),
            ),
            value: settings.apiEnabled,
            onChanged: (v) async {
              try {
                await settings.setApiEnabled(v);
              } catch (error) {
                if (!mounted) return;
                ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(content: Text('Unable to save API setting: $error')),
                );
              }
            },
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              Expanded(
                child: TextField(
                  controller: _portCtrl,
                  enabled: !settings.apiEnabled,
                  keyboardType: TextInputType.number,
                  inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                  decoration: const InputDecoration(
                    labelText: 'Port',
                    helperText: 'Disable the server to change the port',
                    border: OutlineInputBorder(),
                    isDense: true,
                  ),
                ),
              ),
              const SizedBox(width: 12),
              ElevatedButton(
                onPressed: settings.apiEnabled ? null : _applyPort,
                child: const Text('Apply'),
              ),
            ],
          ),

          // ── Endpoint reference ──────────────────────────────────────
          if (settings.apiEnabled) ...[
            const SizedBox(height: 24),
            const Divider(),
            const SizedBox(height: 8),
            const Text(
              'Endpoints',
              style: TextStyle(fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 8),
            _ep('GET', '/', 'Health check / connected status'),
            _ep('GET', '/device', 'Full device state (JSON)'),
            _ep('GET', '/aps', 'Scanned access points'),
            _ep('GET', '/log', 'Raw firmware log'),
            _ep('POST', '/scan', 'Start Wi-Fi scan'),
            _ep('POST', '/attack/start', 'Start the selected attack'),
            _ep('POST', '/attack/stop', 'Stop current attack'),
            _ep('POST', '/nuke', 'Body: {"secs": 30}'),
            _ep('POST', '/cmd', 'Body: {"cmd": "help"}'),
          ],
          const SizedBox(height: 24),
          const Divider(),
          const SizedBox(height: 8),
          const Text(
            'About',
            style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16),
          ),
          const SizedBox(height: 8),
          if (!Platform.isAndroid)
            const Text(
              'App and Android version details are available on Android.',
              style: TextStyle(color: Colors.grey),
            )
          else
            FutureBuilder<Map<String, String>>(
              future: _appInfo,
              builder: (context, snapshot) {
                if (snapshot.hasError) {
                  return Text(
                    'Unable to load version information: ${snapshot.error}',
                    style: TextStyle(
                      color: Theme.of(context).colorScheme.error,
                    ),
                  );
                }
                if (!snapshot.hasData) {
                  return const LinearProgressIndicator();
                }
                final info = snapshot.data!;
                return Column(
                  children: [
                    _infoRow('App version', info['appVersion']!),
                    _infoRow('Android version', info['androidVersion']!),
                    _infoRow('Flutter version', info['flutterVersion']!),
                  ],
                );
              },
            ),
        ],
      ),
    );
  }

  Widget _infoRow(String label, String value) {
    return ListTile(
      contentPadding: EdgeInsets.zero,
      dense: true,
      title: Text(label),
      trailing: Text(value, style: const TextStyle(color: Colors.grey)),
    );
  }

  Widget _ep(String method, String path, String desc) {
    final color = method == 'GET' ? Colors.teal : Colors.orange;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(
        children: [
          Container(
            width: 46,
            padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 2),
            decoration: BoxDecoration(
              color: color.withAlpha(40),
              borderRadius: BorderRadius.circular(4),
              border: Border.all(color: color.withAlpha(120)),
            ),
            child: Text(
              method,
              textAlign: TextAlign.center,
              style: TextStyle(
                color: color,
                fontSize: 11,
                fontWeight: FontWeight.bold,
              ),
            ),
          ),
          const SizedBox(width: 8),
          Text(
            path,
            style: const TextStyle(fontFamily: 'monospace', fontSize: 13),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: Text(
              desc,
              style: const TextStyle(color: Colors.grey, fontSize: 12),
            ),
          ),
        ],
      ),
    );
  }
}
