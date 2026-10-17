import 'package:flutter/material.dart';

import '../services/device_controller.dart';

class DeviceIssueBanner extends StatelessWidget {
  const DeviceIssueBanner({super.key, required this.controller});

  final DeviceController controller;

  @override
  Widget build(BuildContext context) {
    final issue = controller.connectionIssue;
    if (issue == null) return const SizedBox.shrink();

    return MaterialBanner(
      content: Text(issue),
      leading: const Icon(Icons.warning_amber, color: Colors.red),
      actions: [
        TextButton(
          onPressed: controller.dismissConnectionIssue,
          child: const Text('DISMISS'),
        ),
      ],
    );
  }
}
