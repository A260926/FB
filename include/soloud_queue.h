/*
SoLoud audio engine
Copyright (c) 2013-2018 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#ifndef SOLOUD_QUEUE_H
#define SOLOUD_QUEUE_H

#include "soloud.h"

#define SOLOUD_QUEUE_MAX 32

namespace SoLoud
{
	class Queue;

	class QueueInstance : public AudioSourceInstance
	{
		Queue *mParent;

		enum
		{
			// Fixed upper bound for the conversion cache. One chunk is read at
			// a time, and at most one extra frame is needed for interpolation.
			CONV_CACHE = SAMPLE_GRANULARITY + 2,
			CONV_CHUNK = SAMPLE_GRANULARITY
		};

		// Per-item conversion state. It is reset whenever the queue advances
		// to the next audio source.
		bool mConvStarted;
		unsigned int mConvSrcRate;
		unsigned int mConvSrcChannels;
		unsigned int mConvDstRate;
		unsigned int mConvDstChannels;
		unsigned long long mConvOutIndex;
		unsigned long long mConvSrcTotal;
		unsigned long long mConvBufStart;
		unsigned int mConvBufLen;
		float mConvBuffer[2][CONV_CACHE];
		float mConvReadBuffer[2][CONV_CACHE];

		bool convEnsureFrame(unsigned long long aIndex, unsigned long long aKeepFrom);
		float convFrame(unsigned long long aIndex, unsigned int aChannel);
		void convFinishItem();
		unsigned int getAudioConverted(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize);
	public:
		QueueInstance(Queue *aParent);
		virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize);
		virtual bool hasEnded();
		virtual ~QueueInstance();
	};

	class Queue : public AudioSource
	{
	public:
		Queue();
		virtual ~Queue();
		virtual QueueInstance *createInstance();
		// Play sound through the queue
		result play(AudioSource &aSound);
        // Number of audio sources queued for replay
        unsigned int getQueueCount();
		// Is this audio source currently playing?
		bool isCurrentlyPlaying(AudioSource &aSound);
		// Set params by reading them from an audio source
		result setParamsFromAudioSource(AudioSource &aSound);
		// Set params manually
		result setParams(float aSamplerate, unsigned int aChannels = 2);
		// Enable or disable sample rate / channel conversion of queued sources
		result setAutoConvert(bool aEnable);

	public:
	    unsigned int mReadIndex, mWriteIndex, mCount;
	    AudioSourceInstance *mSource[SOLOUD_QUEUE_MAX];
		QueueInstance *mInstance;
		handle mQueueHandle;
		bool mAutoConvert;
		bool mPlayed;
		unsigned int mSourceRate[SOLOUD_QUEUE_MAX];
		unsigned int mSourceChannels[SOLOUD_QUEUE_MAX];
		void findQueueHandle();
		void clearQueue();
		
	};
};

#endif