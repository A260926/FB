#!/usr/bin/env bash
set -euo pipefail
export LIBWEBM_TEST_DATA_PATH="$PWD/testing/testdata"
# Exact test selection and actual run-count guards; no wildcard-only PASS check.
mux='MuxerTest.AddTracks:MuxerTest.SimpleBlock:MuxerTest.SimpleBlockWithAddGenericFrame:MuxerTest.BlockWithAdditional:MuxerTest.BlockAdditionalWithAddGenericFrame:MuxerTest.SegmentDurationComputation:MuxerTest.ForceNewCluster:MuxerTest.OutputCues:MuxerTest.CuesBeforeClusters:MuxerTest.CuesWithForceCluster:MuxerTest.MaxClusterSize:MuxerTest.MaxClusterDuration:MuxerTest.SetCuesTrackNumber:MuxerTest.BlockWithDiscardPadding:MuxerTest.AccurateClusterDuration:MuxerTest.AccurateClusterDurationTwoTracks'
parser='ParserTest.TrackEntries:ParserTest.SimpleBlock:ParserTest.MultipleClusters:ParserTest.BlockGroup:ParserTest.Cues:ParserTest.CuesBeforeClusters:ParserTest.CuesTrackNumber:ParserTest.Opus:ParserTest.DiscardPadding:ParserTest.InvalidFixedLacingSize'
mkdir -p build-userfb
build-userfb/mkvmuxer_tests --gtest_filter="$mux" --gtest_color=no | tee build-userfb/old-muxer.log
grep -Fxq '[  PASSED  ] 16 tests.' build-userfb/old-muxer.log
build-userfb/mkvparser_tests --gtest_filter="$parser" --gtest_color=no | tee build-userfb/old-parser.log
grep -Fxq '[  PASSED  ] 10 tests.' build-userfb/old-parser.log
printf 'OLD_TESTS muxer=16 parser=10 passed\n'
