import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../models/portal_models.dart';
import '../services/device_controller.dart';
import '../theme/app_theme.dart';
import '../widgets/glass_surface.dart';

const _kAccent = Color(0xFFFF5252);

/// Fallback catalog shown before the firmware's `portal ls` reply arrives.
const List<PortalTemplate> _kFallbackTemplates = [
  PortalTemplate('login', 'Network Login', 'Internet WiFi', Icons.login),
  PortalTemplate('terms', 'Free WiFi Gate', 'Free WiFi', Icons.wifi),
  PortalTemplate(
      'update', 'Network Update', 'WiFi-Setup', Icons.system_security_update),
];

/// Evil Portal — captive-portal control screen. Authorized testing only.
class EvilPortalScreen extends StatefulWidget {
  const EvilPortalScreen({super.key});

  @override
  State<EvilPortalScreen> createState() => _EvilPortalScreenState();
}

class _EvilPortalScreenState extends State<EvilPortalScreen> {
  final TextEditingController _ssid = TextEditingController();
  String _selected = 'login';

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final ctrl = context.read<DeviceController>();
      ctrl.portalListTemplates();
      ctrl.portalStatus();
      ctrl.portalRefreshCreds(); // load any persisted captures
    });
  }

  @override
  void dispose() {
    _ssid.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    final templates = ctrl.portalTemplates.isNotEmpty
        ? ctrl.portalTemplates
        : _kFallbackTemplates;
    final running = ctrl.portalRunning;

    // Keep the selection valid if a custom template was removed.
    if (!templates.any((t) => t.id == _selected) && templates.isNotEmpty) {
      _selected = templates.first.id;
    }

    return Scaffold(
      appBar: AppBar(
        title: const Text('Evil Portal'),
        actions: [
          if (running)
            IconButton(
              tooltip: 'Refresh captures',
              icon: const Icon(Icons.refresh),
              onPressed: () => ctrl.portalRefreshCreds(),
            ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          const _AuthWarning(),
          const SizedBox(height: 14),
          if (running)
            _RunningCard(ctrl: ctrl, onStop: () => ctrl.portalStop())
          else ...[
            _SetupCard(
              ctrl: ctrl,
              templates: templates,
              selected: _selected,
              ssidController: _ssid,
              onSelect: (id) => setState(() => _selected = id),
              onPickSsid: (s) => setState(() => _ssid.text = s),
              onStart: () {
                final ssid = _ssid.text.trim();
                ctrl.portalStart(_selected, ssid: ssid.isEmpty ? null : ssid);
              },
            ),
            const SizedBox(height: 14),
            _CustomHtmlCard(ctrl: ctrl),
          ],
          const SizedBox(height: 18),
          _CredHeader(
            count: ctrl.portalCreds.length,
            lifetime: ctrl.portalCredsCount,
            onClear:
                ctrl.portalCreds.isEmpty ? null : () => ctrl.portalClearCreds(),
          ),
          const SizedBox(height: 8),
          if (ctrl.portalCreds.isEmpty)
            const _EmptyCreds()
          else
            ...ctrl.portalCreds.map((c) => _CredTile(cred: c)),
        ],
      ),
    );
  }
}

class _AuthWarning extends StatelessWidget {
  const _AuthWarning();

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: _kAccent.withValues(alpha: 0.10),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: _kAccent.withValues(alpha: 0.4)),
      ),
      child: Row(
        children: [
          const Icon(Icons.gpp_maybe, color: _kAccent, size: 22),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              'Captures credentials from connected clients. Use only on '
              'networks and devices you are authorized to test.',
              style: TextStyle(
                color: _kAccent.withValues(alpha: 0.95),
                fontSize: 12,
                height: 1.35,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _SetupCard extends StatelessWidget {
  const _SetupCard({
    required this.ctrl,
    required this.templates,
    required this.selected,
    required this.ssidController,
    required this.onSelect,
    required this.onPickSsid,
    required this.onStart,
  });

  final DeviceController ctrl;
  final List<PortalTemplate> templates;
  final String selected;
  final TextEditingController ssidController;
  final ValueChanged<String> onSelect;
  final ValueChanged<String> onPickSsid;
  final VoidCallback onStart;

  Future<void> _confirmDelete(BuildContext context, PortalTemplate t) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: const Text('Delete template'),
        content: Text('Remove custom template "${t.id}"?'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(c, false),
              child: const Text('Cancel')),
          TextButton(
              onPressed: () => Navigator.pop(c, true),
              child: const Text('Delete')),
        ],
      ),
    );
    if (ok == true) ctrl.portalRemoveHtml(t.id);
  }

  @override
  Widget build(BuildContext context) {
    final sel = templates.firstWhere(
      (t) => t.id == selected,
      orElse: () => templates.first,
    );
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const _Label('TEMPLATE'),
        const SizedBox(height: 8),
        ...templates.map(
          (t) => _TemplateRow(
            t: t,
            selected: t.id == selected,
            onTap: () => onSelect(t.id),
            onDelete: t.isCustom ? () => _confirmDelete(context, t) : null,
          ),
        ),
        const SizedBox(height: 14),
        const _Label('AP NAME (SSID)'),
        const SizedBox(height: 8),
        TextField(
          controller: ssidController,
          maxLength: 32,
          decoration: InputDecoration(
            hintText: 'Default: "${sel.defaultSsid}"',
            counterText: '',
            isDense: true,
            border: const OutlineInputBorder(),
            prefixIcon: const Icon(Icons.wifi_tethering, size: 20),
          ),
        ),
        const SizedBox(height: 10),
        _CloneNearby(ctrl: ctrl, onPick: onPickSsid),
        const SizedBox(height: 16),
        SizedBox(
          width: double.infinity,
          child: FilledButton.icon(
            style: FilledButton.styleFrom(backgroundColor: _kAccent),
            icon: const Icon(Icons.play_arrow),
            label: const Text('Start portal'),
            onPressed: onStart,
          ),
        ),
      ],
    );
  }
}

/// Expansion listing scanned APs so the operator can clone a nearby SSID
/// (evil-twin). Requires a prior WiFi scan (Networks tab or the button here).
class _CloneNearby extends StatelessWidget {
  const _CloneNearby({required this.ctrl, required this.onPick});

  final DeviceController ctrl;
  final ValueChanged<String> onPick;

  @override
  Widget build(BuildContext context) {
    final aps = ctrl.aps.where((a) => a.ssid.trim().isNotEmpty).toList();
    return Theme(
      data: Theme.of(context).copyWith(dividerColor: Colors.transparent),
      child: ExpansionTile(
        tilePadding: EdgeInsets.zero,
        childrenPadding: EdgeInsets.zero,
        leading: const Icon(Icons.copy_all, color: Color(0xFF4FA8FF), size: 20),
        title: const Text('Clone a nearby network',
            style: TextStyle(fontSize: 13, fontWeight: FontWeight.w600)),
        subtitle: Text('${aps.length} seen · evil-twin SSID',
            style: const TextStyle(fontSize: 11, color: Colors.white54)),
        children: [
          Row(
            children: [
              TextButton.icon(
                icon: const Icon(Icons.radar, size: 16),
                label: const Text('Rescan'),
                onPressed: () => ctrl.send('scan'),
              ),
            ],
          ),
          if (aps.isEmpty)
            const Padding(
              padding: EdgeInsets.symmetric(vertical: 8),
              child: Text('No networks yet — run a scan.',
                  style: TextStyle(color: Colors.white38, fontSize: 12)),
            )
          else
            ...aps.take(30).map(
                  (a) => ListTile(
                    dense: true,
                    contentPadding: EdgeInsets.zero,
                    leading: Icon(
                      a.is5ghz ? Icons.five_g : Icons.wifi,
                      size: 18,
                      color: Colors.white54,
                    ),
                    title: Text(a.ssid,
                        maxLines: 1, overflow: TextOverflow.ellipsis),
                    subtitle: Text('ch ${a.channel} · ${a.rssi} dBm',
                        style: const TextStyle(fontSize: 11)),
                    trailing: const Icon(Icons.north_east, size: 16),
                    onTap: () {
                      onPick(a.ssid);
                      ScaffoldMessenger.of(context).showSnackBar(
                        SnackBar(
                            content: Text('SSID set to "${a.ssid}"'),
                            duration: const Duration(seconds: 1)),
                      );
                    },
                  ),
                ),
        ],
      ),
    );
  }
}

/// Paste + upload operator-supplied HTML as a custom template.
class _CustomHtmlCard extends StatefulWidget {
  const _CustomHtmlCard({required this.ctrl});

  final DeviceController ctrl;

  @override
  State<_CustomHtmlCard> createState() => _CustomHtmlCardState();
}

class _CustomHtmlCardState extends State<_CustomHtmlCard> {
  final TextEditingController _name = TextEditingController();
  final TextEditingController _html = TextEditingController();
  bool _busy = false;

  @override
  void dispose() {
    _name.dispose();
    _html.dispose();
    super.dispose();
  }

  Future<void> _upload() async {
    final name = _name.text.trim();
    final html = _html.text;
    if (name.isEmpty || html.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Name and HTML are required')),
      );
      return;
    }
    setState(() => _busy = true);
    try {
      await widget.ctrl.portalUploadHtml(name, html);
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Uploaded "$name" (${html.length} chars)')),
      );
      _html.clear();
    } catch (_) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Upload failed')),
      );
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Theme(
      data: Theme.of(context).copyWith(dividerColor: Colors.transparent),
      child: ExpansionTile(
        tilePadding: EdgeInsets.zero,
        childrenPadding: EdgeInsets.zero,
        leading:
            const Icon(Icons.upload_file, color: Color(0xFF9B8CFF), size: 20),
        title: const Text('Upload custom HTML',
            style: TextStyle(fontSize: 13, fontWeight: FontWeight.w600)),
        subtitle: const Text('Your own portal page — like Marauder',
            style: TextStyle(fontSize: 11, color: Colors.white54)),
        children: [
          TextField(
            controller: _name,
            maxLength: 15,
            decoration: const InputDecoration(
              labelText: 'Template name',
              counterText: '',
              isDense: true,
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 8),
          TextField(
            controller: _html,
            maxLines: 6,
            style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
            decoration: const InputDecoration(
              labelText: 'Paste HTML (form must POST to /login)',
              isDense: true,
              border: OutlineInputBorder(),
              alignLabelWithHint: true,
            ),
          ),
          const SizedBox(height: 4),
          const Text(
            'Fields named email/username + password are captured.',
            style: TextStyle(color: Colors.white38, fontSize: 11),
          ),
          const SizedBox(height: 10),
          SizedBox(
            width: double.infinity,
            child: OutlinedButton.icon(
              icon: _busy
                  ? const SizedBox(
                      width: 16,
                      height: 16,
                      child: CircularProgressIndicator(strokeWidth: 2))
                  : const Icon(Icons.cloud_upload),
              label: Text(_busy ? 'Uploading…' : 'Upload template'),
              onPressed: _busy ? null : _upload,
            ),
          ),
        ],
      ),
    );
  }
}

class _Label extends StatelessWidget {
  const _Label(this.text);
  final String text;

  @override
  Widget build(BuildContext context) {
    return Text(text,
        style: const TextStyle(
            color: Colors.white38,
            fontSize: 11,
            letterSpacing: 2,
            fontWeight: FontWeight.bold));
  }
}

class _TemplateRow extends StatelessWidget {
  const _TemplateRow({
    required this.t,
    required this.selected,
    required this.onTap,
    this.onDelete,
  });

  final PortalTemplate t;
  final bool selected;
  final VoidCallback onTap;
  final VoidCallback? onDelete;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: GlassCard(
        onTap: onTap,
        padding: const EdgeInsets.all(12),
        radius: 12,
        accent: _kAccent,
        active: selected,
        child: Row(
          children: [
            Icon(t.icon, color: selected ? _kAccent : AppColors.textDim),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(t.label,
                      style: const TextStyle(
                          color: AppColors.text,
                          fontWeight: FontWeight.bold,
                          fontSize: 14)),
                  Text('SSID: ${t.defaultSsid}',
                      style: const TextStyle(
                          color: AppColors.textDim, fontSize: 11)),
                ],
              ),
            ),
            if (onDelete != null)
              IconButton(
                tooltip: 'Delete',
                icon: const Icon(Icons.delete_outline,
                    size: 20, color: AppColors.textFaint),
                onPressed: onDelete,
              ),
            if (selected)
              const Icon(Icons.check_circle, color: _kAccent, size: 20),
          ],
        ),
      ),
    );
  }
}

class _RunningCard extends StatelessWidget {
  const _RunningCard({required this.ctrl, required this.onStop});

  final DeviceController ctrl;
  final VoidCallback onStop;

  String _fmtUp(int s) {
    final m = (s ~/ 60).toString().padLeft(2, '0');
    final sec = (s % 60).toString().padLeft(2, '0');
    return '$m:$sec';
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(16),
        gradient: const LinearGradient(
          colors: [Color(0xFF2A0D0D), Color(0xFF140707)],
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
        ),
        border: Border.all(color: _kAccent.withValues(alpha: 0.5)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Icon(Icons.wifi_tethering, color: _kAccent, size: 20),
              const SizedBox(width: 8),
              Expanded(
                child: Text(
                  ctrl.portalSsid.isEmpty ? '(portal)' : ctrl.portalSsid,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                      color: Colors.white,
                      fontSize: 16,
                      fontWeight: FontWeight.bold),
                ),
              ),
              Container(
                padding:
                    const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                decoration: BoxDecoration(
                  color: _kAccent.withValues(alpha: 0.18),
                  borderRadius: BorderRadius.circular(20),
                ),
                child: const Text('LIVE',
                    style: TextStyle(
                        color: _kAccent,
                        fontSize: 10,
                        fontWeight: FontWeight.bold,
                        letterSpacing: 1.5)),
              ),
            ],
          ),
          const SizedBox(height: 4),
          Text(
            'Template ${ctrl.portalTemplate} · ch ${ctrl.portalChannel} · '
            '192.168.4.1',
            style: const TextStyle(color: Colors.white54, fontSize: 12),
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              _Stat(label: 'CLIENTS', value: '${ctrl.portalClients}'),
              _Stat(label: 'HITS', value: '${ctrl.portalHits}'),
              _Stat(label: 'CREDS', value: '${ctrl.portalCredsCount}'),
              _Stat(label: 'UPTIME', value: _fmtUp(ctrl.portalUp)),
            ],
          ),
          const SizedBox(height: 16),
          SizedBox(
            width: double.infinity,
            child: OutlinedButton.icon(
              style: OutlinedButton.styleFrom(
                foregroundColor: _kAccent,
                side: const BorderSide(color: _kAccent),
              ),
              icon: const Icon(Icons.stop),
              label: const Text('Stop portal'),
              onPressed: onStop,
            ),
          ),
        ],
      ),
    );
  }
}

class _Stat extends StatelessWidget {
  const _Stat({required this.label, required this.value});

  final String label;
  final String value;

  @override
  Widget build(BuildContext context) {
    return Expanded(
      child: Column(
        children: [
          Text(value,
              style: const TextStyle(
                  color: _kAccent,
                  fontSize: 22,
                  fontWeight: FontWeight.w800,
                  fontFamily: 'monospace')),
          Text(label,
              style: const TextStyle(color: Colors.white38, fontSize: 10)),
        ],
      ),
    );
  }
}

class _CredHeader extends StatelessWidget {
  const _CredHeader({
    required this.count,
    required this.lifetime,
    required this.onClear,
  });

  final int count;
  final int lifetime;
  final VoidCallback? onClear;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        const _Label('CAPTURED CREDENTIALS'),
        const SizedBox(width: 8),
        Text('$count', style: const TextStyle(color: _kAccent, fontSize: 11)),
        const Spacer(),
        if (onClear != null)
          TextButton(
            onPressed: onClear,
            child: const Text('Clear',
                style: TextStyle(color: _kAccent, fontSize: 12)),
          ),
      ],
    );
  }
}

class _EmptyCreds extends StatelessWidget {
  const _EmptyCreds();

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 28),
      alignment: Alignment.center,
      child: const Text(
        'No captures yet.\nSubmissions from connected clients appear here.',
        textAlign: TextAlign.center,
        style: TextStyle(color: Colors.white30, fontSize: 12, height: 1.4),
      ),
    );
  }
}

class _CredTile extends StatelessWidget {
  const _CredTile({required this.cred});

  final PortalCred cred;

  String _fmtTime(DateTime t) {
    String two(int v) => v.toString().padLeft(2, '0');
    return '${two(t.hour)}:${two(t.minute)}:${two(t.second)}';
  }

  @override
  Widget build(BuildContext context) {
    final user = cred.user.isEmpty ? '(none)' : cred.user;
    final pass = cred.pass.isEmpty ? '(none)' : cred.pass;
    return Container(
      margin: const EdgeInsets.only(bottom: 8),
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.03),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white.withValues(alpha: 0.10)),
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                _kv('user', user),
                const SizedBox(height: 2),
                _kv('pass', pass),
                const SizedBox(height: 6),
                Text(
                  '${cred.ip} · ${cred.template} · ${_fmtTime(cred.at)}',
                  style: const TextStyle(color: Colors.white38, fontSize: 11),
                ),
              ],
            ),
          ),
          IconButton(
            tooltip: 'Copy user:pass',
            icon: const Icon(Icons.copy, size: 18, color: Colors.white54),
            onPressed: () {
              Clipboard.setData(
                  ClipboardData(text: '${cred.user}:${cred.pass}'));
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(
                    content: Text('Copied'), duration: Duration(seconds: 1)),
              );
            },
          ),
        ],
      ),
    );
  }

  Widget _kv(String k, String v) {
    return RichText(
      text: TextSpan(
        children: [
          TextSpan(
            text: '$k  ',
            style: const TextStyle(color: Colors.white38, fontSize: 12),
          ),
          TextSpan(
            text: v,
            style: const TextStyle(
                color: Colors.white,
                fontSize: 13,
                fontFamily: 'monospace',
                fontWeight: FontWeight.w600),
          ),
        ],
      ),
    );
  }
}
