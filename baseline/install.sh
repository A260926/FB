#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .deps/download .deps/src
fetch() {
  name=$1 url=$2 sha=$3
  archive=".deps/download/$name.tar.gz"
  if [ ! -f "$archive" ]; then curl -fL "$url" -o "$archive"; fi
  printf '%s  %s\n' "$sha" "$archive" | shasum -a 256 -c -
  if [ ! -d ".deps/src/$name" ]; then
    mkdir -p ".deps/src/$name"
    tar -xzf "$archive" -C ".deps/src/$name" --strip-components=1
  fi
}
fetch protobuf https://codeload.github.com/protocolbuffers/protobuf/tar.gz/refs/tags/v3.21.12 930c2c3b5ecc6c9c12615cf5ad93f1cd6e12d0aba862b572e076259970ac3a53
fetch googletest https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.15.2 7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
cmake -S .deps/src/protobuf/cmake -B .deps/protobuf-build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/.deps/install" -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_WITH_ZLIB=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF -DCMAKE_CXX_STANDARD=17
cmake --build .deps/protobuf-build --target install -j4
cmake -S baseline -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$PWD/.deps/install" -DProtobuf_PROTOC_EXECUTABLE="$PWD/.deps/install/bin/protoc"
cmake --build build -j4
