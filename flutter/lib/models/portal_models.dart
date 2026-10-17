import 'package:flutter/material.dart';

/// A built-in captive-portal template advertised by the firmware
/// (`portal: TMPL id=.. label=".." ssid=".."`).
class PortalTemplate {
  const PortalTemplate(this.id, this.label, this.defaultSsid, this.icon,
      {this.isCustom = false});

  final String id; // firmware id (login/terms/update) or custom name
  final String label; // human label
  final String defaultSsid; // AP name used when the operator gives none
  final IconData icon;
  final bool isCustom; // operator-uploaded HTML template

  /// Known icons for the built-in templates; falls back to a generic one.
  static IconData iconFor(String id) {
    switch (id) {
      case 'login':
        return Icons.login;
      case 'terms':
        return Icons.wifi;
      case 'update':
        return Icons.system_security_update;
      case 'custom':
        return Icons.description;
      default:
        return Icons.public;
    }
  }
}

/// One captured credential submission (`portal: CRED tmpl=.. ip=.. u=".." p=".."`).
class PortalCred {
  PortalCred({
    required this.template,
    required this.ip,
    required this.user,
    required this.pass,
    required this.at,
  });

  final String template;
  final String ip;
  final String user;
  final String pass;
  final DateTime at;
}
