These files split the main library into separate .h and .c files. This is intended for those who prefer separate files
or whose build environment better suits this configuration. The files here are generated from a tool based on the
content in the main miniaudio.h file. Do not edit these files directly. If you want to contribute, please make the
contribution in the main file.

This is not always up to date with the most recent commit in the dev branch, but will usually be up to date with the
master branch.

## Regenerating

The split files are produced by the `camal/split.camal` build script, which slices the amalgamated `miniaudio.h` by
marker rather than by fixed line numbers, so it stays correct as the main header changes. The exact rules, applied to
the main header, are:

- The leading file comment block is reused verbatim as the header of both files.
- `miniaudio.h` is the content from the `#ifndef miniaudio_h` line through its matching
  `#endif  /* miniaudio_h */` line.
- `miniaudio.c` starts with `#include "miniaudio.h"` followed by the content from the `#ifndef miniaudio_c` line
  through its matching `#endif  /* miniaudio_c */` line.
- The license footer, from the `/*` block comment beginning `This software` to the end of the file, is appended to
  both files.

After editing `miniaudio.h`, rerun the `camal/split.camal` generation (or apply the same marker-based slicing) and
replace both `extras/miniaudio_split/miniaudio.h` and `extras/miniaudio_split/miniaudio.c` together.
