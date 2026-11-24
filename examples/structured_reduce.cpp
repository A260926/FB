/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2024 Cppcheck team.
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

// Standalone example: generate a small benign seed, run the structured
// reducer against a text-only coverage fixture and write the reduced C source
// to stdout.  The output is itself valid C source and can be fed to
// `c++ -fsyntax-only`.
//
// Build:
//
//   c++ -std=c++17 -I. examples/structured_reduce.cpp \
//       oss-fuzz/type2.cpp oss-fuzz/structured.cpp -o structured_reduce
//
// Run (no arguments needed):
//
//   ./structured_reduce            # uses a temporary fixture script
//   ./structured_reduce <worker>   # uses the given coverage worker

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "oss-fuzz/structured.h"

namespace {

std::string writeFixture()
{
    const char *dirEnv = std::getenv("TMPDIR");
    std::string dir = (dirEnv && *dirEnv) ? dirEnv : "/tmp";
    if (dir.back() != '/')
        dir += '/';
    std::string pattern = dir + "structured-example-worker-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0)
        return std::string();

    // A benign text-only fixture: report the coverage ids that the reduced
    // seed is expected to keep.
    static const char script[] = "#!/bin/sh\necho \"COVER 1 2 3\"\n";
    const ssize_t ignored = write(fd, script, sizeof(script) - 1);
    (void)ignored;
    fchmod(fd, 0700);
    close(fd);
    return std::string(name.data());
}

} // namespace

int main(int argc, char **argv)
{
    std::string worker;
    bool removeWorker = false;
    if (argc >= 2) {
        worker = argv[1];
    } else {
        worker = writeFixture();
        if (worker.empty()) {
            std::cerr << "could not create a coverage fixture\n";
            return 1;
        }
        removeWorker = true;
    }

    // A fixed benign seed.  generateCode2() turns this into several
    // functions with nested if/while statements that reference var1..var5.
    const std::vector<uint8_t> seed = {0x07, 0x21, 0x80, 0x13, 0x42};
    const std::vector<unsigned> required = {1, 2, 3};

    std::string reduced;
    try {
        reduced = reduceCode2(seed.data(), seed.size(), worker, required, 64);
    } catch (const std::exception &e) {
        std::cerr << "reduceCode2 failed: " << e.what() << "\n";
        if (removeWorker)
            unlink(worker.c_str());
        return 1;
    }

    if (removeWorker)
        unlink(worker.c_str());

    std::cout << reduced;
    return 0;
}
