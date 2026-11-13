#!/bin/sh
set -eu
mkdir -p .support-build
test -s testing/decode_order_test.cc
c++ -std=c++11 -O1 -I. testing/decode_order_test.cc mkvmuxer/mkvmuxer.cc mkvmuxer/mkvmuxerutil.cc mkvmuxer/mkvwriter.cc mkvparser/mkvparser.cc mkvparser/mkvreader.cc -o .support-build/decode-order
.support-build/decode-order
bash support/baseline.sh
