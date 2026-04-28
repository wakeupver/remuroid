package com.swordfish.lemuroid.lib.storage.local

import android.content.Context
import com.swordfish.lemuroid.common.kotlin.toStringCRC32
import com.swordfish.lemuroid.lib.library.GameSystem
import com.swordfish.lemuroid.lib.storage.BaseStorageFile
import com.swordfish.lemuroid.lib.storage.StorageFile
import com.swordfish.lemuroid.lib.storage.scanner.SerialScanner
import timber.log.Timber
import java.io.FilterInputStream
import java.io.InputStream
import java.util.zip.CRC32
import java.util.zip.CheckedInputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream

object DocumentFileParser {
    private const val MAX_CHECKED_ENTRIES = 3
    private const val SINGLE_ARCHIVE_THRESHOLD = 0.9

    // Only compute CRC32 for files ≤ 64 MB. Larger files (PS1/N64/Dreamcast ISOs) are
    // matched via serial number, unique extension, or folder name — not CRC.
    private const val MAX_SIZE_CRC32 = 64 * 1024 * 1024L

    // Large read buffer for fast CRC computation (512 KB beats the old 16 KB default).
    private const val CRC_BUFFER_SIZE = 512 * 1024

    fun parseDocumentFile(
        context: Context,
        baseStorageFile: BaseStorageFile,
    ): StorageFile {
        return if (baseStorageFile.extension == "zip") {
            Timber.d("Detected zip file. ${baseStorageFile.name}")
            parseZipFile(context, baseStorageFile)
        } else {
            Timber.d("Detected standard file. ${baseStorageFile.name}")
            parseStandardFile(context, baseStorageFile)
        }
    }

    private fun parseZipFile(
        context: Context,
        baseStorageFile: BaseStorageFile,
    ): StorageFile {
        val inputStream = context.contentResolver.openInputStream(baseStorageFile.uri)
        return ZipInputStream(inputStream).use {
            val gameEntry = findGameEntry(it, baseStorageFile.size)
            if (gameEntry != null) {
                Timber.d("Handing zip file as compressed game: ${baseStorageFile.name}")
                parseCompressedGame(baseStorageFile, gameEntry, it)
            } else {
                Timber.d("Handing zip file as standard: ${baseStorageFile.name}")
                parseStandardFile(context, baseStorageFile)
            }
        }
    }

    private fun parseCompressedGame(
        baseStorageFile: BaseStorageFile,
        entry: ZipEntry,
        zipInputStream: ZipInputStream,
    ): StorageFile {
        Timber.d("Processing zipped entry: ${entry.name}")

        val diskInfo = SerialScanner.extractInfo(entry.name, zipInputStream)

        // entry.crc is 0 for streaming zips that use data descriptors (the CRC is written
        // *after* the file data). Pass null so metadata lookup skips the CRC query rather
        // than looking up the meaningless value "00000000".
        val crc = if (entry.crc != 0L) entry.crc.toStringCRC32() else null

        return StorageFile(
            entry.name,
            entry.size,
            crc,
            diskInfo.serial,
            baseStorageFile.uri,
            baseStorageFile.uri.path,
            diskInfo.systemID,
        )
    }

    private fun parseStandardFile(
        context: Context,
        baseStorageFile: BaseStorageFile,
    ): StorageFile {
        val shouldComputeCrc = baseStorageFile.size < MAX_SIZE_CRC32

        var diskInfo: SerialScanner.DiskInfo? = null
        var crc32: String? = null

        context.contentResolver.openInputStream(baseStorageFile.uri)?.use { rawStream ->
            if (shouldComputeCrc) {
                // Single-pass: compute CRC while also extracting serial info.
                // We wrap the raw stream in a CRC-accumulating stream, then hand a
                // non-closing view of it to SerialScanner so it doesn't close our
                // underlying stream after reading just the header bytes.
                val crcAccumulator = CRC32()
                val checkedStream = CheckedInputStream(rawStream, crcAccumulator)
                val nonClosing = NonClosingInputStream(checkedStream).buffered(CRC_BUFFER_SIZE)

                diskInfo = SerialScanner.extractInfo(baseStorageFile.name, nonClosing)

                // If a serial was found we can skip the rest of the file; otherwise
                // drain the stream so the CRC covers the complete file content.
                if (diskInfo?.serial == null) {
                    val drainBuf = ByteArray(CRC_BUFFER_SIZE)
                    while (nonClosing.read(drainBuf) != -1) { /* drain */ }
                    crc32 = crcAccumulator.value.toStringCRC32()
                }
            } else {
                // Large file: serial-scan only (no CRC). Match will happen via serial,
                // unique extension, or folder-name heuristics.
                diskInfo = SerialScanner.extractInfo(baseStorageFile.name, rawStream)
            }
        }

        Timber.d("Parsed standard file: $baseStorageFile crc=$crc32 serial=${diskInfo?.serial}")

        return StorageFile(
            baseStorageFile.name,
            baseStorageFile.size,
            crc32,
            diskInfo?.serial,
            baseStorageFile.uri,
            baseStorageFile.uri.path,
            diskInfo?.systemID,
        )
    }

    /**
     * A stream wrapper that ignores [close] calls. Lets us hand a stream to a caller
     * that closes it (SerialScanner) without actually closing the underlying resource.
     */
    private class NonClosingInputStream(wrapped: InputStream) : FilterInputStream(wrapped) {
        override fun close() { /* intentionally empty */ }
    }

    /* Finds a zip entry which we assume is a game. Lemuroid only supports single archive games,
       so we are looking for an entry which occupies a large percentage of the archive space.
       This is very fast heuristic to compute and avoids reading the whole stream in most
       scenarios.
       Fallback: when the compressed size is unknown (value -1, common with streaming zip tools
       that write data descriptors after file data), we accept the entry if its extension matches
       a known game ROM extension. */
    fun findGameEntry(
        openedInputStream: ZipInputStream,
        fileSize: Long = -1,
    ): ZipEntry? {
        for (i in 0..MAX_CHECKED_ENTRIES) {
            val entry = openedInputStream.nextEntry ?: break
            if (!isGameEntry(entry, fileSize)) continue
            return entry
        }
        return null
    }

    private fun isGameEntry(
        entry: ZipEntry,
        fileSize: Long,
    ): Boolean {
        if (entry.isDirectory) return false

        // Primary heuristic: the compressed entry should occupy a large fraction of the zip.
        // This is fast and avoids reading any data.
        if (fileSize > 0 && entry.compressedSize > 0) {
            return (entry.compressedSize.toFloat() / fileSize.toFloat()) > SINGLE_ARCHIVE_THRESHOLD
        }

        // Fallback for streaming zips: compressedSize is -1 when the local file header omits
        // it (data descriptor written after file data, common with many zip tools). In that
        // case we trust the entry's file extension instead.
        val ext = entry.name.substringAfterLast('.', "").lowercase(java.util.Locale.ROOT)
        return ext.isNotEmpty() && ext != "zip" && knownRomExtensions.contains(ext)
    }

    // Lazy set of all game extensions known to Lemuroid, excluding "zip" itself.
    // Used as fallback when a zip entry's compressedSize is unknown (streaming zips).
    private val knownRomExtensions: Set<String> by lazy {
        GameSystem.getSupportedExtensions()
            .filterNot { it == "zip" }
            .toHashSet()
    }
}
