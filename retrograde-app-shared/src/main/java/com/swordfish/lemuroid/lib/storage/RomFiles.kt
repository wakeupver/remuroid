package com.swordfish.lemuroid.lib.storage

import java.io.File

sealed class RomFiles {
    data class Standard(val files: List<File>) : RomFiles()
}
