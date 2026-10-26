import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../services/device_controller.dart';

const _accent = Color(0xFF2EE6C5);
const _platform = MethodChannel('deauther/platform');
const _maxUploadFiles = 512;
const _maxUploadDirectories = 2048;
const _maxUploadDepth = 32;

class _UploadSelection {
  const _UploadSelection({
    required this.files,
    required this.directories,
    required this.label,
    this.cleanupToken,
  });

  final List<EspFlashUploadSource> files;
  final List<String> directories;
  final String label;
  final String? cleanupToken;
}

class EspFlasherScreen extends StatefulWidget {
  const EspFlasherScreen({super.key});

  @override
  State<EspFlasherScreen> createState() => _EspFlasherScreenState();
}

class _EspFlasherScreenState extends State<EspFlasherScreen> {
  bool _selectingUpload = false;
  bool _loadingDeviceImages = false;
  DeviceController? _controller;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback(
      (_) => _refreshStatus(clearFinishedSession: true),
    );
  }

  Future<void> _refreshStatus({bool clearFinishedSession = false}) async {
    try {
      final controller = context.read<DeviceController>();
      await controller.espFlashStatus();
      if (clearFinishedSession) controller.espFlashClearSessionState();
    } catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Could not read ESP flasher status: $error')),
        );
      }
    }
  }

  Future<bool> _run(Future<void> Function() operation) async {
    final messenger = ScaffoldMessenger.of(context);
    try {
      await operation();
      if (mounted) {
        messenger.showSnackBar(
          const SnackBar(content: Text('Command accepted by the device.')),
        );
      }
      return true;
    } catch (error) {
      if (mounted) {
        messenger.showSnackBar(SnackBar(content: Text('ESP flasher: $error')));
      }
      return false;
    }
  }

  Future<List<EspFlashDeviceImage>?> _chooseDeviceBins(
    DeviceController controller,
    String storage,
  ) async {
    setState(() => _loadingDeviceImages = true);
    try {
      final files = await controller.espFlashListDeviceImages(storage);
      if (!mounted) return null;
      if (files.isEmpty) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              'No BIN files found in ${storage == 'lfs' ? 'LittleFS' : 'SD card'}.',
            ),
          ),
        );
        return null;
      }
      return await showModalBottomSheet<List<EspFlashDeviceImage>>(
        context: context,
        isScrollControlled: true,
        backgroundColor: const Color(0xFF111923),
        builder: (sheetContext) {
          final selected = <EspFlashDeviceImage>[];
          return StatefulBuilder(
            builder: (context, setSheetState) => SafeArea(
              child: SizedBox(
                height: MediaQuery.sizeOf(context).height * 0.78,
                child: Column(
                  children: [
                    Padding(
                      padding: const EdgeInsets.fromLTRB(20, 20, 20, 12),
                      child: Row(
                        children: [
                          const Icon(Icons.memory, color: _accent),
                          const SizedBox(width: 12),
                          Expanded(
                            child: Text(
                              '${storage == 'lfs' ? 'LittleFS' : 'SD card'} · BIN files',
                              style: Theme.of(context).textTheme.titleLarge,
                            ),
                          ),
                          IconButton(
                            onPressed: () => Navigator.pop(sheetContext),
                            icon: const Icon(Icons.close),
                          ),
                        ],
                      ),
                    ),
                    const Padding(
                      padding: EdgeInsets.symmetric(horizontal: 20),
                      child: Align(
                        alignment: Alignment.centerLeft,
                        child: Text(
                          'Select up to 3 images. Set the correct flash offset for each.',
                          style: TextStyle(color: Colors.white70),
                        ),
                      ),
                    ),
                    const Divider(height: 20),
                    Expanded(
                      child: ListView.separated(
                        itemCount: files.length,
                        separatorBuilder: (_, _) =>
                            const Divider(height: 1, indent: 68),
                        itemBuilder: (context, index) {
                          final file = files[index];
                          final isSelected = selected.any(
                            (item) => item.path == file.path,
                          );
                          return ListTile(
                            leading: Checkbox(
                              value: isSelected,
                              onChanged: (checked) {
                                setSheetState(() {
                                  if (checked == true) {
                                    if (selected.length < 3) {
                                      selected.add(file);
                                    }
                                  } else {
                                    selected.removeWhere(
                                      (item) => item.path == file.path,
                                    );
                                  }
                                });
                              },
                            ),
                            title: Text(
                              file.path.split('/').last,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                            ),
                            subtitle: Text(
                              '${file.path} · ${_formatBytes(file.size)}',
                              maxLines: 2,
                              overflow: TextOverflow.ellipsis,
                            ),
                            onTap: () {
                              setSheetState(() {
                                if (isSelected) {
                                  selected.removeWhere(
                                    (item) => item.path == file.path,
                                  );
                                } else if (selected.length < 3) {
                                  selected.add(file);
                                }
                              });
                            },
                          );
                        },
                      ),
                    ),
                    const Divider(height: 1),
                    Padding(
                      padding: const EdgeInsets.all(16),
                      child: Row(
                        children: [
                          Text('${selected.length}/3 selected'),
                          const Spacer(),
                          TextButton(
                            onPressed: () => Navigator.pop(sheetContext),
                            child: const Text('Cancel'),
                          ),
                          const SizedBox(width: 8),
                          FilledButton(
                            onPressed: selected.isEmpty
                                ? null
                                : () => Navigator.pop(
                                    sheetContext,
                                    List<EspFlashDeviceImage>.of(selected),
                                  ),
                            child: const Text('Continue'),
                          ),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
            ),
          );
        },
      );
    } catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Could not list BIN files: $error')),
        );
      }
      return null;
    } finally {
      if (mounted) setState(() => _loadingDeviceImages = false);
    }
  }

  String _formatBytes(int bytes) {
    if (bytes >= 1024 * 1024) {
      return '${(bytes / (1024 * 1024)).toStringAsFixed(2)} MiB';
    }
    if (bytes >= 1024) return '${(bytes / 1024).toStringAsFixed(1)} KiB';
    return '$bytes B';
  }

  Future<void> _showQuickFlash(DeviceController controller) =>
      _showFlashWizard(controller, quick: true);

  Future<void> _showManualFlash(DeviceController controller) =>
      _showFlashWizard(controller);

  Future<void> _showFlashWizard(
    DeviceController controller, {
    bool quick = false,
  }) async {
    const chips = <String>[
      'auto',
      'esp8266',
      'esp32',
      'esp32s2',
      'esp32s3',
      'esp32c2',
      'esp32c3',
      'esp32c5',
      'esp32c6',
      'esp32h2',
      'esp32p4',
      'esp32c61',
      'esp32s31',
      'esp32h21',
      'esp32h4',
    ];
    var storage = 'lfs';
    var chip = 'auto';
    final selectedImages = <EspFlashDeviceImage>[];
    final offsets = <String, TextEditingController>{};
    final messenger = ScaffoldMessenger.of(context);
    await showDialog<void>(
      context: context,
      builder: (dialogContext) => StatefulBuilder(
        builder: (dialogContext, setDialogState) {
          final targetChipField = DropdownButtonFormField<String>(
            initialValue: chip,
            decoration: const InputDecoration(labelText: 'Target chip'),
            items: chips
                .map(
                  (name) => DropdownMenuItem(
                    value: name,
                    child: Text(name == 'auto' ? 'Detect automatically' : name),
                  ),
                )
                .toList(growable: false),
            onChanged: (value) {
              if (value != null) setDialogState(() => chip = value);
            },
          );
          final sourceField = DropdownButtonFormField<String>(
            initialValue: storage,
            decoration: const InputDecoration(labelText: 'BIN location'),
            items: const [
              DropdownMenuItem(value: 'lfs', child: Text('LittleFS')),
              DropdownMenuItem(value: 'sd', child: Text('SD card')),
            ],
            onChanged: (value) {
              if (value != null) {
                setDialogState(() {
                  storage = value;
                  selectedImages.clear();
                  for (final controller in offsets.values) {
                    controller.dispose();
                  }
                  offsets.clear();
                });
              }
            },
          );
          return AlertDialog(
            backgroundColor: const Color(0xFF111923),
            title: Row(
              children: [
                const Icon(Icons.bolt, color: _accent),
                const SizedBox(width: 10),
                Text(quick ? 'Quick flash' : 'Manual flash'),
              ],
            ),
            content: SingleChildScrollView(
              child: SizedBox(
                width: 420,
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    if (quick) targetChipField,
                    const SizedBox(height: 10),
                    sourceField,
                    const SizedBox(height: 8),
                    OutlinedButton.icon(
                      onPressed: _loadingDeviceImages
                          ? null
                          : () async {
                              final picked = await _chooseDeviceBins(
                                controller,
                                storage,
                              );
                              if (picked != null && dialogContext.mounted) {
                                setDialogState(() {
                                  selectedImages
                                    ..clear()
                                    ..addAll(picked);
                                  offsets.removeWhere((path, field) {
                                    if (selectedImages.any(
                                      (image) => image.path == path,
                                    )) {
                                      return false;
                                    }
                                    field.dispose();
                                    return true;
                                  });
                                  for (final image in selectedImages) {
                                    offsets.putIfAbsent(
                                      image.path,
                                      TextEditingController.new,
                                    );
                                  }
                                });
                              }
                            },
                      icon: const Icon(Icons.folder_open),
                      label: Text(
                        _loadingDeviceImages
                            ? 'Reading storage…'
                            : selectedImages.isEmpty
                            ? 'Choose BIN files (up to 3)'
                            : 'Change BIN files (${selectedImages.length}/3)',
                      ),
                    ),
                    for (
                      var index = 0;
                      index < selectedImages.length;
                      index++
                    ) ...[
                      const SizedBox(height: 8),
                      Container(
                        decoration: BoxDecoration(
                          color: const Color(0xFF1A2631),
                          borderRadius: BorderRadius.circular(14),
                          border: Border.all(
                            color: _accent.withValues(alpha: .3),
                          ),
                        ),
                        padding: const EdgeInsets.all(12),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.stretch,
                          children: [
                            Row(
                              children: [
                                const Icon(Icons.description, color: _accent),
                                const SizedBox(width: 10),
                                Expanded(
                                  child: Column(
                                    crossAxisAlignment:
                                        CrossAxisAlignment.start,
                                    children: [
                                      Text(
                                        '${index + 1}. ${selectedImages[index].path.split('/').last}',
                                        maxLines: 1,
                                        overflow: TextOverflow.ellipsis,
                                      ),
                                      Text(
                                        '${selectedImages[index].path} · ${_formatBytes(selectedImages[index].size)}',
                                        style: Theme.of(
                                          context,
                                        ).textTheme.bodySmall,
                                        maxLines: 2,
                                        overflow: TextOverflow.ellipsis,
                                      ),
                                    ],
                                  ),
                                ),
                                IconButton(
                                  tooltip: 'Remove image',
                                  onPressed: () {
                                    final removed = selectedImages.removeAt(
                                      index,
                                    );
                                    offsets.remove(removed.path)?.dispose();
                                    setDialogState(() {});
                                  },
                                  icon: const Icon(Icons.close),
                                ),
                              ],
                            ),
                            const SizedBox(height: 8),
                            TextField(
                              controller: offsets[selectedImages[index].path],
                              decoration: InputDecoration(
                                labelText: 'Image ${index + 1} flash offset',
                                hintText: '0x10000',
                                prefixIcon: const Icon(Icons.pin),
                              ),
                              keyboardType: TextInputType.text,
                              autocorrect: false,
                            ),
                          ],
                        ),
                      ),
                    ],
                    if (!quick) ...[
                      const SizedBox(height: 10),
                      targetChipField,
                    ],
                    const SizedBox(height: 12),
                    Container(
                      decoration: BoxDecoration(
                        color: const Color(0x222EE6C5),
                        borderRadius: BorderRadius.circular(12),
                      ),
                      padding: const EdgeInsets.all(12),
                      child: const Text(
                        'Up to 3 BIN files are written in one detected target session, '
                        'each at its own configured offset. Target MD5 verification '
                        'is unavailable on ESP8266. The batch stops on the first '
                        'error; earlier images cannot be rolled back. Hold BOOT and '
                        'reset/power-cycle when prompted, then reset again after success.',
                        style: TextStyle(color: Colors.white70, fontSize: 12),
                      ),
                    ),
                  ],
                ),
              ),
            ),
            actions: [
              TextButton(
                onPressed: () => Navigator.pop(dialogContext),
                child: const Text('Cancel'),
              ),
              FilledButton.icon(
                onPressed: selectedImages.isEmpty
                    ? null
                    : () async {
                        final batchImages = <EspFlashBatchImage>[];
                        for (final image in selectedImages) {
                          final rawOffset = offsets[image.path]!.text
                              .trim()
                              .toLowerCase();
                          final parsedOffset = rawOffset.startsWith('0x')
                              ? int.tryParse(rawOffset.substring(2), radix: 16)
                              : int.tryParse(rawOffset);
                          if (parsedOffset == null ||
                              parsedOffset < 0 ||
                              parsedOffset > 0xffffffff ||
                              (parsedOffset & 3) != 0) {
                            messenger.showSnackBar(
                              SnackBar(
                                content: Text(
                                  'Enter a valid aligned offset for ${image.path.split('/').last}.',
                                ),
                              ),
                            );
                            return;
                          }
                          batchImages.add(
                            EspFlashBatchImage(
                              path: image.path,
                              offset: parsedOffset,
                            ),
                          );
                        }
                        if (batchImages.length > 3) {
                          messenger.showSnackBar(
                            const SnackBar(
                              content: Text(
                                'Select no more than 3 BIN files per batch.',
                              ),
                            ),
                          );
                          return;
                        }
                        Navigator.pop(dialogContext);
                        await _run(
                          () => controller.espFlashStartBatch(
                            storage: storage,
                            images: batchImages,
                            chip: chip,
                          ),
                        );
                      },
                icon: const Icon(Icons.play_arrow),
                label: const Text('Start detection & flash'),
              ),
            ],
          );
        },
      ),
    );
    for (final controller in offsets.values) {
      controller.dispose();
    }
  }

  Future<String?> _chooseUploadKind() => showDialog<String>(
    context: context,
    builder: (context) => AlertDialog(
      title: const Text('Choose source'),
      content: const Text('Select individual files or an entire folder.'),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context, 'files'),
          child: const Text('Files'),
        ),
        TextButton(
          onPressed: () => Navigator.pop(context, 'folder'),
          child: const Text('Folder'),
        ),
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('Cancel'),
        ),
      ],
    ),
  );

  Future<String?> _chooseUploadStorage(String label) => showDialog<String>(
    context: context,
    builder: (context) => AlertDialog(
      title: const Text('Upload destination'),
      content: Text('Send $label to:'),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context, 'lfs'),
          child: const Text('LittleFS'),
        ),
        TextButton(
          onPressed: () => Navigator.pop(context, 'sd'),
          child: const Text('SD card'),
        ),
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('Cancel'),
        ),
      ],
    ),
  );

  Future<int> _fileLength(PlatformFile file) async {
    final length = file.lengthSync() ?? await file.length();
    if (length == null) {
      throw StateError('Could not determine the size of "${file.name}".');
    }
    return length;
  }

  List<EspFlashUploadSource> _androidUploadSources(
    List<Object?> rawFiles, {
    String? rootName,
  }) {
    if (rawFiles.length > _maxUploadFiles) {
      throw StateError('Select at most $_maxUploadFiles files per upload.');
    }
    return rawFiles
        .map((item) {
          if (item is! Map<Object?, Object?>) {
            throw StateError('Android picker returned an invalid file.');
          }
          final relativePath = item['path'] as String?;
          final localPath = item['localPath'] as String?;
          final length = item['length'] as int?;
          if (relativePath == null || localPath == null || length == null) {
            throw StateError('Android picker returned incomplete file data.');
          }
          final source = File(localPath);
          return EspFlashUploadSource(
            path: rootName == null ? relativePath : '$rootName/$relativePath',
            length: length,
            openRead: source.openRead,
          );
        })
        .toList(growable: false);
  }

  Future<_UploadSelection?> _pickUploadFiles() async {
    if (Platform.isAndroid) {
      final picked = await _platform.invokeMapMethod<Object?, Object?>(
        'pickUploadFiles',
      );
      if (picked == null) return null;
      final token = picked['token'] as String?;
      try {
        final rawFiles = picked['files'] as List<Object?>?;
        if (token == null || rawFiles == null) {
          throw StateError('Android picker returned incomplete file data.');
        }
        final files = _androidUploadSources(rawFiles);
        return _UploadSelection(
          files: files,
          directories: const [],
          label: '${files.length} file(s)',
          cleanupToken: token,
        );
      } catch (_) {
        if (token != null) {
          await _platform.invokeMethod<void>('deleteUploadFolder', {
            'token': token,
          });
        }
        rethrow;
      }
    }

    final picked = await FilePicker.pickFiles(
      dialogTitle: 'Select files to upload',
    );
    if (picked.isEmpty) return null;
    if (picked.length > _maxUploadFiles) {
      throw StateError('Select at most $_maxUploadFiles files per upload.');
    }
    final sources = <EspFlashUploadSource>[];
    for (final file in picked) {
      final length = await _fileLength(file);
      sources.add(
        EspFlashUploadSource(
          path: file.name,
          length: length,
          openRead: file.readAsByteStream,
        ),
      );
    }
    return _UploadSelection(
      files: sources,
      directories: const [],
      label: '${sources.length} file(s)',
    );
  }

  String _directoryName(String path) {
    final normalized = path
        .replaceAll('\\', '/')
        .replaceFirst(RegExp(r'/+$'), '');
    final name = normalized.split('/').last;
    if (name.isEmpty) throw StateError('Could not determine the folder name.');
    return name;
  }

  Future<_UploadSelection?> _pickUploadFolder() async {
    if (Platform.isAndroid) {
      final picked = await _platform.invokeMapMethod<Object?, Object?>(
        'pickUploadFolder',
      );
      if (picked == null) return null;
      final token = picked['token'] as String?;
      try {
        final rootName = picked['rootName'] as String?;
        final rawFiles = picked['files'] as List<Object?>?;
        final rawDirectories = picked['directories'] as List<Object?>?;
        if (token == null ||
            rootName == null ||
            rawFiles == null ||
            rawDirectories == null) {
          throw StateError('Android folder picker returned incomplete data.');
        }
        final files = _androidUploadSources(rawFiles, rootName: rootName);
        final directories = <String>[rootName];
        for (final item in rawDirectories) {
          if (item is! String || item.isEmpty) {
            throw StateError(
              'Android folder picker returned an invalid folder.',
            );
          }
          directories.add('$rootName/$item');
        }
        return _UploadSelection(
          files: files,
          directories: directories,
          label: rootName,
          cleanupToken: token,
        );
      } catch (error) {
        if (token != null) {
          await _platform.invokeMethod<void>('deleteUploadFolder', {
            'token': token,
          });
        }
        rethrow;
      }
    }

    final path = await FilePicker.getDirectoryPath(
      dialogTitle: 'Select a folder to upload',
    );
    if (path == null) return null;
    final root = Directory(path);
    final rootPath = root.absolute.path
        .replaceAll('\\', '/')
        .replaceFirst(RegExp(r'/+$'), '');
    final rootName = _directoryName(rootPath);
    if (FileSystemEntity.typeSync(rootPath, followLinks: false) !=
        FileSystemEntityType.directory) {
      throw StateError('Select a real folder, not a symbolic link.');
    }
    final files = <EspFlashUploadSource>[];
    final directories = <String>[rootName];

    Future<void> walk(Directory directory, int depth) async {
      if (depth > _maxUploadDepth) {
        throw StateError(
          'Selected folder nesting exceeds $_maxUploadDepth levels.',
        );
      }
      await for (final entity in directory.list(followLinks: false)) {
        if (entity is Link) {
          throw StateError('Symbolic links are not supported in uploads.');
        }
        final absolutePath = entity.absolute.path.replaceAll('\\', '/');
        if (!absolutePath.startsWith('$rootPath/')) {
          throw StateError('Selected folder contains an invalid path.');
        }
        final relativePath = absolutePath.substring(rootPath.length + 1);
        if (entity is Directory) {
          if (directories.length >= _maxUploadDirectories) {
            throw StateError(
              'Select a folder with at most $_maxUploadDirectories directories.',
            );
          }
          directories.add('$rootName/$relativePath');
          await walk(entity, depth + 1);
        } else if (entity is File) {
          if (files.length >= _maxUploadFiles) {
            throw StateError(
              'Select a folder with at most $_maxUploadFiles files.',
            );
          }
          files.add(
            EspFlashUploadSource(
              path: '$rootName/$relativePath',
              length: await entity.length(),
              openRead: entity.openRead,
            ),
          );
        } else {
          throw StateError('The selected folder contains an unsupported item.');
        }
      }
    }

    await walk(root, 0);
    return _UploadSelection(
      files: files,
      directories: directories,
      label: rootName,
    );
  }

  Future<void> _uploadFromPicker(DeviceController controller) async {
    String? stagedFolderToken;
    var uploaded = false;
    var transferStarted = false;
    setState(() => _selectingUpload = true);
    try {
      final kind = await _chooseUploadKind();
      if (kind == null || !mounted) return;
      final selection = kind == 'folder'
          ? await _pickUploadFolder()
          : await _pickUploadFiles();
      if (selection == null) return;
      stagedFolderToken = selection.cleanupToken;
      if (!mounted) return;
      final storage = await _chooseUploadStorage(selection.label);
      if (storage == null || !mounted) return;

      setState(() => _selectingUpload = false);
      final transfer = controller.espFlashUploadBatch(
        storage: storage,
        files: selection.files,
        directories: selection.directories,
      );
      transferStarted = controller.espFlashUploading;
      await transfer;
      uploaded = true;
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              selection.files.isEmpty
                  ? 'Created folder "${selection.label}" on '
                        '${storage == 'lfs' ? 'LittleFS' : 'SD card'}.'
                  : 'Uploaded ${selection.files.length} file(s) to '
                        '${storage == 'lfs' ? 'LittleFS' : 'SD card'}.',
            ),
          ),
        );
      }
    } catch (error) {
      if (mounted) {
        final completed = transferStarted
            ? controller.espFlashUploadCompletedFiles
            : 0;
        final total = transferStarted ? controller.espFlashUploadFileCount : 0;
        final detail = total > 0 ? ' ($completed/$total files verified)' : '';
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text('Upload failed$detail: $error')));
      }
    } finally {
      if (stagedFolderToken != null) {
        try {
          await _platform.invokeMethod<void>('deleteUploadFolder', {
            'token': stagedFolderToken,
          });
        } catch (error) {
          if (mounted) {
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(content: Text('Could not remove staged folder: $error')),
            );
          }
        }
      }
      if (Platform.isIOS) {
        try {
          await FilePicker.clearTemporaryFiles();
        } catch (error) {
          if (mounted) {
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(content: Text('Could not clear picker cache: $error')),
            );
          }
        }
      }
      if (mounted) setState(() => _selectingUpload = false);
      if (uploaded && mounted) {
        await _refreshStatus();
      }
    }
  }

  Future<void> _showSettings() async {
    await showDialog<void>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('ESP Flasher Settings'),
        content: const Text(
          'Host UART: UART1\r\n'
          'Host TX: GPIO25 → target RX\r\n'
          'Host RX: GPIO3 ← target TX\r\n'
          'Target BOOT: GPIO23 (open-drain)\r\n'
          'Target RESET: -1 (manual reset required)\r\n\r\n'
          'UART0/USB serial is not claimed by the flasher. The session checks '
          'that UART1 is unowned before install and deletes its driver when done. '
          'Confirm GPIO availability against your exact board schematic.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('Close'),
          ),
        ],
      ),
    );
  }

  void _showAbout() {
    showAboutDialog(
      context: context,
      applicationName: 'ESP Flasher UART',
      applicationVersion: 'ESP-IDF 5.5.1',
      children: const [
        Text(
          'Serial flashing streams the selected BIN in bounded blocks. '
          'Target chip and flash size are checked before writing, and the '
          'device reports verification and cleanup failures explicitly.',
        ),
      ],
    );
  }

  Widget _option({
    required IconData icon,
    required String title,
    required String subtitle,
    required VoidCallback? onTap,
  }) {
    return Card(
      child: ListTile(
        leading: Icon(icon, color: onTap == null ? Colors.white38 : _accent),
        title: Text(title),
        subtitle: Text(subtitle),
        trailing: const Icon(Icons.chevron_right),
        onTap: onTap,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final controller = context.watch<DeviceController>();
    _controller = controller;
    final flashLogs = controller.rawLog
        .split(RegExp(r'[\r\n]+'))
        .where((line) => line.startsWith('espflash:'))
        .toList(growable: false);
    final recentFlashLogs = flashLogs.length > 8
        ? flashLogs.sublist(flashLogs.length - 8)
        : flashLogs;
    final flashBusy = const {
      'starting',
      'detecting',
      'waiting',
      'flashing',
      'cancelling',
      'unknown',
    }.contains(controller.espFlashState);
    return Scaffold(
      appBar: AppBar(
        title: const Text('ESP Flasher UART'),
        actions: [
          IconButton(
            onPressed: () => _refreshStatus(),
            tooltip: 'Refresh flasher status',
            icon: const Icon(Icons.refresh),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(14),
        children: [
          Container(
            margin: const EdgeInsets.only(bottom: 12),
            decoration: BoxDecoration(
              gradient: const LinearGradient(
                colors: [Color(0xFF193B42), Color(0xFF17222D)],
                begin: Alignment.topLeft,
                end: Alignment.bottomRight,
              ),
              borderRadius: BorderRadius.circular(18),
              border: Border.all(color: _accent.withValues(alpha: .28)),
            ),
            padding: const EdgeInsets.all(18),
            child: Row(
              children: [
                Container(
                  width: 48,
                  height: 48,
                  decoration: const BoxDecoration(
                    color: Color(0x332EE6C5),
                    shape: BoxShape.circle,
                  ),
                  child: const Icon(Icons.memory, color: _accent),
                ),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        'UART FLASH CONSOLE',
                        style: Theme.of(context).textTheme.labelSmall?.copyWith(
                          color: _accent,
                          letterSpacing: 1.4,
                          fontWeight: FontWeight.w700,
                        ),
                      ),
                      const SizedBox(height: 4),
                      Text(
                        'Up to 3 images · verified where supported',
                        style: Theme.of(context).textTheme.titleMedium,
                      ),
                    ],
                  ),
                ),
                const Icon(Icons.shield_outlined, color: Colors.white60),
              ],
            ),
          ),
          _option(
            icon: Icons.flash_on,
            title: 'Quick Flash',
            subtitle: 'Choose target chip, up to 3 BIN files, and each offset',
            onTap: controller.espFlashUploading || flashBusy
                ? null
                : () => _showQuickFlash(controller),
          ),
          _option(
            icon: Icons.tune,
            title: 'Manual Flash',
            subtitle: 'Choose BIN source, target chip, and per-image offsets',
            onTap: controller.espFlashUploading || flashBusy
                ? null
                : () => _showManualFlash(controller),
          ),
          _option(
            icon: Icons.upload_file,
            title: _selectingUpload
                ? 'Preparing file selection…'
                : 'Upload files or folder',
            subtitle:
                'Choose files or a folder; transfer to LittleFS or SD with CRC verification',
            onTap:
                controller.espFlashUploading ||
                    _selectingUpload ||
                    flashBusy ||
                    controller.espFlashRemoteUploadId != null
                ? null
                : () => _uploadFromPicker(controller),
          ),
          if (controller.espFlashRemoteUploadId != null) ...[
            const SizedBox(height: 8),
            Card(
              child: Padding(
                padding: const EdgeInsets.all(14),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text(
                      'Remote upload needs recovery',
                      style: TextStyle(
                        color: _accent,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                    const SizedBox(height: 8),
                    LinearProgressIndicator(
                      value: controller.espFlashRemoteUploadSize == 0
                          ? null
                          : (controller.espFlashRemoteUploadOffset /
                                    controller.espFlashRemoteUploadSize)
                                .clamp(0.0, 1.0)
                                .toDouble(),
                    ),
                    const SizedBox(height: 8),
                    Text(
                      'Upload ${controller.espFlashRemoteUploadId}: '
                      '${controller.espFlashRemoteUploadOffset} / '
                      '${controller.espFlashRemoteUploadSize} bytes. '
                      'Cancel removes the incomplete .part file.',
                    ),
                    Align(
                      alignment: Alignment.centerRight,
                      child: TextButton(
                        onPressed: () =>
                            _run(controller.espFlashCancelRemoteUpload),
                        child: const Text('Cancel remote upload'),
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ],
          if (controller.espFlashState != 'idle' &&
              !controller.espFlashUploading) ...[
            const SizedBox(height: 8),
            Card(
              child: Padding(
                padding: const EdgeInsets.all(14),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      'Flash ${controller.espFlashState.toUpperCase()}',
                      style: const TextStyle(
                        color: _accent,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                    const SizedBox(height: 8),
                    if (controller.espFlashImageBytes > 0)
                      LinearProgressIndicator(
                        value:
                            (controller.espFlashProgrammedBytes /
                                    controller.espFlashImageBytes)
                                .clamp(0.0, 1.0)
                                .toDouble(),
                      ),
                    if (controller.espFlashImageBytes > 0) ...[
                      const SizedBox(height: 6),
                      Text(
                        '${(100 * controller.espFlashProgrammedBytes / controller.espFlashImageBytes).clamp(0, 100).round()}% · '
                        '${controller.espFlashProgrammedBytes} / '
                        '${controller.espFlashImageBytes} bytes',
                        style: Theme.of(context).textTheme.bodySmall,
                      ),
                    ],
                    const SizedBox(height: 8),
                    Text(controller.espFlashMessage),
                    if (recentFlashLogs.isNotEmpty) ...[
                      const SizedBox(height: 10),
                      Container(
                        width: double.infinity,
                        decoration: BoxDecoration(
                          color: const Color(0xFF0B1118),
                          borderRadius: BorderRadius.circular(12),
                        ),
                        padding: const EdgeInsets.all(10),
                        child: SelectableText(
                          recentFlashLogs.join('\n'),
                          style: const TextStyle(
                            fontFamily: 'monospace',
                            fontSize: 11,
                            color: Colors.white70,
                          ),
                        ),
                      ),
                    ],
                    if (controller.espFlashState == 'detecting')
                      const LinearProgressIndicator(),
                    if (controller.espFlashState == 'waiting' ||
                        controller.espFlashState == 'detecting' ||
                        controller.espFlashState == 'flashing' ||
                        controller.espFlashState == 'cancelling')
                      Align(
                        alignment: Alignment.centerRight,
                        child: TextButton(
                          onPressed: controller.espFlashCancelRequested
                              ? null
                              : () => _run(controller.espFlashCancelFlash),
                          child: Text(
                            controller.espFlashCancelRequested
                                ? 'Cancelling…'
                                : 'Cancel flash',
                          ),
                        ),
                      ),
                  ],
                ),
              ),
            ),
          ],
          if (controller.espFlashUploading) ...[
            const SizedBox(height: 8),
            LinearProgressIndicator(
              value: controller.espFlashUploadTotalBytes == 0
                  ? null
                  : controller.espFlashUploadBytes /
                        controller.espFlashUploadTotalBytes,
            ),
            ListTile(
              title: Text(
                '${controller.espFlashUploadFileIndex}/'
                '${controller.espFlashUploadFileCount} · '
                '${controller.espFlashUploadPath}',
              ),
              subtitle: Text(
                '${controller.espFlashUploadCompletedFiles}/'
                '${controller.espFlashUploadFileCount} files verified · '
                '${controller.espFlashUploadBytes} / '
                '${controller.espFlashUploadTotalBytes} bytes',
              ),
              trailing: TextButton(
                onPressed: controller.espFlashCancelUpload,
                child: const Text('Cancel'),
              ),
            ),
          ],
          if (_selectingUpload) ...[
            const SizedBox(height: 8),
            const LinearProgressIndicator(),
            const ListTile(title: Text('Selecting and preparing files…')),
          ],
          _option(
            icon: Icons.swap_horiz,
            title: 'Switch to firmware A',
            subtitle: 'Unavailable until the target OTA layout is verified',
            onTap: null,
          ),
          _option(
            icon: Icons.swap_horiz,
            title: 'Switch to firmware B',
            subtitle: 'Unavailable until the target OTA layout is verified',
            onTap: null,
          ),
          _option(
            icon: Icons.restart_alt,
            title: 'Reset board',
            subtitle: 'Unavailable: target RESET pin is configured as -1',
            onTap: null,
          ),
          _option(
            icon: Icons.memory,
            title: controller.espFlashBootAsserted
                ? 'Release target BOOT'
                : 'Enter bootloader',
            subtitle: controller.espFlashBootAsserted
                ? 'Release GPIO23 after manually resetting the target'
                : 'Assert GPIO23; manually reset or power-cycle the target',
            onTap: () async {
              final operation = controller.espFlashBootAsserted
                  ? controller.espFlashReleaseBoot
                  : controller.espFlashAssertBoot;
              await _run(operation);
            },
          ),
          _option(
            icon: Icons.settings,
            title: 'Settings',
            subtitle: 'UART and target control pin assignments',
            onTap: _showSettings,
          ),
          _option(
            icon: Icons.info_outline,
            title: 'About',
            subtitle: 'Flasher implementation and safety notes',
            onTap: _showAbout,
          ),
        ],
      ),
    );
  }

  @override
  void dispose() {
    _controller?.espFlashClearSessionState();
    super.dispose();
  }
}
