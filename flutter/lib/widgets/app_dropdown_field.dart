import 'package:flutter/material.dart';

import '../theme/app_theme.dart';

class AppDropdownField<T> extends StatelessWidget {
  const AppDropdownField({
    super.key,
    required this.label,
    required this.value,
    required this.valueLabel,
    required this.itemBuilder,
    required this.onSelected,
  });

  final String label;
  final T value;
  final String valueLabel;
  final PopupMenuItemBuilder<T> itemBuilder;
  final PopupMenuItemSelected<T> onSelected;

  @override
  Widget build(BuildContext context) {
    return InputDecorator(
      decoration: InputDecoration(labelText: label),
      child: PopupMenuButton<T>(
        initialValue: value,
        onSelected: onSelected,
        itemBuilder: itemBuilder,
        position: PopupMenuPosition.under,
        popUpAnimationStyle: appPopupMenuAnimationStyle,
        child: Row(
          children: [
            Expanded(
              child: Text(
                valueLabel,
                style: Theme.of(context).textTheme.bodyLarge,
              ),
            ),
            const Icon(Icons.arrow_drop_down),
          ],
        ),
      ),
    );
  }
}
