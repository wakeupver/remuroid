package com.swordfish.lemuroid.lib.storage

import java.io.File

sealed class RomFiles {
    /** ROM loaded from a regular file path on disk (multi-file / disc-based games). */
    data class Standard(val files: List<File>) : RomFiles()

    /** ROM loaded entirely into memory from a zip entry. No cache file is written.
     *  Only valid for single-file cartridge-based games whose libretro core does not
     *  require a full filesystem path (need_fullpath = false). */
    data class Bytes(val bytes: ByteArray, val fileName: String) : RomFiles()
}
