#!/bin/sh
# Build GEL/PyGEL, install the C++ package to ~/.local, then create and
# pip-install the PyGEL3D wheel.
set -eu
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_INSTALL_PREFIX="${HOME}/.local"
cmake --build build -j 12
cmake --install build

rm -fr dist
# Reuse the CMake tree just built; the Python build copies only this
# platform's library into a py3-none-<platform> wheel.
python -m pip install -q build setuptools cmake ninja
PYGEL_SKIP_CMAKE=1 python -m build --wheel --no-isolation
pip uninstall --yes PyGEL3D
pip install dist/*whl
