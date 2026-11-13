#!/bin/sh
set -eu
mkdir -p .support-build
c++ -std=c++11 -O1 -I. support/baseline.cc mkvmuxer/mkvmuxer.cc mkvmuxer/mkvmuxerutil.cc mkvmuxer/mkvwriter.cc -o .support-build/baseline
.support-build/baseline
