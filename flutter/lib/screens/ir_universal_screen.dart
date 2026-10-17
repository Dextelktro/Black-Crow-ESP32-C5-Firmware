import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/ir_models.dart';
import '../services/device_controller.dart';
import 'ir_hub_screen.dart';
import 'ir_universal_remote_screen.dart';

/// Grid of universal-remote categories (TV, A/C, Projector, LED, Fan, Audio).
class IrUniversalScreen extends StatelessWidget {
  const IrUniversalScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final ctrl = context.watch<DeviceController>();
    return Scaffold(
      appBar: AppBar(title: const Text('Infrared Universal')),
      body: GridView.count(
        padding: const EdgeInsets.all(16),
        crossAxisCount: 2,
        crossAxisSpacing: 14,
        mainAxisSpacing: 14,
        childAspectRatio: 0.98,
        children: [
          for (final cat in kIrCategories)
            _CategoryCard(
              category: cat,
              onTap: () => pushIr(
                context,
                ctrl,
                IrUniversalRemoteScreen(category: cat),
              ),
            ),
        ],
      ),
    );
  }
}

class _CategoryCard extends StatelessWidget {
  const _CategoryCard({required this.category, required this.onTap});
  final IrCategory category;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final color = category.color;
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(18),
        child: Container(
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(18),
            gradient: LinearGradient(
              colors: [
                color.withValues(alpha: 0.16),
                Colors.white.withValues(alpha: 0.02),
              ],
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
            ),
            border: Border.all(color: color.withValues(alpha: 0.4)),
          ),
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Container(
                    width: 46,
                    height: 46,
                    decoration: BoxDecoration(
                      color: color.withValues(alpha: 0.18),
                      borderRadius: BorderRadius.circular(13),
                    ),
                    child: Icon(category.icon, color: color, size: 26),
                  ),
                  const Spacer(),
                  if (!category.builtIn)
                    Container(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 7, vertical: 3),
                      decoration: BoxDecoration(
                        color: Colors.white.withValues(alpha: 0.06),
                        borderRadius: BorderRadius.circular(20),
                        border:
                            Border.all(color: color.withValues(alpha: 0.4)),
                      ),
                      child: Text('CAPTURE',
                          style: TextStyle(
                              color: color,
                              fontSize: 8,
                              letterSpacing: 1,
                              fontWeight: FontWeight.bold)),
                    ),
                ],
              ),
              const Spacer(),
              Text(
                category.label.replaceFirst('Universal ', ''),
                style: const TextStyle(
                    color: Colors.white,
                    fontSize: 17,
                    fontWeight: FontWeight.bold),
              ),
              const SizedBox(height: 4),
              Text(
                category.builtIn
                    ? '${category.buttons.length} buttons · built-in'
                    : 'Capture & emulate',
                style: const TextStyle(color: Colors.white54, fontSize: 11),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
