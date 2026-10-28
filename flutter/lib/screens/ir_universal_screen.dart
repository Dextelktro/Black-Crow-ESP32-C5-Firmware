import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/ir_models.dart';
import '../services/device_controller.dart';
import '../theme/app_theme.dart';
import '../widgets/glass_surface.dart';
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
      body: GridView.builder(
        padding: const EdgeInsets.fromLTRB(16, 16, 16, 24),
        // Responsive columns that stay proportional across screen widths, with
        // a fixed row height tuned to the content (no stretched empty space).
        gridDelegate: const SliverGridDelegateWithMaxCrossAxisExtent(
          maxCrossAxisExtent: 210,
          mainAxisSpacing: 14,
          crossAxisSpacing: 14,
          mainAxisExtent: 126,
        ),
        itemCount: kIrCategories.length,
        itemBuilder: (_, i) {
          final cat = kIrCategories[i];
          return _CategoryCard(
            category: cat,
            onTap: () => pushIr(
              context,
              ctrl,
              IrUniversalRemoteScreen(category: cat),
            ),
          );
        },
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
    final subtitle = category.builtIn
        ? '${category.buttons.length} commands'
        : 'Capture & emulate';
    return GlassCard(
      onTap: onTap,
      padding: const EdgeInsets.all(14),
      radius: 18,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              // Category hue survives only as a small icon tint.
              Container(
                width: 44,
                height: 44,
                decoration: BoxDecoration(
                  color: color.withValues(alpha: 0.16),
                  borderRadius: BorderRadius.circular(13),
                ),
                child: Icon(category.icon, color: color, size: 24),
              ),
              const Spacer(),
              if (!category.builtIn)
                Container(
                  padding:
                      const EdgeInsets.symmetric(horizontal: 7, vertical: 3),
                  decoration: BoxDecoration(
                    color: color.withValues(alpha: 0.08),
                    borderRadius: BorderRadius.circular(AppRadius.chip),
                    border: Border.all(color: color.withValues(alpha: 0.4)),
                  ),
                  child: Text(
                    'CAPTURE',
                    style: TextStyle(
                      color: color,
                      fontSize: 8,
                      letterSpacing: 1,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                ),
            ],
          ),
          const SizedBox(height: 12),
          Text(
            category.label.replaceFirst('Universal ', ''),
            style: const TextStyle(
              color: AppColors.text,
              fontSize: 16,
              fontWeight: FontWeight.bold,
            ),
          ),
          const SizedBox(height: 2),
          Text(
            subtitle,
            style: const TextStyle(color: AppColors.textDim, fontSize: 11),
          ),
        ],
      ),
    );
  }
}
