#!/bin/bash
set -euo pipefail
mkdir -p .task-build
c++ -std=c++11 -DWEBVTT_STATIC=1 -Iinclude -Isrc/libwebvtt -Itest/gtest -Isrc \
  test/gtest/gtest-all.cc test/gtest/gtest_main.cc \
  src/libwebvttxx/abstract_parser.cpp src/libwebvttxx/file_parser.cpp \
  test/unit/csline_unittest.cpp test/unit/csvertical_unittest.cpp \
  .task-build/libwebvtt.a -pthread -lm -o .task-build/old-settings
TEST_FILE_DIR=test/unit .task-build/old-settings --gtest_filter='CueSettingLine.*:CueSettingVertical.*'
