set -eu
cd "$(dirname "$0")/.."
cmake -S . -B build-checks -DCMAKE_BUILD_TYPE=Release -DFOONATHAN_MEMORY_ASAN=OFF -DFOONATHAN_MEMORY_BUILD_TESTS=OFF -DFOONATHAN_MEMORY_BUILD_EXAMPLES=OFF -DFOONATHAN_MEMORY_BUILD_TOOLS=OFF -DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer'
cmake --build build-checks --target foonathan_memory -j2
