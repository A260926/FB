libaribcaption
======
[日本語](README_ja.md)

A portable caption decoder / renderer for handling ARIB STD-B24 based TV broadcast captions.

## Background
While **CEA-608/708** closed caption standards are used by the **ATSC** system in North America,
**DVB Subtitles / DVB Teletext** defined in **DVB** standard are used in Europe and many parts of the world,
Japan established its own TV broadcasting standard **ISDB** that includes a kind of caption service defined in **ARIB STD-B24**
by the **A**ssociation of **R**adio **I**ndustries and **B**usinesses (ARIB).

Brazil also adopted **ISDB-T** International for their broadcasting by establishing Brazilian version **SBTVD / ISDB-Tb** based on the Japanese standard,
which has been widely used in South America countries and around the world.
Brazilian version also includes a caption service for Latin languages defined in **ABNT NBR 15606-1** which is modified from **ARIB STD-B24** specification.
Philippines also adopted ISDB-T International based on the Brazilian standards,
but uses **UTF-8** for caption encoding based on the Japansese specification **ARIB STD-B24**.

Though ISDB-based TV broadcasting has been operating for about 20 years, ARIB based caption is still lacking support in general players.

## Overview
libaribcaption provides decoder and renderer for handling ARIB STD-B24 based broadcast captions,
making it possible for general players to render ARIB captions with the same effect (or even better) as Television.

libaribcaption is written in C++17 but also provides C interfaces to make it easier to integrate into video players.
It is a lightweight library that only depends on libfreetype and libfontconfig in the worst case.

libaribcaption is a cross-platform library that works on various platforms, including but not limited to:
- Windows 7+
- Windows XP+  (libfreetype required)
- Linux  (libfreetype and libfontconfig required)
- Android 2.x+  (libfreetype required)
- macOS
- iOS

## Screenshot
![screenshot0.png](screenshots/screenshot0.png)

## Features
- Support captions in Japanese (ARIB STD-B24 JIS), Latin languages (ABNT NBR 15606-1) and Philippine (ARIB STD-B24 UTF-8)
- Re-encode Japanese Profile A 960x540 captions (including cached DRCS patterns) back into ARIB private caption packets
- Full support for rendering ARIB additional symbols (Gaiji) and DRCS characters
- Lightweight and portable implementation that works on various platforms
- Performance optimized (SSE2 on x86/x64) graphics rendering
- Multiple text rendering backend driven by DirectWrite / CoreText / FreeType
- Zero third-party dependencies on Windows (using DirectWrite) and macOS / iOS (using CoreText)
- Built-in font fallback mechanism
- Built-in DRCS converting table for replacing / rendering known DRCS characters into / by alternative Unicode

## Build
CMake 3.28+ and a C++17 compatible compiler will be necessary for building. Usually you just have to:
```bash
cd libaribcaption
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j8
cmake --install .      # Optional
```

By default libaribcaption is compiled as static library, indicate `ARIBCC_SHARED_LIBRARY:BOOL=ON` to build as a shared library:
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DARIBCC_SHARED_LIBRARY:BOOL=ON    # or -DBUILD_SHARED_LIBS:BOOL=ON
```

libaribcaption has several CMake options that can be specified:
```bash
ARIBCC_BUILD_TESTS:BOOL            # Compile test codes inside /test. Default to OFF
ARIBCC_SHARED_LIBRARY:BOOL         # Compile as shared library. Default to OFF
ARIBCC_NO_EXCEPTIONS:BOOL          # Disable C++ Exceptions. Default to OFF
ARIBCC_NO_RTTI:BOOL                # Disable C++ RTTI. Default to OFF
ARIBCC_NO_RENDERER:BOOL            # Disable the renderer and leave only the decoder behind. Default to OFF
ARIBCC_IS_ANDROID:BOOL             # Indicate target platform is Android. Detected automatically by default.
ARIBCC_USE_DIRECTWRITE:BOOL        # Enable DirectWrite font provider & renderer. Default to ON on Windows
ARIBCC_USE_GDI_FONT:BOOL           # Enable GDI font provider which is necessary for WinXP support. Default to OFF.
ARIBCC_USE_CORETEXT:BOOL           # Enable CoreText font provider & renderer. Default to ON on macOS / iOS
ARIBCC_USE_FREETYPE:BOOL           # Enable FreeType based renderer. Default to ON on Linux / Android
ARIBCC_USE_EMBEDDED_FREETYPE:BOOL  # Use embedded FreeType instead of searching system library. Default to OFF
ARIBCC_USE_FONTCONFIG:BOOL         # Enable Fontconfig font provider. Default to ON on Linux and other platforms
```

By default, libaribcaption only enables DirectWrite on Windows and CoreText on macOS / iOS without any third-party
dependencies, But you can still enable the FreeType based text renderer by indicating
`-DARIBCC_USE_FREETYPE:BOOL=ON`.

For Windows XP support, you have to turn off DirectWrite (which will result in a crash), enable GDI font provider and FreeType:
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -ARIBCC_USE_DIRECTWRITE:BOOL=OFF -DARIBCC_USE_GDI_FONT:BOOL=ON -DARIBCC_USE_FREETYPE:BOOL=ON
```

For enabling FreeType text renderer on Windows, consider using `vcpkg` or `msys2` for accessing third-party libraries.

If you are under some kind of environment (like Android NDK or Windows) that is hard to prepare system-wide installed FreeType,
consider using embedded FreeType by indicating `-DARIBCC_USE_EMBEDDED_FREETYPE:BOOL=ON`.
This option will automatically fetch and compile a static-linked FreeType library internally.

## Usage
libaribcaption could be imported through `find_package()` if you have installed it into system:
```cmake
cmake_minimum_required(VERSION 3.28)
project(testarib LANGUAGES C CXX)

find_package(aribcaption REQUIRED)

add_executable(testarib main.cpp)

target_compile_features(testarib
    PRIVATE
        cxx_std_17
)

target_include_directories(testarib
    PRIVATE
        ${ARIBCAPTION_INCLUDE_DIR}
)

target_link_libraries(testarib
    PRIVATE
        aribcaption::aribcaption
)
```

Or using `add_subdirectory()` to import source folder directly:

```cmake
cmake_minimum_required(VERSION 3.28)
project(testarib2 LANGUAGES C CXX)

set(ARIBCC_USE_FREETYPE ON CACHE BOOL "Enable FreeType")    # Indicate options here (optional)
add_subdirectory(thirdparty/libaribcaption)

add_executable(testarib2 main.cpp)

target_compile_features(testarib2
    PRIVATE
        cxx_std_17
)

target_link_libraries(testarib2
    PRIVATE
        aribcaption::aribcaption
)
```

Or using pkg-config if you have installed it into system:
```bash
# Link to libaribcaption static library
gcc main.c -o main `pkg-config --cflags --libs --static libaribcaption`

# Link to libaribcaption shared library
gcc main.c -o main `pkg-config --cflags --libs libaribcaption`
```

## Encoder
`aribcaption::Encoder` (see [encoder.hpp](include/aribcaption/encoder.hpp)) turns `Caption` objects back into
ARIB STD-B24 private caption data packets which can be fed into the `Decoder`. Each packet starts with the
`0x80 0xFF 0xF0` private caption header (no outer MPEG-TS framing or PES start code) and carries a CRC-16.

Supported scope (anything outside it makes `Encode()` return `false` without touching the output or the
encoder state):

- Japanese JIS encoding, first language, Profile A, 960x540 full-screen captions
- 36x36 characters, horizontal spacing 4, vertical spacing 24, horizontal/vertical scale 1.0
- No ruby, decoration, enclosure or built-in sound; transparent background
- The eight opaque foreground colors of CLUT palette 0; text positions are non-negative integers whose
  40x60 character box lies completely inside the plane
- Text characters whose Unicode codepoint is reversibly representable through the bundled JIS conversion
  tables (UTF-8 mode is never used, a question mark is never treated as success)
- DRCS patterns of 1..64 width/height, 2 or 4 tones, compact high-bit-first pixels; only the 94 slots of
  DRCS-1 are used. Identical bitmaps share a slot across captions, definitions are emitted only when a
  pattern is first used or its slot is replaced, and `Reset()` clears the cache and restarts the session
  with a Japanese caption management group

```cpp
#include <aribcaption/encoder.hpp>

aribcaption::Encoder encoder;
std::vector<aribcaption::EncodedPES> packets;
if (encoder.Encode(caption, packets)) {
    // packets[*].pts mirrors the source Caption (PTS_NOPTS is preserved)
    // packets[*].data starts with 0x80 0xFF 0xF0
}
```

The [caption_encode](examples/caption_encode.cpp) example writes two captions sharing one DRCS bitmap to a
file (8-byte big-endian signed PTS, 4-byte big-endian length, packet bytes) and verifies them in a separate
read-only decoder process:

```bash
# Build the library, encoder, example and round-trip tests, then run everything
bash task_support/run.sh target

# Or run the example directly
caption_encode output.pes             # write mode (removes any previous output)
caption_encode --decode output.pes    # separate-process decoder verification
```

## Documents
See the comments in [public headers](include/aribcaption), and [sample code with ffmpeg](test/ffmpeg)

## Hints
libaribcaption's C++ headers are also written in C++17. If your environment doesn't support C++17,
consider using the C API or switch to a newer compiler.

The C API ([public headers] with ".h" extensions) could be useful for calling from Pure C or other languages,
see [capi sample](test/capi) for usage.

[public headers]: include/aribcaption

## Recommended fonts
These fonts are recommended for Japanese ARIB caption rendering:

Windows TV MaruGothic

Hiragino Maru Gothic ProN (macOS)

[Rounded M+ 1m for ARIB](https://www.axfc.net/u/3107925)

[和田研中丸ゴシック2004ARIB](https://ja.osdn.net/projects/jis2004/wiki/FrontPage)

## License
libaribcaption is released under MIT License. You should include the copyright notice and permission notice in your distribution.

## References
[ARIB STD-B24](https://www.arib.or.jp/english/std_tr/broadcasting/std-b24.html)

[ARIB TR-B14](https://www.arib.or.jp/english/std_tr/broadcasting/tr-b14.html)

ABNT NBR 15606-1

[ISDB-T Standards (Philippines)](https://ntc.gov.ph/wp-content/uploads/2018/MC/MC-07-12-2014-Attachment.pdf)

## Other implementations
libaribcaption is heavily inspired by the following projects:

[aribb24](https://github.com/nkoriyama/aribb24)

[aribb24.js](https://github.com/monyone/aribb24.js)

[TVCaptionMod2](https://github.com/xtne6f/TVCaptionMod2)
