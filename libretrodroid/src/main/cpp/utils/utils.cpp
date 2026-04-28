#include <iostream>
#include <fstream>
#include <unistd.h>

#include "utils.h"
#include "../log.h"

namespace libretrodroid {

std::vector<char> Utils::readFileAsBytes(const std::string &filePath) {
    std::ifstream fileStream(filePath, std::ios::binary);
    if (!fileStream) {
        LOGE("Cannot open file: %s", filePath.c_str());
        return {};
    }
    fileStream.seekg(0, std::ios::end);
    auto size = static_cast<size_t>(fileStream.tellg());
    fileStream.seekg(0, std::ios::beg);
    std::vector<char> bytes(size);
    fileStream.read(bytes.data(), static_cast<std::streamsize>(size));
    return bytes;
}

std::vector<char> Utils::readFileAsBytes(int fileDescriptor) {
    FILE* file = fdopen(fileDescriptor, "r");
    if (!file) {
        LOGE("fdopen failed for fd %d", fileDescriptor);
        ::close(fileDescriptor);
        return {};
    }
    size_t size = getFileSize(file);
    std::vector<char> bytes(size);
    fread(bytes.data(), sizeof(char), size, file);
    fclose(file);
    return bytes;
}

size_t Utils::getFileSize(FILE* file) {
    fseek(file, 0, SEEK_SET);
    fseek(file, 0, SEEK_END);
    size_t size = static_cast<size_t>(ftell(file));
    fseek(file, 0, SEEK_SET);
    return size;
}

} //namespace libretrodroid