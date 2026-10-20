package com.brito.deauther.deauther

import android.content.ContentValues
import android.content.Intent
import android.database.Cursor
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.StatFs
import android.provider.DocumentsContract
import android.provider.MediaStore
import android.provider.OpenableColumns
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.io.FileOutputStream
import java.io.OutputStream
import java.util.UUID

class MainActivity : FlutterActivity() {
    private companion object {
        const val UPLOAD_FOLDER_PICKER_REQUEST = 4108
        const val UPLOAD_FILES_PICKER_REQUEST = 4109
        const val MAX_UPLOAD_FILES = 512
        const val MAX_UPLOAD_DIRECTORIES = 2047
        const val MAX_UPLOAD_DEPTH = 32
        const val UPLOAD_RESERVE_BYTES = 8L * 1024 * 1024
    }

    private data class DownloadOutput(
        val uri: Uri?,
        val file: File?,
        val output: OutputStream,
        val location: String,
    )

    private data class UploadDocument(
        val sourceUri: Uri,
        val relativePath: String,
        val reportedSize: Long,
    )

    private val downloads = mutableMapOf<String, DownloadOutput>()
    private var pendingUploadPickerResult: MethodChannel.Result? = null

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "deauther/platform")
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "androidSdkInt" -> result.success(Build.VERSION.SDK_INT)
                    "pickUploadFolder", "pickUploadFiles" -> {
                        if (pendingUploadPickerResult != null) {
                            result.error(
                                "UPLOAD_PICKER_BUSY",
                                "An upload picker or import is already running",
                                null,
                            )
                        } else {
                            val isFolder = call.method == "pickUploadFolder"
                            pendingUploadPickerResult = result
                            try {
                                val requestCode: Int
                                val intent: Intent
                                if (isFolder) {
                                    requestCode = UPLOAD_FOLDER_PICKER_REQUEST
                                    intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
                                        addFlags(
                                            Intent.FLAG_GRANT_READ_URI_PERMISSION or
                                                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION,
                                        )
                                    }
                                } else {
                                    requestCode = UPLOAD_FILES_PICKER_REQUEST
                                    intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                                        addCategory(Intent.CATEGORY_OPENABLE)
                                        type = "*/*"
                                        putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                                    }
                                }
                                startActivityForResult(
                                    intent,
                                    requestCode,
                                )
                            } catch (error: Exception) {
                                pendingUploadPickerResult = null
                                result.error(
                                    "UPLOAD_PICKER_FAILED",
                                    error.message,
                                    null,
                                )
                            }
                        }
                    }
                    "deleteUploadFolder" -> try {
                        val token = call.argument<String>("token")
                            ?: throw IllegalArgumentException("missing token")
                        require(Regex("^[a-fA-F0-9-]{36}$").matches(token)) {
                            "invalid staging token"
                        }
                        val parent = File(cacheDir, "upload_folders").canonicalFile
                        val staged = File(parent, token).canonicalFile
                        require(staged.parentFile == parent) { "invalid staging path" }
                        check(!staged.exists() || staged.deleteRecursively()) {
                            "could not remove staged upload folder"
                        }
                        result.success(null)
                    } catch (error: Exception) {
                        result.error(
                            "UPLOAD_FOLDER_CLEANUP_FAILED",
                            error.message,
                            null,
                        )
                    }
                    "appInfo" -> {
                        val packageInfo = packageManager.getPackageInfo(packageName, 0)
                        val versionCode = if (Build.VERSION.SDK_INT >= 28) {
                            packageInfo.longVersionCode
                        } else {
                            @Suppress("DEPRECATION")
                            packageInfo.versionCode.toLong()
                        }

                        result.success(
                            mapOf(
                                "appVersion" to "${packageInfo.versionName ?: "Unknown"} ($versionCode)",
                                "androidVersion" to "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})",
                                "flutterVersion" to BuildConfig.FLUTTER_SDK_VERSION,
                            ),
                        )
                    }
                    "downloadBegin" -> try {
                        val name = safeFileName(call.argument<String>("name"))
                        val mimeType = call.argument<String>("mimeType") ?: "application/octet-stream"
                        val token = UUID.randomUUID().toString()
                        val download = createDownload(name, mimeType)
                        downloads[token] = download
                        result.success(
                            mapOf(
                                "token" to token,
                                "location" to download.location,
                            ),
                        )
                    } catch (error: Exception) {
                        result.error("DOWNLOAD_CREATE_FAILED", error.message, null)
                    }
                    "downloadAppend" -> try {
                        val token = call.argument<String>("token")
                            ?: throw IllegalArgumentException("missing token")
                        val bytes = call.argument<ByteArray>("bytes")
                            ?: throw IllegalArgumentException("missing bytes")
                        val download = downloads[token]
                            ?: throw IllegalStateException("download is not open")
                        download.output.write(bytes)
                        result.success(null)
                    } catch (error: Exception) {
                        result.error("DOWNLOAD_WRITE_FAILED", error.message, null)
                    }
                    "downloadFinish" -> try {
                        val token = call.argument<String>("token")
                            ?: throw IllegalArgumentException("missing token")
                        val download = downloads[token]
                            ?: throw IllegalStateException("download is not open")
                        download.output.flush()
                        download.output.close()
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && download.uri != null) {
                            val values = ContentValues().apply {
                                put(MediaStore.MediaColumns.IS_PENDING, 0)
                            }
                            check(
                                contentResolver.update(
                                    download.uri,
                                    values,
                                    null,
                                    null,
                                ) > 0,
                            ) { "could not publish Downloads entry" }
                        }
                        downloads.remove(token)
                        result.success(download.location)
                    } catch (error: Exception) {
                        result.error("DOWNLOAD_FINISH_FAILED", error.message, null)
                    }
                    "downloadAbort" -> try {
                        val token = call.argument<String>("token")
                            ?: throw IllegalArgumentException("missing token")
                        deleteDownload(token)
                        result.success(null)
                    } catch (error: Exception) {
                        result.error("DOWNLOAD_ABORT_FAILED", error.message, null)
                    }
                    else -> result.notImplemented()
                }
            }
    }

    @Deprecated("Use the Activity Result API when FlutterActivity supports registration here")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val isFolder = when (requestCode) {
            UPLOAD_FOLDER_PICKER_REQUEST -> true
            UPLOAD_FILES_PICKER_REQUEST -> false
            else -> return
        }
        val reply = pendingUploadPickerResult ?: return
        if (resultCode != RESULT_OK ||
            data == null ||
            (isFolder && data.data == null) ||
            (!isFolder && data.data == null && data.clipData == null)
        ) {
            pendingUploadPickerResult = null
            reply.success(null)
            return
        }
        val uris = if (isFolder) {
            listOf(data.data!!)
        } else {
            val clips = data.clipData
            if (clips != null) {
                (0 until clips.itemCount).map { clips.getItemAt(it).uri }
            } else {
                listOf(data.data!!)
            }
        }
        Thread {
            var stagedRoot: File? = null
            try {
                val resultMap = (if (isFolder) {
                    stageUploadFolder(uris.single())
                } else {
                    stageUploadFiles(uris)
                }).also {
                    stagedRoot = it.first
                }
                runOnUiThread {
                    if (pendingUploadPickerResult === reply) {
                        pendingUploadPickerResult = null
                        reply.success(resultMap.second)
                    } else {
                        resultMap.first.deleteRecursively()
                    }
                }
            } catch (error: Exception) {
                stagedRoot?.deleteRecursively()
                runOnUiThread {
                    if (pendingUploadPickerResult === reply) {
                        pendingUploadPickerResult = null
                        reply.error(
                            "UPLOAD_IMPORT_FAILED",
                            error.message,
                            null,
                        )
                    }
                }
            }
        }.start()
    }

    private fun stageUploadFolder(treeUri: Uri): Pair<File, Map<String, Any>> {
        val rootDocumentId = DocumentsContract.getTreeDocumentId(treeUri)
        val rootUri = DocumentsContract.buildDocumentUriUsingTree(
            treeUri,
            rootDocumentId,
        )
        val rootName = queryDocumentName(rootUri)
        validateUploadSegment(rootName)

        val files = mutableListOf<UploadDocument>()
        val directories = mutableListOf<String>()
        val visitedDirectories = mutableSetOf(rootDocumentId)
        val seenPaths = mutableSetOf<String>()
        collectUploadDocuments(
            treeUri = treeUri,
            documentId = rootDocumentId,
            parentPath = "",
            depth = 0,
            files = files,
            directories = directories,
            visitedDirectories = visitedDirectories,
            seenPaths = seenPaths,
        )
        return stageUploadDocuments(rootName, files, directories)
    }

    private fun stageUploadFiles(uris: List<Uri>): Pair<File, Map<String, Any>> {
        check(uris.isNotEmpty() && uris.size <= MAX_UPLOAD_FILES) {
            "select between 1 and $MAX_UPLOAD_FILES files"
        }
        val files = mutableListOf<UploadDocument>()
        val seenPaths = mutableSetOf<String>()
        for (uri in uris) {
            val (name, size) = queryOpenableFile(uri)
            validateUploadSegment(name)
            check(seenPaths.add(name)) {
                "duplicate file name selected: $name"
            }
            files.add(UploadDocument(uri, name, size))
        }
        return stageUploadDocuments(null, files, emptyList())
    }

    private fun stageUploadDocuments(
        rootName: String?,
        files: List<UploadDocument>,
        directories: List<String>,
    ): Pair<File, Map<String, Any>> {
        val knownBytes = files.fold(0L) { total, file ->
            if (file.reportedSize < 0) {
                total
            } else {
                check(total <= Long.MAX_VALUE - file.reportedSize) {
                    "selected folder size is too large"
                }
                total + file.reportedSize
            }
        }
        val stagingParent = File(cacheDir, "upload_folders")
        check(stagingParent.exists() || stagingParent.mkdirs()) {
            "could not create upload cache"
        }
        val availableBytes = StatFs(stagingParent.absolutePath).availableBytes
        check(knownBytes <= maxOf(0L, availableBytes - UPLOAD_RESERVE_BYTES)) {
            "not enough free app storage to stage this folder"
        }

        val token = UUID.randomUUID().toString()
        val stagingRoot = File(stagingParent, token)
        check(stagingRoot.mkdir()) { "could not create upload staging folder" }
        val stagedFiles = mutableListOf<Map<String, Any>>()
        try {
            for (entry in files) {
                val target = File(stagingRoot, entry.relativePath).canonicalFile
                check(target.path.startsWith(stagingRoot.canonicalPath + File.separator)) {
                    "selected folder contains an invalid path"
                }
                val parent = target.parentFile
                    ?: throw IllegalStateException("invalid staged file path")
                check(parent.exists() || parent.mkdirs()) {
                    "could not create staged subfolder"
                }
                check(target.createNewFile()) {
                    "duplicate file path in selected folder: ${entry.relativePath}"
                }

                var copiedBytes = 0L
                val input = contentResolver.openInputStream(entry.sourceUri)
                    ?: throw IllegalStateException(
                    "could not read ${entry.relativePath}",
                )
                input.use { source ->
                    FileOutputStream(target).use { output ->
                        val buffer = ByteArray(32 * 1024)
                        while (true) {
                            val count = source.read(buffer)
                            if (count < 0) break
                            copiedBytes += count
                            check(copiedBytes <= 0x7fffffffL) {
                                "file exceeds the 2 GiB upload limit: ${entry.relativePath}"
                            }
                            val remaining = StatFs(stagingParent.absolutePath).availableBytes
                            check(remaining >= count + UPLOAD_RESERVE_BYTES) {
                                "app storage filled while staging the selected folder"
                            }
                            output.write(buffer, 0, count)
                        }
                        output.fd.sync()
                    }
                }
                check(entry.reportedSize < 0 || copiedBytes == entry.reportedSize) {
                    "file changed while staging: ${entry.relativePath}"
                }
                stagedFiles.add(
                    mapOf(
                        "path" to entry.relativePath,
                        "localPath" to target.absolutePath,
                        "length" to copiedBytes,
                    ),
                )
            }
            val result = mutableMapOf<String, Any>(
                "token" to token,
                "files" to stagedFiles,
                "directories" to directories,
            )
            if (rootName != null) result["rootName"] = rootName
            return stagingRoot to result
        } catch (error: Exception) {
            if (stagingRoot.exists() && !stagingRoot.deleteRecursively()) {
                throw IllegalStateException(
                    "${error.message}; could not clean partial upload cache",
                    error,
                )
            }
            throw error
        }
    }

    private fun collectUploadDocuments(
        treeUri: Uri,
        documentId: String,
        parentPath: String,
        depth: Int,
        files: MutableList<UploadDocument>,
        directories: MutableList<String>,
        visitedDirectories: MutableSet<String>,
        seenPaths: MutableSet<String>,
    ) {
        check(depth <= MAX_UPLOAD_DEPTH) {
            "selected folder nesting exceeds $MAX_UPLOAD_DEPTH levels"
        }
        val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
            treeUri,
            documentId,
        )
        val projection = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
        )
        val cursor = contentResolver.query(childrenUri, projection, null, null, null)
            ?: throw IllegalStateException("could not list selected folder")
        cursor.use {
            val idColumn = it.getColumnIndexOrThrow(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            )
            val nameColumn = it.getColumnIndexOrThrow(
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            )
            val mimeColumn = it.getColumnIndexOrThrow(
                DocumentsContract.Document.COLUMN_MIME_TYPE,
            )
            val sizeColumn = it.getColumnIndex(
                DocumentsContract.Document.COLUMN_SIZE,
            )
            while (it.moveToNext()) {
                val name = it.getString(nameColumn)
                    ?: throw IllegalStateException("folder item has no name")
                validateUploadSegment(name)
                val relativePath = if (parentPath.isEmpty()) {
                    name
                } else {
                    "$parentPath/$name"
                }
                check(seenPaths.add(relativePath)) {
                    "duplicate file or folder name: $relativePath"
                }
                val childId = it.getString(idColumn)
                    ?: throw IllegalStateException("folder item has no document id")
                val mimeType = it.getString(mimeColumn)
                if (mimeType == DocumentsContract.Document.MIME_TYPE_DIR) {
                    check(directories.size < MAX_UPLOAD_DIRECTORIES) {
                        "selected folder contains too many subfolders"
                    }
                    check(visitedDirectories.add(childId)) {
                        "selected folder contains a directory cycle"
                    }
                    directories.add(relativePath)
                    collectUploadDocuments(
                        treeUri,
                        childId,
                        relativePath,
                        depth + 1,
                        files,
                        directories,
                        visitedDirectories,
                        seenPaths,
                    )
                } else {
                    check(files.size < MAX_UPLOAD_FILES) {
                        "selected folder contains more than $MAX_UPLOAD_FILES files"
                    }
                    files.add(
                        UploadDocument(
                            DocumentsContract.buildDocumentUriUsingTree(
                                treeUri,
                                childId,
                            ),
                            relativePath,
                            if (sizeColumn >= 0 && !it.isNull(sizeColumn)) {
                                it.getLong(sizeColumn)
                            } else {
                                -1
                            },
                        ),
                    )
                }
            }
        }
    }

    private fun queryDocumentName(uri: Uri): String {
        val cursor: Cursor = contentResolver.query(
            uri,
            arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME),
            null,
            null,
            null,
        ) ?: throw IllegalStateException("could not read selected folder name")
        cursor.use {
            check(it.moveToFirst()) { "selected folder has no name" }
            return it.getString(0)
                ?: throw IllegalStateException("selected folder has no name")
        }
    }

    private fun queryOpenableFile(uri: Uri): Pair<String, Long> {
        val cursor = contentResolver.query(
            uri,
            arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE),
            null,
            null,
            null,
        ) ?: throw IllegalStateException("could not read selected file metadata")
        cursor.use {
            check(it.moveToFirst()) { "selected file has no metadata" }
            val nameColumn = it.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            val sizeColumn = it.getColumnIndex(OpenableColumns.SIZE)
            val name = if (nameColumn >= 0) it.getString(nameColumn) else null
            check(!name.isNullOrBlank()) { "selected file has no name" }
            val size = if (sizeColumn >= 0 && !it.isNull(sizeColumn)) {
                it.getLong(sizeColumn)
            } else {
                -1
            }
            return name to size
        }
    }

    private fun validateUploadSegment(name: String) {
        require(
            name.isNotBlank() &&
                name != "." &&
                name != ".." &&
                name.length <= 128 &&
                name.none { it == '/' || it == '\\' || it == ':' || it == '"' || it.code < 32 || it.code == 127 },
        ) {
            "unsupported file or folder name: $name"
        }
    }

    private fun safeFileName(input: String?): String {
        val name = input?.substringAfterLast('/')?.substringAfterLast('\\')
            ?.replace(Regex("[^A-Za-z0-9._-]"), "_")
            ?.trim('.')
            .orEmpty()
        require(name.isNotEmpty()) { "invalid file name" }
        return name
    }

    private fun createDownload(name: String, mimeType: String): DownloadOutput {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val values = ContentValues().apply {
                put(MediaStore.MediaColumns.DISPLAY_NAME, name)
                put(MediaStore.MediaColumns.MIME_TYPE, mimeType)
                put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
                put(MediaStore.MediaColumns.IS_PENDING, 1)
            }
            val uri = contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                ?: throw IllegalStateException("could not create Downloads entry")
            try {
                val output = contentResolver.openOutputStream(uri, "w")
                    ?: throw IllegalStateException("could not open Downloads entry")
                return DownloadOutput(uri, null, output, "${Environment.DIRECTORY_DOWNLOADS}/$name")
            } catch (error: Exception) {
                contentResolver.delete(uri, null, null)
                throw error
            }
        }

        val directory = Environment.getExternalStoragePublicDirectory(
            Environment.DIRECTORY_DOWNLOADS,
        )
        check(directory.exists() || directory.mkdirs()) { "could not create Downloads directory" }
        var file = File(directory, name)
        var suffix = 1
        while (file.exists()) {
            val base = name.substringBeforeLast('.', name)
            val extension = name.substringAfterLast('.', "").let {
                if (it.isEmpty() || it == name) "" else ".$it"
            }
            file = File(directory, "$base ($suffix)$extension")
            suffix++
        }
        return DownloadOutput(null, file, FileOutputStream(file), file.absolutePath)
    }

    private fun deleteDownload(token: String) {
        val download = downloads.remove(token) ?: return
        try {
            download.output.close()
        } finally {
            if (download.uri != null) {
                contentResolver.delete(download.uri, null, null)
            } else {
                download.file?.delete()
            }
        }
    }
}
