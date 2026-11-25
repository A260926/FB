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

# FLV caption channel splitter
`caption/flv_splitter.h` implements a streaming de-multiplexer that turns one
captioned AVC/FLV stream into four complete FLV streams, one per CEA-608
service (CC1, CC2, CC3, CC4). Audio, script and non-AVC video tags, the AVC
configuration tag, composition times and the original 32 bit timestamps are
forwarded untouched. Only the `cc_data` entries of ATSC/GA94 user data SEI
messages (`user_data_type_code` 3 with `process_cc_data_flag` set) are
filtered; every other SEI message, the GA94 flags, `em_data` and any bytes
following `cc_data` are preserved. `cc_count` and the SEI message length are
recomputed together with the NAL length, the FLV `DataSize` and the trailing
`PreviousTagSize`.

Routing is decided from the original input, never from the four filtered
copies. Each field starts on its first channel; PACs, mid-row changes,
special/extended characters and control codes with a channel bit update the
selection, while basic characters follow the most recent selection. Field 1
maps to CC1/CC2 and field 2 to CC3/CC4, only words with odd parity are routed,
and complete XDS sequences in field 2 are kept on all four outputs.

Usage:

```c
static int emit(unsigned service, const uint8_t* bytes, size_t size, void* opaque);

caption_flv_splitter_t* ctx = caption_flv_splitter_new(max_tag, emit, opaque);
caption_flv_splitter_feed(ctx, data, size, &consumed);   /* consumed <= size */
/* ... */
caption_flv_splitter_finish(ctx);
caption_flv_splitter_free(ctx);
```

`feed` and `finish` return `0` when the data was processed, `1` when emission
is paused and `-1` on a format error, truncation or a callback abort. Input
may be split at any byte boundary; `consumed` reports only the bytes actually
taken from this call, which is 0 while pending output has not drained. Feeding
zero bytes drives pending output. Each FLV header/tag is checked in full and
all four outputs are prepared before service 1..4 are called in order; a
paused service is retried with identical bytes before any later service, and
accepted services are never replayed. `max_tag` bounds a complete tag
(11 byte header + body + 4 byte `PreviousTagSize`) and must be at least 13.
Buffering is limited to one input tag plus its four outputs; the callback owns
nothing and must copy the bytes it wants to keep. `free` discards pending
output without delivering it.

`caption/sei_wire.h` exposes the SEI wire codec shared with `mpeg.c`:
`caption_sei_decode` parses a complete NAL (0x06 header included) into a
message chain with `timestamp` 0, and `caption_sei_encode` renders a chain to
a newly allocated NAL (`06 80` for an empty chain). Both handle the full RBSP
of a NAL before reading or after writing payload type/size, so escaping across
message boundaries is correct. `sei_parse`/`sei_render` and the existing
full-screen export keep their original conventions.

`examples/flv_split_main.c` (`flv_split`) reads an input FLV and writes the
four service streams; on any failure it returns non-zero and removes the
incomplete outputs.
