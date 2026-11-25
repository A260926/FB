/**********************************************************************************************/
/* The MIT License                                                                            */
/*                                                                                            */
/* Copyright 2016-2017 Twitch Interactive, Inc. or its affiliates. All Rights Reserved.       */
/*                                                                                            */
/* Permission is hereby granted, free of charge, to any person obtaining a copy               */
/* of this software and associated documentation files (the "Software"), to deal              */
/* in the Software without restriction, including without limitation the rights               */
/* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell                  */
/* copies of the Software, and to permit persons to whom the Software is                      */
/* furnished to do so, subject to the following conditions:                                   */
/*                                                                                            */
/* The above copyright notice and this permission notice shall be included in                 */
/* all copies or substantial portions of the Software.                                        */
/*                                                                                            */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR                 */
/* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,                   */
/* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE                */
/* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER                     */
/* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,              */
/* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN                  */
/* THE SOFTWARE.                                                                              */
/**********************************************************************************************/
#ifndef LIBCAPTION_SEI_WIRE_H
#define LIBCAPTION_SEI_WIRE_H
#ifdef __cplusplus
extern "C" {
#endif

#include "mpeg.h"
#include <stddef.h>

/*
 * Decode a complete H.264 SEI NAL unit (including the 0x06 NAL header) into a
 * message chain owned by libcaption's SEI representation.  The whole NAL is
 * RBSP-unescaped before the payload type/size fields are inspected, so 0x03
 * emulation prevention bytes embedded in headers, payloads and at message
 * boundaries are all handled.  Zero length messages and payload types/sizes
 * encoded with multiple 0xFF extension bytes are preserved.
 *
 * The caller must pass an already sei_init()'ed and empty chain.  On success
 * an independent message chain is produced and out->timestamp is set to 0.
 * On failure out is left empty and -1 is returned.
 */
int caption_sei_decode(sei_t* out, const uint8_t* nal, size_t size);

/*
 * Encode a message chain as a complete H.264 SEI NAL unit (including the 0x06
 * NAL header and the RBSP stop bit).  An empty chain encodes as 06 80.
 *
 * The complete RBSP (message headers, payloads, message boundaries and the
 * stop bit) is escaped as one stream; the emulation-prevention zero count is
 * never reset per payload.  The returned buffer is independently allocated
 * and must be released by the caller with free().  On failure *out is set to
 * NULL and *size to 0.
 */
int caption_sei_encode(const sei_t* in, uint8_t** out, size_t* size);

#ifdef __cplusplus
}
#endif
#endif
