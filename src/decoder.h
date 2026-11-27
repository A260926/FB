/*
decoder.h

diStorm3 - Powerful disassembler for X86/AMD64
http://ragestorm.net/distorm/
distorm at gmail dot com
Copyright (C) 2003-2021 Gil Dabah
This library is licensed under the BSD license. See the file COPYING.
*/


#ifndef DECODER_H
#define DECODER_H

#include "config.h"
#include "../include/prefix_trace.h"

typedef unsigned int _iflags;

_DecodeResult decode_internal(_CodeInfo* _ci, int supportOldIntr, _DInst result[], unsigned int maxResultCount, unsigned int* usedInstructionsCount);

/*
 * Same as decode_internal, plus an optional sideband array of prefix traces
 * (one entry per emitted instruction, may be NULL to skip tracing).
 */
_DecodeResult decode_internal_ex(_CodeInfo* _ci, int supportOldIntr, _DInst result[], unsigned int maxResultCount, unsigned int* usedInstructionsCount, _PrefixTraceInfo traces[]);

#endif /* DECODER_H */
