/*
 * Copyright (c) 2011 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

/*
	File:		ALACDecoder.h
*/

#ifndef _ALACDECODER_H
#define _ALACDECODER_H

#if PRAGMA_ONCE
#pragma once
#endif

#include <stdint.h>

#include "ALACAudioTypes.h"

struct BitBuffer;

// Description of one audio element as it was parsed from a packet.  Bit
// offsets are relative to the start of the packet bitstream.  This is used by
// the alacconvert channel projection path to locate exact element boundaries
// without searching for tag values.
struct ALACElementTrace
{
	uint8_t		tag;			// ID_SCE, ID_CPE or ID_LFE
	uint8_t		instanceTag;
	uint32_t	channelStart;	// first decoded channel slot
	uint32_t	channelCount;	// 1 for SCE/LFE, 2 for CPE
	uint32_t	startBit;		// position of the 3-bit element tag
	uint32_t	endBit;			// position just past the element syntax
	uint32_t	numSamples;		// sample frames carried by the element
};

class ALACDecoder
{
	public:
		ALACDecoder();
		~ALACDecoder();

		int32_t	Init( void * inMagicCookie, uint32_t inMagicCookieSize );
		int32_t	Decode( struct BitBuffer * bits, uint8_t * sampleBuffer, uint32_t numSamples, uint32_t numChannels, uint32_t * outNumSamples );

		// Strict packet decode used by the projection path.  Unlike Decode(),
		// this parses every element through ID_END, rejects elements other
		// than SCE/CPE/LFE, requires the channel total to match numChannels,
		// and requires the packet to end exactly on a byte boundary after
		// ID_END.  Element boundaries and instance tags are reported in
		// 'traces'.
		int32_t	DecodePacket( struct BitBuffer * bits, uint8_t * sampleBuffer, uint32_t numSamples, uint32_t numChannels,
							  uint32_t * outNumSamples, ALACElementTrace * traces, uint32_t maxTraces, uint32_t * outTraceCount );

	public:
		// decoding parameters (public for use in the analyzer)
		ALACSpecificConfig		mConfig;

	protected:
		int32_t	FillElement( struct BitBuffer * bits );
		int32_t	DataStreamElement( struct BitBuffer * bits );

		// Single-element decoders.  The 3-bit element tag has already been
		// consumed; everything else including the instance tag is read here.
		int32_t	DecodeMonoElement( struct BitBuffer * bits, uint8_t * sampleBuffer, uint32_t channelIndex,
									   uint32_t numChannels, uint32_t * ioNumSamples, uint8_t * outInstanceTag );
		int32_t	DecodeStereoElement( struct BitBuffer * bits, uint8_t * sampleBuffer, uint32_t channelIndex,
									     uint32_t numChannels, uint32_t * ioNumSamples, uint8_t * outInstanceTag );

		uint16_t					mActiveElements;

		// decoding buffers
		int32_t *				mMixBufferU;
		int32_t *				mMixBufferV;
		int32_t *				mPredictor;
		uint16_t *				mShiftBuffer;	// note: this points to mPredictor's memory but different
												//		 variable for clarity and type difference
};

#endif	/* _ALACDECODER_H */
