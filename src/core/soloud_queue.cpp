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

#include "soloud.h"
#include <string.h>

namespace SoLoud
{
	namespace
	{
		bool supportedFormat(float aSamplerate, unsigned int aChannels)
		{
			if (aChannels != 1 && aChannels != 2)
				return false;
			if (!(aSamplerate > 0.0f) || aSamplerate > 4294967040.0f)
				return false;
			unsigned int rate = (unsigned int)aSamplerate;
			return (float)rate == aSamplerate;
		}
	}

	QueueInstance::QueueInstance(Queue *aParent)
	{
		mParent = aParent;
		mFlags |= PROTECTED;
		mConvStarted = false;
		mConvSrcRate = 0;
		mConvSrcChannels = 0;
		mConvDstRate = 0;
		mConvDstChannels = 0;
		mConvOutIndex = 0;
		mConvSrcTotal = 0;
		mConvBufStart = 0;
		mConvBufLen = 0;
	}

	float QueueInstance::convFrame(unsigned long long aIndex, unsigned int aChannel)
	{
		return mConvBuffer[aChannel][(unsigned int)(aIndex - mConvBufStart)];
	}

	bool QueueInstance::convEnsureFrame(unsigned long long aIndex, unsigned long long aKeepFrom)
	{
		AudioSourceInstance *source = mParent->mSource[mParent->mReadIndex];

		while (aIndex >= mConvSrcTotal)
		{
			if (source->hasEnded())
				return false;

			// Drop frames which can no longer take part in interpolation.
			unsigned int drop = 0;
			if (aKeepFrom > mConvBufStart)
			{
				unsigned long long dropTo = aKeepFrom;
				if (dropTo > mConvSrcTotal)
					dropTo = mConvSrcTotal;
				drop = (unsigned int)(dropTo - mConvBufStart);
			}
			if (drop)
			{
				unsigned int c;
				for (c = 0; c < mConvSrcChannels; c++)
					memmove(mConvBuffer[c], mConvBuffer[c] + drop, sizeof(float) * (mConvBufLen - drop));
				mConvBufStart += drop;
				mConvBufLen -= drop;
			}

			unsigned int space = CONV_CACHE - mConvBufLen;
			unsigned int want = space < CONV_CHUNK ? space : CONV_CHUNK;
			if (want == 0)
				return false;

			unsigned int read = source->getAudio(mConvReadBuffer[0], want, CONV_CACHE);
			if (read > want)
				read = want;
			if (read == 0)
			{
				// A short read is not EOF. If the source has not ended, return
				// what has been produced so far and let the next call retry.
				return false;
			}

			unsigned int c;
			for (c = 0; c < mConvSrcChannels; c++)
				memcpy(mConvBuffer[c] + mConvBufLen, mConvReadBuffer[c], sizeof(float) * read);
			mConvBufLen += read;
			mConvSrcTotal += read;
		}
		return true;
	}

	void QueueInstance::convFinishItem()
	{
		AudioSourceInstance *source = mParent->mSource[mParent->mReadIndex];
		if (source)
		{
			delete source;
			mParent->mSource[mParent->mReadIndex] = 0;
		}
		mParent->mReadIndex = (mParent->mReadIndex + 1) % SOLOUD_QUEUE_MAX;
		mParent->mCount--;
		mLoopCount++;
		mConvStarted = false;
	}

	unsigned int QueueInstance::getAudioConverted(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		unsigned int produced = 0;

		while (produced < aSamplesToRead && mParent->mCount)
		{
			AudioSourceInstance *source = mParent->mSource[mParent->mReadIndex];
			if (source == 0)
			{
				// Defensive: keep the queue consistent even if an entry is gone.
				mParent->mReadIndex = (mParent->mReadIndex + 1) % SOLOUD_QUEUE_MAX;
				mParent->mCount--;
				mConvStarted = false;
				continue;
			}

			if (!mConvStarted)
			{
				// Each item starts from its own source position zero, with no
				// interpolation history carried over from the previous item.
				mConvStarted = true;
				mConvSrcRate = mParent->mSourceRate[mParent->mReadIndex];
				mConvSrcChannels = mParent->mSourceChannels[mParent->mReadIndex];
				mConvDstRate = (unsigned int)mBaseSamplerate;
				mConvDstChannels = mChannels;
				mConvOutIndex = 0;
				mConvSrcTotal = 0;
				mConvBufStart = 0;
				mConvBufLen = 0;

				// Empty sources are skipped without producing anything.
				if (source->hasEnded())
				{
					convFinishItem();
					continue;
				}
			}

			unsigned long long position = mConvOutIndex * (unsigned long long)mConvSrcRate;
			unsigned long long index = position / mConvDstRate;
			unsigned int remainder = (unsigned int)(position % mConvDstRate);

			if (source->hasEnded() && index >= mConvSrcTotal)
			{
				convFinishItem();
				continue;
			}

			if (!convEnsureFrame(index, index))
			{
				if (source->hasEnded() && index >= mConvSrcTotal)
				{
					convFinishItem();
					continue;
				}
				break;
			}

			float values[2];
			unsigned int c;
			bool blocked = false;
			for (c = 0; c < mConvSrcChannels; c++)
			{
				float left = convFrame(index, c);
				if (remainder == 0)
				{
					values[c] = left;
				}
				else
				{
					unsigned long long right = index + 1;
					if (right >= mConvSrcTotal)
					{
						if (source->hasEnded())
						{
							// The right neighbor at the end is clamped to the last
							// available source frame.
							right = mConvSrcTotal - 1;
						}
						else if (!convEnsureFrame(right, index))
						{
							if (source->hasEnded())
							{
								right = mConvSrcTotal - 1;
							}
							else
							{
								blocked = true;
								break;
							}
						}
					}
					float rightValue = convFrame(right, c);
					float frac = (float)((double)remainder / (double)mConvDstRate);
					values[c] = left + (rightValue - left) * frac;
				}
			}
			if (blocked)
				break;

			if (mConvDstChannels == 1)
			{
				aBuffer[produced] = (mConvSrcChannels == 2)
					? (values[0] + values[1]) * 0.5f
					: values[0];
			}
			else
			{
				aBuffer[produced] = values[0];
				aBuffer[aBufferSize + produced] = (mConvSrcChannels == 2) ? values[1] : values[0];
			}

			produced++;
			mConvOutIndex++;

			// If this was the last converted frame, advance immediately so the
			// finished item no longer counts as the currently playing one.
			if (source->hasEnded() &&
				mConvOutIndex * (unsigned long long)mConvSrcRate >=
				mConvSrcTotal * (unsigned long long)mConvDstRate)
			{
				convFinishItem();
			}
		}

		return produced;
	}

	unsigned int QueueInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		if (!mParent->mAutoConvert)
		{
			if (mParent->mCount == 0)
			{
				return 0;
			}
			unsigned int copycount = aSamplesToRead;
			unsigned int copyofs = 0;
			while (copycount && mParent->mCount)
			{
				int readcount = mParent->mSource[mParent->mReadIndex]->getAudio(aBuffer + copyofs, copycount, aBufferSize);
				copyofs += readcount;
				copycount -= readcount;
				if (mParent->mSource[mParent->mReadIndex]->hasEnded())
				{
					delete mParent->mSource[mParent->mReadIndex];
					mParent->mSource[mParent->mReadIndex] = 0;
					mParent->mReadIndex = (mParent->mReadIndex + 1) % SOLOUD_QUEUE_MAX;
					mParent->mCount--;
					mLoopCount++;
				}
			}
			return copyofs;
		}

		return getAudioConverted(aBuffer, aSamplesToRead, aBufferSize);
	}

	bool QueueInstance::hasEnded()
	{
		return mLoopCount != 0 && mParent->mCount == 0;
	}

	QueueInstance::~QueueInstance()
	{
		if (mParent)
		{
			if (mParent->mInstance == this)
			{
				mParent->mInstance = 0;
				mParent->mQueueHandle = 0;
			}
			// Stopping the queue releases everything that has not been played,
			// including items which were read completely but whose converted
			// output had not yet been delivered.
			mParent->clearQueue();
		}
	}

	Queue::Queue()
	{
		mQueueHandle = 0;
		mInstance = 0;
		mReadIndex = 0;
		mWriteIndex = 0;
		mCount = 0;
		mAutoConvert = false;
		mPlayed = false;
		int i;
		for (i = 0; i < SOLOUD_QUEUE_MAX; i++)
		{
			mSource[i] = 0;
			mSourceRate[i] = 0;
			mSourceChannels[i] = 0;
		}
	}

	Queue::~Queue()
	{
		stop();
		clearQueue();
	}

	void Queue::clearQueue()
	{
		int i;
		for (i = 0; i < SOLOUD_QUEUE_MAX; i++)
		{
			if (mSource[i])
			{
				delete mSource[i];
				mSource[i] = 0;
			}
			mSourceRate[i] = 0;
			mSourceChannels[i] = 0;
		}
		mReadIndex = 0;
		mWriteIndex = 0;
		mCount = 0;
	}

	QueueInstance * Queue::createInstance()
	{
		if (mInstance)
		{
			stop();
			mInstance = 0;
		}
		mPlayed = true;
		mInstance = new QueueInstance(this);
		return mInstance;
	}

	void Queue::findQueueHandle()
	{
		// Find the channel the queue is playing on to calculate handle..
		int i;
		for (i = 0; mQueueHandle == 0 && i < (signed)mSoloud->mHighestVoice; i++)
		{
			if (mSoloud->mVoice[i] == mInstance)
			{
				mQueueHandle = mSoloud->getHandleFromVoice_internal(i);
			}
		}
	}

	result Queue::play(AudioSource &aSound)
	{
		if (!mSoloud)
		{
			return INVALID_PARAMETER;
		}

		findQueueHandle();

		if (mQueueHandle == 0)
			return INVALID_PARAMETER;

		if (mCount >= SOLOUD_QUEUE_MAX)
			return OUT_OF_MEMORY;

		if (mAutoConvert && !supportedFormat(aSound.mBaseSamplerate, aSound.mChannels))
			return INVALID_PARAMETER;

		if (!aSound.mAudioSourceID)
		{
			aSound.mAudioSourceID = mSoloud->mAudioSourceID;
			mSoloud->mAudioSourceID++;
		}

		SoLoud::AudioSourceInstance *instance = aSound.createInstance();

		if (instance == 0)
		{
			return OUT_OF_MEMORY;
		}
		instance->init(aSound, 0);
		instance->mAudioSourceID = aSound.mAudioSourceID;

		mSoloud->lockAudioMutex_internal();
		mSource[mWriteIndex] = instance;
		if (mAutoConvert)
		{
			mSourceRate[mWriteIndex] = (unsigned int)aSound.mBaseSamplerate;
			mSourceChannels[mWriteIndex] = aSound.mChannels;
		}
		else
		{
			mSourceRate[mWriteIndex] = 0;
			mSourceChannels[mWriteIndex] = 0;
		}
		mWriteIndex = (mWriteIndex + 1) % SOLOUD_QUEUE_MAX;
		mCount++;
		mSoloud->unlockAudioMutex_internal();

		return SO_NO_ERROR;
	}


	unsigned int Queue::getQueueCount()
	{
		if (!mSoloud)
		{
			return 0;
		}
		unsigned int count;
		mSoloud->lockAudioMutex_internal();
		count = mCount;
		mSoloud->unlockAudioMutex_internal();
		return count;
	}

	bool Queue::isCurrentlyPlaying(AudioSource &aSound)
	{
		if (mSoloud == 0 || mCount == 0 || aSound.mAudioSourceID == 0)
			return false;
		mSoloud->lockAudioMutex_internal();
		bool res = mSource[mReadIndex]->mAudioSourceID == aSound.mAudioSourceID;
		mSoloud->unlockAudioMutex_internal();
		return res;
	}

	result Queue::setParamsFromAudioSource(AudioSource &aSound)
	{
		if (mAutoConvert)
		{
			if (mPlayed)
				return INVALID_PARAMETER;
			if (!supportedFormat(aSound.mBaseSamplerate, aSound.mChannels))
				return INVALID_PARAMETER;
		}
		mChannels = aSound.mChannels;
		mBaseSamplerate = aSound.mBaseSamplerate;

		return SO_NO_ERROR;
	}

	result Queue::setParams(float aSamplerate, unsigned int aChannels)
	{
		if (aChannels < 1 || aChannels > MAX_CHANNELS)
			return INVALID_PARAMETER;
		if (mAutoConvert)
		{
			if (mPlayed)
				return INVALID_PARAMETER;
			if (!supportedFormat(aSamplerate, aChannels))
				return INVALID_PARAMETER;
		}
		mChannels = aChannels;
		mBaseSamplerate = aSamplerate;
		return SO_NO_ERROR;
	}

	result Queue::setAutoConvert(bool aEnable)
	{
		if (mCount != 0 || mInstance != 0)
			return INVALID_PARAMETER;
		if (aEnable && !supportedFormat(mBaseSamplerate, mChannels))
			return INVALID_PARAMETER;
		mAutoConvert = aEnable;
		return SO_NO_ERROR;
	}
};
