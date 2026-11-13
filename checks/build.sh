#!/bin/bash
set -euo pipefail
out=$1; shift
c++ -std=c++17 -O0 -I. imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp "$@" -o "$out"
