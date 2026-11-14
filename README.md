<!--
  SPDX-FileCopyrightText: © 2014 Team CharLS
  SPDX-License-Identifier: BSD-3-Clause
-->

<img src="https://raw.githubusercontent.com/team-charls/charls/main/doc/jpeg_ls_logo.png" alt="JPEG-LS Logo" width="100"/>

# CharLS

[![License](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](https://raw.githubusercontent.com/team-charls/charls/main/LICENSE.md)
[![REUSE status](https://api.reuse.software/badge/github.com/team-charls/charls)](https://api.reuse.software/info/github.com/team-charls/charls)
[![Build status](https://ci.appveyor.com/api/projects/status/yq0naf3v2m8nfa8r/branch/main?svg=true)](https://ci.appveyor.com/project/vbaderks/charls/branch/main)
[![Build Status](https://dev.azure.com/team-charls/charls/_apis/build/status/team-charls.charls?branchName=main)](https://dev.azure.com/team-charls/charls/_build/latest?definitionId=2&branchName=main)
[![Reliability Rating](https://sonarcloud.io/api/project_badges/measure?project=team-charls_charls&metric=reliability_rating)](https://sonarcloud.io/summary/new_code?id=team-charls_charls)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/13142/badge)](https://www.bestpractices.dev/projects/13142)
[![Vcpkg package](https://repology.org/badge/version-for-repo/vcpkg/charls.svg)](https://repology.org/metapackage/charls)
[![Conan Center](https://img.shields.io/conan/v/charls)](https://conan.io/center/recipes/charls)

CharLS is a C++ implementation of the JPEG-LS standard for lossless and near-lossless image compression and decompression.
JPEG-LS is a low-complexity image compression standard that matches JPEG 2000 compression ratios.

## Features

* C++17 library implementation with a binary C interface for maximum interoperability.
* Supports Windows, Linux and macOS on x86, x64, arm, arm64, mips, s390x and ppc64le.
* Adapters for .NET, JavaScript (WebAssembly) and Python applications available.
* Excellent compression and decompression performance.

## About JPEG-LS

JPEG-LS (ISO/IEC 14495-1:1999 / ITU-T.87) is an image compression standard derived from the Hewlett Packard LOCO algorithm. JPEG-LS has low complexity (meaning fast compression) and high compression ratios, similar to the JPEG 2000 lossless ratios. JPEG-LS is more similar to the old Lossless JPEG than to JPEG 2000, but interestingly the two different techniques result in vastly different performance characteristics.
Wikipedia on lossless JPEG and JPEG-LS: <https://en.wikipedia.org/wiki/Lossless_JPEG>  
Tip: ITU makes their version of the JPEG-LS standard (ITU-T.87) freely available for download. The text is identical with the ISO version.

## About this software

This project's goal is to provide a full implementation of the ISO/IEC 14495-1:1999, "Lossless and near-lossless compression of continuous-tone still images: Baseline" standard. This library is written from scratch in portable C++. The main branch uses C++17. The 2.x branch is maintained in C++14. All mainstream JPEG-LS features are implemented by this library.
According to preliminary test results published on <https://imagecompression.info/gralic,> CharLS is about *twice as fast* as the original HP code, and beats both JPEG-XR and JPEG 2000 by a factor 3.

### Limitations

The following JPEG-LS options are not supported by the CharLS implementation. Most of these options are rarely used in practice.

* No support to encode JPEG restart markers  
  Decoding is supported, but no recovery mechanism is implemented for corrupted JPEG-LS files.
* No support for subsampled scans.
  Subsampling is a lossy encoding mechanism and not used in lossless scenarios.
* No support for point transform.
  Point transform is a lossy encoding mechanism and not used in lossless scenarios.

#### Note about JPEG-LS part 2

After releasing the original baseline JPEG-LS standard ISO 14495-1:1999, ISO released an extension to the JPEG-LS standard called ISO/IEC 14495-2:2003: "Lossless and near-lossless compression of continuous-tone still images: Extensions". CharLS doesn't support this extension.

## Supported platforms

The code is regularly compiled/tested on Windows and 64-bit Linux. Additionally, the code has been successfully tested on Linux Intel/AMD 32/64 bit (slackware, debian, gentoo), Solaris SPARC systems, Intel based Macs and Windows CE (ARM CPU, emulated), where the less common compilers may require minor code edits. It leverages C++ language features (templates, traits) to create optimized code, which generally perform best with recent compilers.

### Support Matrix

| Dimension     | Supported Version      |
|---------------|------------------------|
| C Version     | >= 17                  |
| C++ Version   | >= 17                  |
| CMake         | >= 3.22                |
| GCC           | >= 9.1                 |
| Clang         | >= 7.0.0               |
| MSVC          | >= 2019                |
| Apple Clang   | >= 12                  |

## Getting Started

With [vcpkg](https://github.com/Microsoft/vcpkg) on Windows

```powershell
PS> vcpkg install charls charls:x64-windows
```

With [vcpkg](https://github.com/Microsoft/vcpkg) on Linux or macOS

```bash
~/$ ./vcpkg install charls
```

For other platforms, more install options, how to build from source, and more, take a look at the [documentation](https://github.com/team-charls/charls/wiki).

Once you have the library, the sample folder provides some code samples to get you started.

### Incremental (row by row) decoding

`charls::jpegls_stream_decoder` decodes a JPEG-LS byte stream that is supplied in small pieces and
delivers one complete row at a time. It does not require the complete compressed stream or a
destination buffer for the whole image. The parser and entropy decoder used by the existing
whole-frame C and C++ API are not affected; both APIs use the same JPEG-LS numeric rules.

```cpp
#include <charls/jpegls_stream_decoder.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

// Decodes a stream that arrives in small chunks and hands every decoded row to process_row().
// process_row() is application specific: it receives the row bytes and their size.
void process_row(const std::byte* row_data, std::size_t row_size);

void decode_stream(const std::vector<std::byte>& encoded, std::size_t chunk_size)
{
    charls::jpegls_stream_decoder decoder;

    std::size_t offset = 0;
    bool done = false;
    while (!done)
    {
        const std::size_t size = std::min(chunk_size, encoded.size() - offset);
        const bool final = offset + size == encoded.size();

        // The decoder reports how many bytes it accepted. A suffix that was not accepted needs to be
        // submitted again; consumed bytes may be overwritten or released immediately.
        const charls::stream_decode_result result = decoder.push(encoded.data() + offset, size, final);
        offset += result.consumed;

        switch (result.status)
        {
        case charls::stream_decode_status::row_ready:
            // The row is only valid until the next push or reset. Its layout matches the whole-frame
            // decoder: samples of a pixel are adjacent; samples above 8 bits use the host byte order.
            const auto [row_data, row_size] = decoder.row();
            process_row(row_data, row_size);
            break;

        case charls::stream_decode_status::need_input:
            // All input was consumed, but no complete row is available yet. Keep final=true once the
            // end of the input has been reached and submit the remaining suffix (or an empty buffer).
            break;

        case charls::stream_decode_status::done:
            // All rows were delivered and the EOI marker was consumed and verified.
            // Bytes after the EOI marker are not consumed and stay with the caller.
            done = true;
            break;
        }
    }
}

// A decoder can be reused for another stream, also with a different frame size.
void reuse_decoder()
{
    charls::jpegls_stream_decoder decoder;
    decoder.reset(); // Returns the decoder to the state directly after construction.
}
```

`frame_info()` returns the frame information once the JPEG-LS frame header has been read. Calling it
before that, or calling `row()` when the last `push` did not return `row_ready`, throws
`charls::jpegls_error` with `jpegls_errc::invalid_operation`. `push` throws `jpegls_error` for a
corrupted stream, for a stream that ends before all rows and the EOI marker have been read, and for
JPEG-LS features that this entry point does not support (multi-scan planar color data, mapping
tables, HP color transforms, DNL and oversize image dimensions). After an error the object must be
reset before it can be reused.

## Release Management

This repository is provided as source code, and specifically does not offer binary releases. Instead, it is encouraged to either “live at head” (build from the latest version of or, if necessary, build against a known, supported branch, known as a Long Term Support (LTS) branch.
Support for older compiler versions will be phased out, 5 years from the moment that a newer version of that compiler has become available. The same applies to the minimal required C++ language version.

### Long Term Support (LTS) Branches

Before any major breaking change in the API and/or ABI a branch will be created from main to freeze that snapshot as LTS branch.

## Related Projects

* [CharLS.Native .NET](https://github.com/team-charls/charls-native-dotnet) - a .NET adapter assembly for CharLS: [NuGet package](https://www.nuget.org/packages/CharLS.Native/)
* [JPEG-LS WIC codec](https://github.com/team-charls/jpegls-wic-codec) - Windows Imaging Component (WIC) codec for JPEG-LS .jls files
* [charls-js](https://github.com/chafey/charls-js) - WebAssembly build of CharLS: [Demo](https://chafey.github.io/charls-js/test/browser/index.html)
* [JPEG-LS plugin for Python Pillow](https://github.com/planetmarshall/pillow-jpls) - JPEG-LS plugin for Python Pillow: [Pip package](https://pypi.org/project/pillow-jpls/)

## Users & Acknowledgements

CharLS is being used by [GDCM DICOM toolkit](https://sourceforge.net/projects/gdcm/), thanks for [Mathieu Malaterre](https://sourceforge.net/users/malat) for getting CharLS started on Linux. Kato Kanryu wrote an initial version of the color transforms and the DIB output format code, for an [irfanview](https://www.irfanview.com) plugin using CharLS.

## Legal

The code in this project is available through a BSD style license, allowing use of the code in commercial closed source applications if you wish. **All** the code in this project is written from scratch, and not based on other JPEG-LS implementations. Be aware that Hewlett Packard claims to own patents that apply to JPEG-LS implementations, but they license it for free for conformant JPEG-LS implementations. Some of these patents may already have expired in your country. Read more at <https://hpl.hp.com/loco/> before you use this if you use this code for commercial purposes.
