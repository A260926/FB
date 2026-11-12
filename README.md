# version
v0.8
Matthew Szatmary m3u8@twitch.tv / matt@szatmary.org

# libcaption

libcaption is a library written in C to aid in the creating and parsing of closed caption data, open sourced under the MIT license to use within community developed broadcast tools. To maintain consistency across platforms libcaption aims to implement a subset of EIA608, CEA708 as supported by the Apple iOS platform.

608 support is currently limited to encoding and decoding the necessary control and preamble codes as well as support for the Basic North American, Special North American and Extended Western European character sets.

708 support is limited to encoding the 608 data in NTSC field 1 user data type structure. Decoding of native DTVCC (CEA-708) caption packets into per-service windows is available through `caption/dtvcc.h`, see below.

In addition, utility functions to create h.264 SEI (Supplementary enhancement information) NALUs (Network Abstraction Layer Unit) for inclusion into an h.264 elementary stream are provided.

H.264 utility functions are limited to wrapping the 708 payload into a SEI NALU. This is accomplished by prepending the 708 payload with 3 bytes (nal_unit_type = 6, payloadType = 4, and PayloadSize = variable), and appending a stop bit encoded into a full byte (with a value of 127). In addition if the 708 payload contains an emulated start code, a three byte sequence equaling 0,0,1 an emulation prevention byte (3) is inserted. Functions to reverse this operation are also provided.

## DTVCC (CEA-708) window decoder

`caption/dtvcc.h` decodes native DTVCC caption packets (cc_type 2/3 of GA94 user data) into per-service caption windows. The decoder covers transport and window/text state only: fonts, colors and pixel rendering are out of scope, and the window model is kept as-is (it is not reduced to the 15x32 608 screen). The player reads window snapshots and performs its own layout.

Supported subset: services 1-6 and extended services 7-63 (service 0 is padding); commands DF0-DF7, CW0-CW7, CLW, DSW, HDW, TGW, DLW, RST and SPL; G0 printable characters (Basic North American set), BS, CR, FF and NUL. Anything else (SPA, SPC, SWA, DLY, DLC, G1/G2/G3 characters, ...) is rejected with `DTVCC_STATUS_UNSUPPORTED`, malformed input (e.g. truncated command parameters, a service block running past its packet) with `DTVCC_STATUS_ERROR`. A rejected packet is discarded as a whole, previously received packets are not affected. Detected packet loss clears all service windows until the next complete packet arrives.

```c
#include "caption/dtvcc.h"

dtvcc_decoder_t* dec = dtvcc_decoder_create();

// for each cea708_t (parsed from h.264, or constructed with cea708_add_cc_data):
// 608 entries (cc_type 0/1) are ignored here, keep feeding them to caption_frame_decode
dtvcc_status_t status = dtvcc_decoder_decode(dec, &cea708);

// read a self contained snapshot, later input never modifies it
dtvcc_window_snapshot_t snap;
if (dtvcc_decoder_window_snapshot(dec, 1 /*service*/, 0 /*window*/, &snap)) {
    // snap.visible, snap.row_count/col_count, snap.pen_row/pen_col, snap.text[row], ...
}

dtvcc_decoder_reset(dec); // drop all state, e.g. on a seek
dtvcc_decoder_free(dec);
```

A complete runnable version of this snippet is available in `examples/dtvccdump.c`.

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
