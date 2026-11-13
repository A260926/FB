set -eu
cd "$(dirname "$0")/.."
cmake -S . -B build-checks -DCMAKE_BUILD_TYPE=Release -DMI_BUILD_TESTS=OFF -DMI_BUILD_SHARED=OFF -DMI_BUILD_OBJECT=OFF -DMI_OVERRIDE=OFF -DMI_OSX_INTERPOSE=OFF -DMI_OSX_ZONE=OFF -DMI_GUARDED=ON -DMI_USE_CXX=OFF -DMI_OPT_ARCH=OFF
cmake --build build-checks --target mimalloc-static -j2
