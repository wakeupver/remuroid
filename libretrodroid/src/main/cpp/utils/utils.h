/*
 *     Copyright (C) 2019  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef LIBRETRODROID_UTILS_H
#define LIBRETRODROID_UTILS_H

#include <string>
#include <vector>

namespace libretrodroid {

class Utils {
public:
    static std::vector<char> readFileAsBytes(const std::string &filePath);
    static std::vector<char> readFileAsBytes(int fileDescriptor);

    static size_t getFileSize(FILE* file);
};

}

#endif //LIBRETRODROID_UTILS_H
