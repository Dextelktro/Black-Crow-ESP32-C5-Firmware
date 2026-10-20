import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:path_provider/path_provider.dart';
import 'package:permission_handler/permission_handler.dart';

class DownloadStore {
  DownloadStore._({
    required this.fileName,
    required this.location,
    this.token,
    this.file,
    this.sink,
  });

  static const MethodChannel _channel = MethodChannel('deauther/platform');

  final String fileName;
  final String location;
  final String? token;
  final File? file;
  final IOSink? sink;

  static Future<DownloadStore> create(String sourcePath) async {
    final name = _safeName(sourcePath);
    if (Platform.isAndroid) {
      final sdk = await _channel.invokeMethod<int>('androidSdkInt');
      if (sdk != null && sdk < 29) {
        final status = await Permission.storage.request();
        if (!status.isGranted) {
          throw StateError(
            'Storage permission is required to write Downloads.',
          );
        }
      }
      final response = await _channel.invokeMapMethod<String, dynamic>(
        'downloadBegin',
        {'name': name, 'mimeType': _mimeType(name)},
      );
      if (response == null ||
          response['token'] is! String ||
          response['location'] is! String) {
        throw StateError('Android did not return the Downloads destination.');
      }
      return DownloadStore._(
        fileName: name,
        token: response['token'] as String,
        location: response['location'] as String,
      );
    }

    if (!Platform.isWindows && !Platform.isLinux && !Platform.isMacOS) {
      throw UnsupportedError('Downloads are not supported on this platform.');
    }
    final directory = await getDownloadsDirectory();
    if (directory == null) {
      throw StateError('The system Downloads directory is unavailable.');
    }
    await directory.create(recursive: true);
    final file = await _uniqueFile(directory, name);
    final sink = file.openWrite(mode: FileMode.writeOnly);
    return DownloadStore._(
      fileName: name,
      location: file.path,
      file: file,
      sink: sink,
    );
  }

  Future<void> write(Uint8List bytes) async {
    if (token != null) {
      await _channel.invokeMethod<void>('downloadAppend', {
        'token': token,
        'bytes': bytes,
      });
    } else {
      sink!.add(bytes);
    }
  }

  Future<String> finish() async {
    if (token != null) {
      return await _channel.invokeMethod<String>('downloadFinish', {
            'token': token,
          }) ??
          location;
    }
    await sink!.flush();
    await sink!.close();
    return location;
  }

  Future<void> abort() async {
    if (token != null) {
      await _channel.invokeMethod<void>('downloadAbort', {'token': token});
    } else {
      try {
        await sink!.close();
      } finally {
        if (await file!.exists()) {
          await file!.delete();
        }
      }
    }
  }

  static Future<File> _uniqueFile(Directory directory, String name) async {
    var candidate = File('${directory.path}${Platform.pathSeparator}$name');
    var suffix = 1;
    while (await candidate.exists()) {
      final dot = name.lastIndexOf('.');
      final base = dot > 0 ? name.substring(0, dot) : name;
      final extension = dot > 0 ? name.substring(dot) : '';
      candidate = File(
        '${directory.path}${Platform.pathSeparator}$base ($suffix)$extension',
      );
      suffix++;
    }
    return candidate;
  }

  static String _safeName(String path) {
    final name = path.split(RegExp(r'[/\\]')).last;
    final safe = name.replaceAll(RegExp(r'[^A-Za-z0-9._-]'), '_');
    if (safe.isEmpty || safe == '.' || safe == '..') {
      throw ArgumentError.value(path, 'path', 'Invalid file name.');
    }
    return safe;
  }

  static String _mimeType(String name) {
    final extension = name.split('.').last.toLowerCase();
    return switch (extension) {
      'txt' || 'log' || 'csv' || 'json' => 'text/plain',
      'jpg' || 'jpeg' => 'image/jpeg',
      'png' => 'image/png',
      'pdf' => 'application/pdf',
      _ => 'application/octet-stream',
    };
  }
}
