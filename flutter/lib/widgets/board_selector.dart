import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/settings.dart';

/// Segmented control that selects the active [BoardVariant] and persists it
/// through [Settings]. Shared by the Settings and Hardware Peripheral screens
/// so the choice (and its error handling) lives in one place.
class BoardSelector extends StatelessWidget {
  const BoardSelector({super.key, this.style});

  /// Optional visual style (e.g. the Hardware Peripheral screen's themed look).
  /// When null the default [SegmentedButton] styling is used.
  final ButtonStyle? style;

  @override
  Widget build(BuildContext context) {
    final board = context.watch<Settings>().board;
    return SegmentedButton<BoardVariant>(
      showSelectedIcon: false,
      style: style,
      segments: const [
        ButtonSegment(
          value: BoardVariant.devkit,
          label: Text('DevKitC-1'),
          icon: Icon(Icons.developer_board, size: 16),
        ),
        ButtonSegment(
          value: BoardVariant.xiao,
          label: Text('XIAO C5'),
          icon: Icon(Icons.memory, size: 16),
        ),
      ],
      selected: {board},
      onSelectionChanged: (selection) async {
        // Capture the messenger before the await so no BuildContext is used
        // across the async gap.
        final messenger = ScaffoldMessenger.of(context);
        try {
          await context.read<Settings>().setBoard(selection.first);
        } catch (error) {
          messenger.showSnackBar(
            SnackBar(content: Text('Unable to save board: $error')),
          );
        }
      },
    );
  }
}
