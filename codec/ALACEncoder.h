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
	File:		ALACEncoder.h
*/

#pragma once

#include <stdint.h>

#include "ALACAudioTypes.h"

// Snapshot of the encoder's *adaptive* state: the per-channel coefficient
// arrays and the last mix resolution. The LPC analysis path evaluates one
// candidate (or the original path) on a private copy so candidates cannot
// contaminate each other; the winning pass is then re-run so the state that
// ends up in the encoder is exactly the one produced by the chosen path.
struct LPCEncoderState
{
	int16_t	mLastMixRes[kALACMaxChannels];
	int16_t	mCoefsU[kALACMaxChannels][kALACMaxSearches][kALACMaxCoefs];
	int16_t	mCoefsV[kALACMaxChannels][kALACMaxSearches][kALACMaxCoefs];
};

struct BitBuffer;

class ALACEncoder
{
	public:
		ALACEncoder();
		virtual ~ALACEncoder();

		virtual int32_t	Encode(AudioFormatDescription theInputFormat, AudioFormatDescription theOutputFormat,
                                   unsigned char * theReadBuffer, unsigned char * theWriteBuffer, int32_t * ioNumBytes);
		virtual int32_t	Finish( );

		void				SetFastMode( bool fast ) { mFastMode = fast; };

		// enable/disable per-packet LPC analysis (estimated coefficients from the
		// current packet's actual data instead of the fixed warm-up start)
		// - this must be called *before* InitializeEncoder()
		// - only has an effect on 16-bit and 24-bit integer PCM input
		// - when enabled it takes priority over fast mode
		void				SetLPCAnalysis( bool analysis ) { mLPCAnalysis = analysis; };

		// this must be called *before* InitializeEncoder()
		void				SetFrameSize( uint32_t frameSize ) { mFrameSize = frameSize; };

		void				GetConfig( ALACSpecificConfig & config );
        uint32_t            GetMagicCookieSize(uint32_t inNumChannels);
        void				GetMagicCookie( void * config, uint32_t * ioSize ); 

        virtual int32_t	InitializeEncoder(AudioFormatDescription theOutputFormat);

    protected:
		virtual void		GetSourceFormat( const AudioFormatDescription * source, AudioFormatDescription * output );
		
		int32_t			EncodeStereo( struct BitBuffer * bitstream, void * input, uint32_t stride, uint32_t channelIndex, uint32_t numSamples );
		int32_t			EncodeStereoFast( struct BitBuffer * bitstream, void * input, uint32_t stride, uint32_t channelIndex, uint32_t numSamples );
		int32_t			EncodeStereoEscape( struct BitBuffer * bitstream, void * input, uint32_t stride, uint32_t numSamples );
		int32_t			EncodeMono( struct BitBuffer * bitstream, void * input, uint32_t stride, uint32_t channelIndex, uint32_t numSamples );

		// LPC analysis helpers (used when mLPCAnalysis is set)
		int32_t			EncodeStereoLPC( struct BitBuffer * bitstream, void * input, uint32_t stride, uint32_t channelIndex, uint32_t numSamples );
		int32_t			EncodeMonoLPC( struct BitBuffer * bitstream, void * input, uint32_t stride, uint32_t channelIndex, uint32_t numSamples );
		int32_t			LPCFit( const int32_t * signal, uint32_t numSamples, uint32_t order, uint32_t chanBits, int16_t * coefs );
		int32_t			EncodePacket( bool useLPCAnalysis, struct BitBuffer * bitstream, AudioFormatDescription theInputFormat,
									unsigned char * theReadBuffer, uint32_t numFrames );
		void			SaveEncoderState( struct LPCEncoderState * state );
		void			RestoreEncoderState( const struct LPCEncoderState * state );


		// ALAC encoder parameters
		int16_t					mBitDepth;
		bool					mFastMode;

		// per-packet LPC analysis mode (off by default, set before InitializeEncoder)
		bool					mLPCAnalysis;

		// encoding state
		int16_t					mLastMixRes[kALACMaxChannels];

		// encoding buffers
		int32_t *				mMixBufferU;
		int32_t *				mMixBufferV;
		int32_t *				mPredictorU;
		int32_t *				mPredictorV;
		uint16_t *				mShiftBufferUV;
		
		uint8_t *					mWorkBuffer;
		uint8_t *					mLPCDryBuffer;		// private output buffer for analysis evaluation

		// per-channel coefficients buffers
		int16_t					mCoefsU[kALACMaxChannels][kALACMaxSearches][kALACMaxCoefs];
		int16_t					mCoefsV[kALACMaxChannels][kALACMaxSearches][kALACMaxCoefs];

		// encoding statistics
		uint32_t					mTotalBytesGenerated;
		uint32_t					mAvgBitRate;
		uint32_t					mMaxFrameBytes;
        uint32_t                  mFrameSize;
        uint32_t                  mMaxOutputBytes;
        uint32_t                  mNumChannels;
        uint32_t                  mOutputSampleRate;		
};
