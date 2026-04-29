package com.swordfish.lemuroid.lib.bios

import com.swordfish.lemuroid.common.files.safeDelete
import com.swordfish.lemuroid.common.kotlin.associateByNotNull
import com.swordfish.lemuroid.common.kotlin.writeToFile
import com.swordfish.lemuroid.lib.library.SystemCoreConfig
import com.swordfish.lemuroid.lib.library.SystemID
import com.swordfish.lemuroid.lib.library.db.entity.Game
import com.swordfish.lemuroid.lib.storage.DirectoriesManager
import com.swordfish.lemuroid.lib.storage.StorageFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import timber.log.Timber
import java.io.File
import java.io.InputStream

class BiosManager(private val directoriesManager: DirectoriesManager) {
    private val crcLookup = SUPPORTED_BIOS.associateByNotNull { it.externalCRC32 }
    private val nameLookup = SUPPORTED_BIOS.associateByNotNull { it.externalName }

    fun getMissingBiosFiles(
        coreConfig: SystemCoreConfig,
        game: Game,
    ): List<String> {
        val regionalBiosFiles = coreConfig.regionalBIOSFiles

        val gameLabels =
            Regex("\\([A-Za-z]+\\)")
                .findAll(game.title)
                .map { it.value.drop(1).dropLast(1) }
                .filter { it.isNotBlank() }
                .toSet()

        Timber.d("Found game labels: $gameLabels")

        val requiredRegionalFiles =
            gameLabels.intersect(regionalBiosFiles.keys)
                .ifEmpty { regionalBiosFiles.keys }
                .mapNotNull { regionalBiosFiles[it] }

        Timber.d("Required regional files for game: $requiredRegionalFiles")

        return (coreConfig.requiredBIOSFiles + requiredRegionalFiles)
            .filter { !File(directoriesManager.getSystemDirectory(), it).exists() }
    }

    fun deleteBiosBefore(timestampMs: Long) {
        Timber.i("Pruning old bios files")
        SUPPORTED_BIOS
            .map { File(directoriesManager.getSystemDirectory(), it.libretroFileName) }
            .filter { it.lastModified() < normalizeTimestamp(timestampMs) }
            .forEach {
                Timber.d("Pruning old bios file: ${it.path}")
                it.safeDelete()
            }
    }

    @Deprecated("Use the suspend variant")
    fun getBiosInfo(): BiosInfo {
        val bios =
            SUPPORTED_BIOS.groupBy {
                File(directoriesManager.getSystemDirectory(), it.libretroFileName).exists()
            }.withDefault { listOf() }

        return BiosInfo(bios.getValue(true), bios.getValue(false))
    }

    suspend fun getBiosInfoAsync(): BiosInfo =
        withContext(Dispatchers.IO) {
            getBiosInfo()
        }

    fun tryAddBiosAfter(
        storageFile: StorageFile,
        inputStream: InputStream,
        timestampMs: Long,
    ): Boolean {
        val bios = findByCRC(storageFile) ?: findByName(storageFile) ?: return false

        Timber.i("Importing bios file: $bios")

        val biosFile = File(directoriesManager.getSystemDirectory(), bios.libretroFileName)
        if (biosFile.exists() && biosFile.setLastModified(normalizeTimestamp(timestampMs))) {
            Timber.d("Bios file already present. Updated last modification date.")
        } else {
            Timber.d("Bios file not available. Copying new file.")
            biosFile.parentFile?.mkdirs()
            inputStream.writeToFile(biosFile)
        }
        return true
    }

    private fun findByCRC(storageFile: StorageFile): Bios? {
        return crcLookup[storageFile.crc]
    }

    private fun findByName(storageFile: StorageFile): Bios? {
        return nameLookup[storageFile.name]
    }

    private fun normalizeTimestamp(timestamp: Long) = (timestamp / 1000) * 1000

    data class BiosInfo(val detected: List<Bios>, val notDetected: List<Bios>)

    companion object {
        private val SUPPORTED_BIOS =
            listOf(
                Bios(
                    "lynxboot.img",
                    "FCD403DB69F54290B51035D82F835E7B",
                    "Lynx Boot Image",
                    SystemID.LYNX,
                    "0D973C9D",
                ),
                Bios(
                    "bios_CD_E.bin",
                    "E66FA1DC5820D254611FDCDBA0662372",
                    "Sega CD E",
                    SystemID.SEGACD,
                    "529AC15A",
                ),
                Bios(
                    "bios_CD_J.bin",
                    "278A9397D192149E84E820AC621A8EDD",
                    "Sega CD J",
                    SystemID.SEGACD,
                    "9D2DA8F2",
                ),
                Bios(
                    "bios_CD_U.bin",
                    "2EFD74E3232FF260E371B99F84024F7F",
                    "Sega CD U",
                    SystemID.SEGACD,
                    "C6D10268",
                ),
                Bios(
                    "bios7.bin",
                    "DF692A80A5B1BC90728BC3DFC76CD948",
                    "Nintendo DS ARM7",
                    SystemID.NDS,
                    "1280F0D5",
                ),
                Bios(
                    "bios9.bin",
                    "A392174EB3E572FED6447E956BDE4B25",
                    "Nintendo DS ARM9",
                    SystemID.NDS,
                    "2AB23573",
                ),
                Bios(
                    "firmware.bin",
                    "E45033D9B0FA6B0DE071292BBA7C9D13",
                    "Nintendo DS Firmware",
                    SystemID.NDS,
                    "945F9DC9",
                    "nds_firmware.bin",
                ),
                Bios(
                    "dc/dc_boot.bin",
                    "E10C53C2F8B90BEB96A37FFEBC11B734",
                    "Dreamcast BIOS",
                    SystemID.DREAMCAST,
                    "89F2B1A1",
                    "dc_boot.bin",
                ),
                Bios(
                    "dc/dc_flash.bin",
                    "0A93F7940C455905BEA0CF1AC83E5344",
                    "Dreamcast Flash (US)",
                    SystemID.DREAMCAST,
                    "B7E5ACED",
                    "dc_flash.bin",
                ),
                // PlayStation BIOS files used by SwanStation.
                // openbios.bin has no fixed CRC (any build is accepted).
                Bios(
                    "openbios.bin",
                    "",   // No fixed MD5 — any build of OpenBIOS is accepted
                    "PlayStation OpenBIOS (All regions)",
                    SystemID.PSX,
                    null, // No fixed CRC32
                    "openbios.bin",
                ),
                Bios(
                    "scph5500.bin",
                    "8DD7D5296A650FAC7319BCE665A6A53C",
                    "PlayStation BIOS v3.0 NTSC-J",
                    SystemID.PSX,
                    "FF3EEB8C",
                    "scph5500.bin",
                ),
                Bios(
                    "scph5501.bin",
                    "490F666E1AFB15B7362B406ED1CEA246",
                    "PlayStation BIOS v3.0 NTSC-U",
                    SystemID.PSX,
                    "8D8CB7E4",
                    "scph5501.bin",
                ),
                Bios(
                    "scph5502.bin",
                    "32736F17079D0B2B7024407C39BD3050",
                    "PlayStation BIOS v3.0 PAL",
                    SystemID.PSX,
                    "D786F0B9",
                    "scph5502.bin",
                ),
            )
    }
}
