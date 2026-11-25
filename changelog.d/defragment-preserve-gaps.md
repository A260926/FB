### Added

- `mp4.DefragmentPreserveGaps` and the `-preserve-gaps` option of `mp4ff-defragment` keep recording
  gaps blank via synthesized empty edits instead of extending the previous sample duration, with
  decode islands built from the final surviving samples' original DTS and durations
