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

/*
 * traceOut is NULL for the plain decompose entry and points at the
 * caller-allocated side-band array (same capacity as result) for the prefix
 * trace entry.
 */
_DecodeResult decode_internal(_CodeInfo* _ci, int supportOldIntr, _DInst result[], _PrefixTraceInfo traceOut[], unsigned int maxResultCount, unsigned int* usedInstructionsCount);

#endif /* DECODER_H */
