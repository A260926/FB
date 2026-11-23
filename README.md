# version
v0.8
Matthew Szatmary m3u8@twitch.tv / matt@szatmary.org

# libcaption

libcaption is a library written in C to aid in the creating and parsing of closed caption data, open sourced under the MIT license to use within community developed broadcast tools. To maintain consistency across platforms libcaption aims to implement a subset of EIA608, CEA708 as supported by the Apple iOS platform.

608 support is currently limited to encoding and decoding the necessary control and preamble codes as well as support for the Basic North American, Special North American and Extended Western European character sets.

708 support is limited to encoding the 608 data in NTSC field 1 user data type structure.

In addition, utility functions to create h.264 SEI (Supplementary enhancement information) NALUs (Network Abstraction Layer Unit) for inclusion into an h.264 elementary stream are provided.

H.264 utility functions are limited to wrapping the 708 payload into a SEI NALU. This is accomplished by prepending the 708 payload with 3 bytes (nal_unit_type = 6, payloadType = 4, and PayloadSize = variable), and appending a stop bit encoded into a full byte (with a value of 127). In addition if the 708 payload contains an emulated start code, a three byte sequence equaling 0,0,1 an emulation prevention byte (3) is inserted. Functions to reverse this operation are also provided.

## Characters
| | | | | | | | | | | | | | | | | |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
|BNA| |!|"|#|$|%|&|’|(|)|á|+|,|-|.|/|
|BNA|0|1|2|3|4|5|6|7|8|9|:|;|<|=|>|?|
|BNA|@|A|B|C|D|E|F|G|H|I|J|K|L|M|N|O|
|BNA|P|Q|R|S|T|U|V|W|X|Y|Z|[|é|]|í|ó|
|BNA|ú|a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|
|BNA|p|q|r|s|t|u|v|w|x|y|z|ç|÷|Ñ|ñ|█|
|SNA|®|°|½|¿|™|¢|£|♪|à|&nbsp;|è|â|ê|î|ô|û|
|WES|Á|É|Ó|Ú|Ü|ü|‘|¡|*|'|—|©|℠|•|“|”|
|WEF|À|Â|Ç|È|Ê|Ë|ë|Î|Ï|ï|Ô|Ù|ù|Û|«|»|
|WEP|Ã|ã|Í|Ì|ì|Ò|ò|Õ|õ|{|}|&#92;|^|_|&#124;|~|
|WEG|Ä|ä|Ö|ö|ß|¥|¤|¦|Å|å|Ø|ø|┌|┐|└|┘|

* BNA = Basic North American character set
* SNA = Special North American character set
* WES = Extended Western European character set : Extended Spanish/Miscellaneous
* WEF = Extended Western European character set : Extended French
* WEP = Extended Western European character set : Portuguese
* WEG = Extended Western European character set : German/Danish

## Limitations
Current B-frame support for caption creation is minimal. libcaption ensures no re-ordering of captions is required
on playback.

## Build Directions
# Mac Os/Linux
Install build dependencies (git, cmake, a compiler such as xcode, gcc or clang and optionally re2c and ffmpeg)
* run `cmake . && make`
* or to compile without re2c `cmake -DENABLE_RE2C=OFF . && make`
* finally `sudo make install` to install
# Windows
I have never tested libcaption in windows. It is written in pure C with no dependencies,
so there is no reason it would not work.

## CC1 pop-on playout (`caption/playout.h`, `caption/diff608.h`)
`cc_playout_t` is a stateful, single-channel (CC1) pop-on EIA-608 scheduler for the
one-word-per-transmission-frame ("608") channel. It runs entirely from frame numbers and
the words it has already emitted: there is no wall clock, threading, network or third-party
dependency.

### Ownership
* Create with `cc_playout_create(start_frame)` and release with `cc_playout_free()`. The
  modeled receiver starts with both displayed and hidden memories empty.
* `cc_playout_set()` deep copies the supplied `caption_frame_t.front` snapshot and the event
  id. The caller may reuse or free its frame immediately; two playouts never share state.
* Records returned by `cc_playout_pull()` are plain values (`cc_playout_record_t`: an
  absolute frame number and one odd-parity 16-bit word). Idle frames carry the padding word
  `0x8080`; every word always has correct odd parity.

### Screen model and frame budget
Each input row must be empty or hold one run of `A-Z`/`0-9` beginning at column 0; every
other cell is blank, white and not underlined. Empty screens are valid. Anything else is
rejected. For one event the preload is: RCL twice, then per changed row (ascending row order)
a column-0 PAC twice, DER twice, then the text as paired characters (an odd final character
is paired with a 608 null byte, never a visible space). Unchanged rows are not rewritten and
no whole-screen erase is used. The first EOC is emitted exactly on the event's display frame
and the duplicate EOC on the next frame; preload starts as early as possible and padding
fills any gap before the timed EOC. An event is feasible only when its whole preload fits in
the frames available before its first EOC, after the previous event's second EOC. The maximum
single preload is bounded by `CC_PLAYOUT_MAX_PLAN_WORDS`.

### Scheduling and editing
`set()` adds or replaces an event by string id and `cancel()` removes one; two ids may not
share a display frame. Every change replays the entire uncommitted queue against the real
on-the-wire receiver state (including later events) before it is accepted. If any event
cannot keep its exact display frame, the call fails and the transmitted prefix, queue and
current position are left unchanged — deadlines are never silently moved. An event whose
first EOC has been sent is committed: further `set()`/`cancel()` calls on that id fail, while
its duplicate EOC is still transmitted. Before that point an event may be edited or cancelled
even after part of its preload is on the wire: the receiver is never reset, the visible
screen is never cleared, the frame position never moves backwards, and only the unsent
suffix is re-planned from the residual hidden memory (unchanged rows keep being reused).
Re-displaying a screen still held in hidden memory therefore emits only RCL and the EOC pair.

### Error codes
`CC_PLAYOUT_OK` on success; otherwise `CC_PLAYOUT_INVALID` (bad argument or unsupported
screen), `CC_PLAYOUT_NOT_FOUND` (cancel of an unknown id), `CC_PLAYOUT_CONFLICT` (duplicate
display frame), `CC_PLAYOUT_LATE` (the queue cannot be preloaded in time),
`CC_PLAYOUT_COMMITTED` (first EOC already sent) or `CC_PLAYOUT_OVERFLOW` (frame/capacity
arithmetic would overflow). The existing whole-screen SEI export
(`sei_from_caption_frame`) is unchanged and still reproduces its original picture; the
decoder EOC now swaps the two memories and retains the old displayed memory as the new
hidden memory, and DER truly clears the rest of the current write row.

### Example and tests
`examples/caption_playout.c` takes an edit script and an output path. Script lines are
`SET id frame TEXT` (`-` for an empty screen; TEXT goes in the last row), `CANCEL id` or
`PULL count`; each `PULL` writes `frame HEXWORD` lines. A malformed or rejected command exits
nonzero and removes the partially written output. Unit tests live in
`unit_tests/test_playout.c` (groups `screens`, `deadlines`, `editing`).
