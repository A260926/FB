/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2022 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "structured.h"
#include "type2.h"

namespace {

bool readFile(const char *filename, std::string &out)
{
    std::ifstream f(filename, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "failed to open file:" << filename << "\n";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool parseUnsigned(const char *text, unsigned &out)
{
    if (!text || !*text)
        return false;
    unsigned value = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        const unsigned digit = static_cast<unsigned>(*p - '0');
        if (value > (0xFFFFFFFFu - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool parseRequired(const char *text, std::vector<unsigned> &out)
{
    out.clear();
    if (!text || !*text)
        return false;
    if (std::strcmp(text, "-") == 0)
        return true;

    std::string spec(text);
    size_t pos = 0;
    while (pos <= spec.size()) {
        const size_t comma = spec.find(',', pos);
        const std::string token = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (token.empty())
            return false;
        unsigned id = 0;
        if (!parseUnsigned(token.c_str(), id))
            return false;
        out.push_back(id);
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    return true;
}

bool parseBudget(const char *text, size_t &out)
{
    if (!text || !*text)
        return false;
    size_t value = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        const size_t digit = static_cast<size_t>(*p - '0');
        if (value > (static_cast<size_t>(-1) - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

/* Write the finished reduction to a fresh file and rename it into place so a
 * failure never leaves half an output file behind. */
bool writeFileAtomically(const char *target, const std::string &content)
{
    std::string path(target);
    std::string dir = ".";
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos)
        dir = path.substr(0, slash == 0 ? 1 : slash);

    std::string pattern = dir + "/.structured-out-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0) {
        std::cerr << "failed to create output file near " << target << ": " << std::strerror(errno) << "\n";
        return false;
    }

    const char *p = content.data();
    size_t left = content.size();
    while (left) {
        const ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            std::cerr << "failed to write output file: " << std::strerror(errno) << "\n";
            close(fd);
            unlink(name.data());
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    if (close(fd) != 0) {
        std::cerr << "failed to close output file: " << std::strerror(errno) << "\n";
        unlink(name.data());
        return false;
    }
    if (rename(name.data(), target) != 0) {
        std::cerr << "failed to move output file into place: " << std::strerror(errno) << "\n";
        unlink(name.data());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 7 && std::strcmp(argv[1], "--reduce") == 0) {
        std::vector<unsigned> required;
        size_t budget = 0;
        if (!parseRequired(argv[3], required)) {
            std::cerr << "invalid REQUIRED list\n";
            return 1;
        }
        if (!parseBudget(argv[4], budget)) {
            std::cerr << "invalid BUDGET\n";
            return 1;
        }

        std::string input;
        if (!readFile(argv[5], input))
            return 1;

        std::string reduced;
        try {
            reduced = reduceCode2(reinterpret_cast<const uint8_t *>(input.data()),
                                  input.size(), argv[2], required, budget);
        } catch (const std::exception &e) {
            std::cerr << e.what() << "\n";
            return 1;
        }

        if (!writeFileAtomically(argv[6], reduced))
            return 1;
        return 0;
    }

    if (argc == 2) {
        std::string input;
        if (!readFile(argv[1], input))
            return 1;
        std::cout << generateCode2(reinterpret_cast<const uint8_t *>(input.data()), input.size()) << std::endl;
        return 0;
    }

    std::cout << "Invalid args, no filename\n";
    return 1;
}
