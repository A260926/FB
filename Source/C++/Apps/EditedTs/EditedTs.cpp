/*****************************************************************
|
|    AP4 - Edited MP4 to MPEG2-TS converter
|
|    Copyright 2002-2025 Axiomatic Systems, LLC
|
|    This file is part of Bento4/AP4.
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2, or (at your option)
|    any later version.
|
 ****************************************************************/

#include "Ap4.h"
#include "Ap4EditedTs.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>

static bool
ParseU64(const char* text, AP4_UI64& value)
{
    if (text == NULL || *text == '\0') return false;
    errno = 0;
    char* end = NULL;
    unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    value = (AP4_UI64)parsed;
    return true;
}

static bool
ParseU32(const char* text, AP4_UI32& value)
{
    AP4_UI64 parsed = 0;
    if (!ParseU64(text, parsed) || parsed > 0xFFFFFFFFULL) return false;
    value = (AP4_UI32)parsed;
    return true;
}

int
main(int argc, char** argv)
{
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: EditedTs <input.mp4> <output.ts> "
                     "<origin90k> <pcrInterval90k>\n");
        return 1;
    }

    AP4_UI64 origin90k = 0;
    AP4_UI32 pcr_interval90k = 0;
    if (!ParseU64(argv[3], origin90k) ||
        !ParseU32(argv[4], pcr_interval90k)) {
        std::fprintf(stderr, "ERROR: invalid decimal clock argument\n");
        return 1;
    }

    AP4_ByteStream* input = NULL;
    AP4_Result result = AP4_FileByteStream::Create(
        argv[1], AP4_FileByteStream::STREAM_MODE_READ, input);
    if (AP4_FAILED(result)) {
        std::fprintf(stderr, "ERROR: cannot open input (%d)\n", result);
        return 2;
    }

    AP4_ByteStream* output = NULL;
    result = AP4_FileByteStream::Create(
        argv[2], AP4_FileByteStream::STREAM_MODE_WRITE, output);
    if (AP4_FAILED(result)) {
        std::fprintf(stderr, "ERROR: cannot open output (%d)\n", result);
        input->Release();
        return 2;
    }

    int exit_code = 0;
    {
        AP4_File file(*input, true);
        AP4_Movie* movie = file.GetMovie();
        if (movie == NULL) {
            std::fprintf(stderr, "ERROR: no movie in input\n");
            exit_code = 3;
        } else {
            result = AP4_EditedTsWriter::Write(*movie, *output,
                                               origin90k, pcr_interval90k);
            if (AP4_FAILED(result)) {
                std::fprintf(stderr, "ERROR: edited TS write failed (%d)\n", result);
                exit_code = 4;
            }
        }
    }

    output->Release();
    input->Release();
    return exit_code;
}
