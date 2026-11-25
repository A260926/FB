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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "soloud.h"
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#include "soloud_wavstream.h"
#include "soloud_file.h"
#include "stb_vorbis.h"

namespace SoLoud
{
	// Bidirectional playback constants. The window bounds the per-voice decoded
	// PCM; the fractional bits match the mixer's 20-bit fixed point step.
	static const unsigned int DIR_WINDOW_FRAMES = 2048;
	static const unsigned int DIR_FRAC_BITS = 20;
	static const unsigned long long DIR_FRAC_ONE = 1ULL << DIR_FRAC_BITS;

	size_t drflac_read_func(void* pUserData, void* pBufferOut, size_t bytesToRead)
	{
		File *fp = (File*)pUserData;
		return fp->read((unsigned char*)pBufferOut, (unsigned int)bytesToRead);
	}

	size_t drmp3_read_func(void* pUserData, void* pBufferOut, size_t bytesToRead)
	{
		File *fp = (File*)pUserData;
		return fp->read((unsigned char*)pBufferOut, (unsigned int)bytesToRead);
	}

	size_t drwav_read_func(void* pUserData, void* pBufferOut, size_t bytesToRead)
	{
		File *fp = (File*)pUserData;
		return fp->read((unsigned char*)pBufferOut, (unsigned int)bytesToRead);
	}

	drflac_bool32 drflac_seek_func(void* pUserData, int offset, drflac_seek_origin origin)
	{
		File *fp = (File*)pUserData;
		if (origin != drflac_seek_origin_start)
			offset += fp->pos();
		fp->seek(offset);
		return 1;
	}

	drmp3_bool32 drmp3_seek_func(void* pUserData, int offset, drmp3_seek_origin origin)
	{
		File *fp = (File*)pUserData;
		if (origin != drmp3_seek_origin_start)
			offset += fp->pos();
		fp->seek(offset);
		return 1;
	}

	drmp3_bool32 drwav_seek_func(void* pUserData, int offset, drwav_seek_origin origin)
	{
		File *fp = (File*)pUserData;
		if (origin != drwav_seek_origin_start)
			offset += fp->pos();
		fp->seek(offset);
		return 1;
	}

	WavStreamInstance::WavStreamInstance(WavStream *aParent)
	{
		mOggFrameSize = 0;
		mParent = aParent;
		mOffset = 0;
		mCodec.mOgg = 0;
		mCodec.mFlac = 0;
		mFile = 0;
		mDirEnabled = false;
		mDirDirection = 1;
		mDirXFixed = 0;
		mDirEnded = false;
		mDirFloat = false;
		mDirWindow = 0;
		mDirRawBuf = 0;
		mDirWindowStart = 0;
		mDirWindowFrames = 0;
		mDirSourceFrames = 0;
		if (aParent->mMemFile)
		{
			MemoryFile *mf = new MemoryFile();
			mFile = mf;
			mf->openMem(aParent->mMemFile->getMemPtr(), aParent->mMemFile->length(), false, false);
		}
		else
		if (aParent->mFilename)
		{
			DiskFile *df = new DiskFile;
			mFile = df;
			df->open(aParent->mFilename);
		}
		else
		if (aParent->mStreamFile)
		{
			mFile = aParent->mStreamFile;
			mFile->seek(0); // stb_vorbis assumes file offset to be at start of ogg
		}
		else
		{
			return;
		}
		
		if (mFile)
		{
			if (mParent->mFiletype == WAVSTREAM_WAV)
			{
				mCodec.mWav = new drwav;
				if (!drwav_init(mCodec.mWav, drwav_read_func, drwav_seek_func, (void*)mFile, NULL))
				{
					delete mCodec.mWav;
					mCodec.mWav = 0;
					if (mFile != mParent->mStreamFile)
						delete mFile;
					mFile = 0;
				}
			}
			else
			if (mParent->mFiletype == WAVSTREAM_OGG)
			{
				int e;

				mCodec.mOgg = stb_vorbis_open_file((Soloud_Filehack *)mFile, 0, &e, 0);

				if (!mCodec.mOgg)
				{
					if (mFile != mParent->mStreamFile)
						delete mFile;
					mFile = 0;
				}
				mOggFrameSize = 0;
				mOggFrameOffset = 0;
				mOggOutputs = 0;
			}
			else
			if (mParent->mFiletype == WAVSTREAM_FLAC)
			{
				mCodec.mFlac = drflac_open(drflac_read_func, drflac_seek_func, (void*)mFile, NULL);
				if (!mCodec.mFlac)
				{
					if (mFile != mParent->mStreamFile)
						delete mFile;
					mFile = 0;
				}
			}
			else
			if (mParent->mFiletype == WAVSTREAM_MP3)
			{
				mCodec.mMp3 = new drmp3;
				if (!drmp3_init(mCodec.mMp3, drmp3_read_func, drmp3_seek_func, (void*)mFile, NULL))
				{
					delete mCodec.mMp3;
					mCodec.mMp3 = 0;
					if (mFile != mParent->mStreamFile)
						delete mFile;
					mFile = 0;
				}
			}
			else
			{
				if (mFile != mParent->mStreamFile)
					delete mFile;
				mFile = NULL;
				return;
			}
		}
	}

	WavStreamInstance::~WavStreamInstance()
	{
		delete[] mDirWindow;
		delete[] mDirRawBuf;
		switch (mParent->mFiletype)
		{
		case WAVSTREAM_OGG:
			if (mCodec.mOgg)
			{
				stb_vorbis_close(mCodec.mOgg);
			}
			break;
		case WAVSTREAM_FLAC:
			if (mCodec.mFlac)
			{
				drflac_close(mCodec.mFlac);
			}
			break;
		case WAVSTREAM_MP3:
			if (mCodec.mMp3)
			{
				drmp3_uninit(mCodec.mMp3);
				delete mCodec.mMp3;
				mCodec.mMp3 = 0;
			}
			break;
		case WAVSTREAM_WAV:
			if (mCodec.mWav)
			{
				drwav_uninit(mCodec.mWav);
				delete mCodec.mWav;
				mCodec.mWav = 0;
			}
			break;
		}
		if (mFile != mParent->mStreamFile)
		{
			delete mFile;
		}
	}

	static int getOggData(float **aOggOutputs, float *aBuffer, int aSamples, int aPitch, int aFrameSize, int aFrameOffset, int aChannels)
	{			
		if (aFrameSize <= 0)
			return 0;

		int samples = aSamples;
		if (aFrameSize - aFrameOffset < samples)
		{
			samples = aFrameSize - aFrameOffset;
		}

		int i;
		for (i = 0; i < aChannels; i++)
		{
			memcpy(aBuffer + aPitch * i, aOggOutputs[i] + aFrameOffset, sizeof(float) * samples);
		}
		return samples;
	}

	

	unsigned int WavStreamInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{			
		unsigned int offset = 0;
		float tmp[512 * MAX_CHANNELS];
		if (mFile == NULL)
			return 0;
		switch (mParent->mFiletype)
		{
		case WAVSTREAM_FLAC:
			{
				unsigned int i, j, k;

				for (i = 0; i < aSamplesToRead; i += 512)
				{
					unsigned int blockSize = (aSamplesToRead - i) > 512 ? 512 : aSamplesToRead - i;
					offset += (unsigned int)drflac_read_pcm_frames_f32(mCodec.mFlac, blockSize, tmp);

					for (j = 0; j < blockSize; j++)
					{
						for (k = 0; k < mChannels; k++)
						{
							aBuffer[k * aSamplesToRead + i + j] = tmp[j * mCodec.mFlac->channels + k];
						}
					}
				}
				mOffset += offset;
				return offset;
			}
			break;
		case WAVSTREAM_MP3:
			{
				unsigned int i, j, k;

				for (i = 0; i < aSamplesToRead; i += 512)
				{
					unsigned int blockSize = (aSamplesToRead - i) > 512 ? 512 : aSamplesToRead - i;
					offset += (unsigned int)drmp3_read_pcm_frames_f32(mCodec.mMp3, blockSize, tmp);

					for (j = 0; j < blockSize; j++)
					{
						for (k = 0; k < mChannels; k++)
						{
							aBuffer[k * aSamplesToRead + i + j] = tmp[j * mCodec.mMp3->channels + k];
						}
					}
				}
				mOffset += offset;
				return offset;
			}
		break;
		case WAVSTREAM_OGG:
			{
				if (mOggFrameOffset < mOggFrameSize)
				{
					int b = getOggData(mOggOutputs, aBuffer, aSamplesToRead, aBufferSize, mOggFrameSize, mOggFrameOffset, mChannels);
					mOffset += b;
					offset += b;
					mOggFrameOffset += b;
				}

				while (offset < aSamplesToRead)
				{
					mOggFrameSize = stb_vorbis_get_frame_float(mCodec.mOgg, NULL, &mOggOutputs);
					mOggFrameOffset = 0;
					int b = getOggData(mOggOutputs, aBuffer + offset, aSamplesToRead - offset, aBufferSize, mOggFrameSize, mOggFrameOffset, mChannels);
					mOffset += b;
					offset += b;
					mOggFrameOffset += b;

					if (mOffset >= mParent->mSampleCount || b == 0)
					{
						mOffset += offset;
						return offset;
					}
				}
			}
			break;
		case WAVSTREAM_WAV:
			{
				if (mDirEnabled)
				{
					// Bidirectional voices are rendered through renderBidirectional() instead.
					return 0;
				}
				unsigned int i, j, k;

				for (i = 0; i < aSamplesToRead; i += 512)
				{
					unsigned int blockSize = (aSamplesToRead - i) > 512 ? 512 : aSamplesToRead - i;
					offset += (unsigned int)drwav_read_pcm_frames_f32(mCodec.mWav, blockSize, tmp);

					for (j = 0; j < blockSize; j++)
					{
						for (k = 0; k < mChannels; k++)
						{
							aBuffer[k * aSamplesToRead + i + j] = tmp[j * mCodec.mWav->channels + k];
						}
					}
				}
				mOffset += offset;
				return offset;
			}
			break;
		}
		return aSamplesToRead;
	}

	result WavStreamInstance::seek(double aSeconds, float* mScratch, unsigned int mScratchSize)
	{
		if (mDirEnabled)
		{
			// Bidirectional seek: material seconds, range [0, length). Direction and the
			// accumulated stream time are preserved; window and resampler leftovers dropped.
			double length = mParent->getLength();
			if (!(aSeconds >= 0.0 && aSeconds < length))
			{
				return INVALID_PARAMETER;
			}
			unsigned int frame = (unsigned int)floor(aSeconds * (double)mBaseSamplerate);
			if (frame >= mDirSourceFrames)
			{
				return INVALID_PARAMETER;
			}
			mDirXFixed = (signed long long)frame << DIR_FRAC_BITS;
			mDirEnded = false;
			mDirWindowFrames = 0;
			mSrcOffset = 0;
			mLeftoverSamples = 0;
			mOffset = frame;
			return SO_NO_ERROR;
		}
		if (mCodec.mOgg)
		{
			int pos = (int)floor(mBaseSamplerate * aSeconds);
			stb_vorbis_seek(mCodec.mOgg, pos);
			// Since the position that we just sought to might not be *exactly*
			// the position we asked for, we're re-calculating the position just
			// for the sake of correctness.
			mOffset = stb_vorbis_get_sample_offset(mCodec.mOgg);
			double newPosition = float(mOffset / mBaseSamplerate);
			mStreamPosition = newPosition;
			return 0;
		}
		else {
			return AudioSourceInstance::seek(aSeconds, mScratch, mScratchSize);
		}
	}

	result WavStreamInstance::rewind()
	{
		switch (mParent->mFiletype)
		{
		case WAVSTREAM_OGG:
			if (mCodec.mOgg)
			{
				stb_vorbis_seek_start(mCodec.mOgg);
			}
			break;
		case WAVSTREAM_FLAC:
			if (mCodec.mFlac)
			{
				drflac_seek_to_pcm_frame(mCodec.mFlac, 0);
			}
			break;
		case WAVSTREAM_MP3:
			if (mCodec.mMp3)
			{
				drmp3_seek_to_pcm_frame(mCodec.mMp3, 0);
			}
			break;
		case WAVSTREAM_WAV:
			if (mCodec.mWav)
			{
				drwav_seek_to_pcm_frame(mCodec.mWav, 0);
			}
			break;
		}
		mOffset = 0;
		mStreamPosition = 0.0f;
		return 0;
	}

	bool WavStreamInstance::hasEnded()
	{
		if (mDirEnabled)
		{
			return mDirEnded;
		}
		if (mOffset >= mParent->mSampleCount)
		{
			return 1;
		}
		return 0;
	}

	WavStream::WavStream()
	{
		mFilename = 0;
		mSampleCount = 0;
		mFiletype = WAVSTREAM_WAV;
		mMemFile = 0;
		mStreamFile = 0;
	}
	
	WavStream::~WavStream()
	{
		stop();
		delete[] mFilename;
		delete mMemFile;
	}
	
#define MAKEDWORD(a,b,c,d) (((d) << 24) | ((c) << 16) | ((b) << 8) | (a))

	result WavStream::loadwav(File * fp)
	{
		fp->seek(0);
		drwav decoder;

		if (!drwav_init(&decoder, drwav_read_func, drwav_seek_func, (void*)fp, NULL))
			return FILE_LOAD_FAILED;

		mChannels = decoder.channels;
		if (mChannels > MAX_CHANNELS)
		{
			mChannels = MAX_CHANNELS;
		}

		mBaseSamplerate = (float)decoder.sampleRate;
		mSampleCount = (unsigned int)decoder.totalPCMFrameCount;
		mFiletype = WAVSTREAM_WAV;
		drwav_uninit(&decoder);

		return SO_NO_ERROR;
	}

	result WavStream::loadogg(File * fp)
	{
		fp->seek(0);
		int e;
		stb_vorbis *v;
		v = stb_vorbis_open_file((Soloud_Filehack *)fp, 0, &e, 0);
		if (v == NULL)
			return FILE_LOAD_FAILED;
		stb_vorbis_info info = stb_vorbis_get_info(v);
		mChannels = info.channels;
		if (info.channels > MAX_CHANNELS)
		{
			mChannels = MAX_CHANNELS;
		}
		mBaseSamplerate = (float)info.sample_rate;
		int samples = stb_vorbis_stream_length_in_samples(v);
		stb_vorbis_close(v);
		mFiletype = WAVSTREAM_OGG;

		mSampleCount = samples;

		return 0;
	}

	result WavStream::loadflac(File * fp)
	{
		fp->seek(0);
		drflac* decoder = drflac_open(drflac_read_func, drflac_seek_func, (void*)fp, NULL);

		if (decoder == NULL)
			return FILE_LOAD_FAILED;
		
		mChannels = decoder->channels;
		if (mChannels > MAX_CHANNELS)
		{
			mChannels = MAX_CHANNELS;
		}

		mBaseSamplerate = (float)decoder->sampleRate;
		mSampleCount = (unsigned int)decoder->totalPCMFrameCount;
		mFiletype = WAVSTREAM_FLAC;
		drflac_close(decoder);

		return SO_NO_ERROR;
	}

	result WavStream::loadmp3(File * fp)
	{
		fp->seek(0);
		drmp3 decoder;
		if (!drmp3_init(&decoder, drmp3_read_func, drmp3_seek_func, (void*)fp, NULL))
			return FILE_LOAD_FAILED;


		mChannels = decoder.channels;
		if (mChannels > MAX_CHANNELS)
		{
			mChannels = MAX_CHANNELS;
		}

		drmp3_uint64 samples = drmp3_get_pcm_frame_count(&decoder);

		mBaseSamplerate = (float)decoder.sampleRate;
		mSampleCount = (unsigned int)samples;
		mFiletype = WAVSTREAM_MP3;
		drmp3_uninit(&decoder);

		return SO_NO_ERROR;
	}

	result WavStream::load(const char *aFilename)
	{
		delete[] mFilename;
		delete mMemFile;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;
		DiskFile fp;
		int res = fp.open(aFilename);
		if (res != SO_NO_ERROR)
			return res;
		
		int len = (int)strlen(aFilename);
		mFilename = new char[len+1];		
		memcpy(mFilename, aFilename, len);
		mFilename[len] = 0;
		
		res = parse(&fp);

		if (res != SO_NO_ERROR)
		{
			delete[] mFilename;
			mFilename = 0;
			return res;
		}

		return 0;
	}

	result WavStream::loadMem(const unsigned char *aData, unsigned int aDataLen, bool aCopy, bool aTakeOwnership)
	{
		delete[] mFilename;
		delete mMemFile;
		mStreamFile = 0;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;

		if (aData == NULL || aDataLen == 0)
			return INVALID_PARAMETER;

		MemoryFile *mf = new MemoryFile();
		int res = mf->openMem(aData, aDataLen, aCopy, aTakeOwnership);
		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		res = parse(mf);

		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		mMemFile = mf;

		return 0;
	}

	result WavStream::loadToMem(const char *aFilename)
	{
		DiskFile df;
		int res = df.open(aFilename);
		if (res == SO_NO_ERROR)
		{
			res = loadFileToMem(&df);
		}
		return res;
	}

	result WavStream::loadFile(File *aFile)
	{
		delete[] mFilename;
		delete mMemFile;
		mStreamFile = 0;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;

		int res = parse(aFile);

		if (res != SO_NO_ERROR)
		{
			return res;
		}

		mStreamFile = aFile;

		return 0;
	}

	result WavStream::loadFileToMem(File *aFile)
	{
		delete[] mFilename;
		delete mMemFile;
		mStreamFile = 0;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;

		MemoryFile *mf = new MemoryFile();
		int res = mf->openFileToMem(aFile);
		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		res = parse(mf);

		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		mMemFile = mf;

		return res;
	}


	result WavStream::parse(File *aFile)
	{
		int tag = aFile->read32();
		int res = SO_NO_ERROR;
		if (tag == MAKEDWORD('O', 'g', 'g', 'S'))
		{
			res = loadogg(aFile);
		}
		else
		if (tag == MAKEDWORD('R', 'I', 'F', 'F'))
		{
			res = loadwav(aFile);
		}
		else
		if (tag == MAKEDWORD('f', 'L', 'a', 'C'))
		{
			res = loadflac(aFile);
		}
		else
		if (loadmp3(aFile) == SO_NO_ERROR)
		{
			res = SO_NO_ERROR;
		}
		else
		{
			res = FILE_LOAD_FAILED;
		}
		return res;
	}

	AudioSourceInstance *WavStream::createInstance()
	{
		return new WavStreamInstance(this);
	}

	double WavStream::getLength()
	{
		if (mBaseSamplerate == 0)
			return 0;
		return mSampleCount / mBaseSamplerate;
	}

	// ---------------------------------------------------------------------
	// Bidirectional (forward / reverse) playback
	//
	// The voice keeps a material-space playhead x (next output frame's source
	// coordinate, in 20-bit fixed point). A fixed window of at most
	// DIR_WINDOW_FRAMES source frames is decoded from disk; window refills seek
	// the file at most once per crossing, so continuous reverse playback never
	// thrashes the file per output frame.
	// ---------------------------------------------------------------------

	bool WavStreamInstance::dirCheckSupport() const
	{
		if (mParent->mFiletype != WAVSTREAM_WAV || mFile == 0 || mCodec.mWav == 0)
			return false;
		if (mChannels < 1 || mChannels > 2)
			return false;
		if (mFlags & (AudioSourceInstance::LOOPING | AudioSourceInstance::PROCESS_3D))
			return false;
		for (unsigned int f = 0; f < FILTERS_PER_STREAM; f++)
		{
			if (mFilter[f])
				return false;
		}
		if (mBusHandle != 0)
			return false;
		if (mParent->mSampleCount == 0 || mCodec.mWav->totalPCMFrameCount == 0)
			return false;
		if (mCodec.mWav->translatedFormatTag == DR_WAVE_FORMAT_PCM && mCodec.mWav->bitsPerSample == 16)
			return true;
		if (mCodec.mWav->translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT && mCodec.mWav->bitsPerSample == 32)
			return true;
		return false;
	}

	bool WavStreamInstance::isBidirectional() const
	{
		return mDirEnabled;
	}

	bool WavStreamInstance::supportsWavStreamDirection() const
	{
		return dirCheckSupport();
	}

	result WavStreamInstance::enableWavStreamDirection()
	{
		if (mDirEnabled)
			return SO_NO_ERROR;
		if (!dirCheckSupport())
			return INVALID_PARAMETER;

		mDirFloat = (mCodec.mWav->translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT);
		mDirSourceFrames = (unsigned int)mCodec.mWav->totalPCMFrameCount;
		// Take over the playhead from the position actually delivered by the
		// legacy resampler, never from the decoder's read-ahead position.
		mDirXFixed = (signed long long)mDeliveredSrcPosFixed;
		mDirDirection = 1;
		mDirEnded = (mDirXFixed < 0 ||
			(unsigned long long)mDirXFixed >= (unsigned long long)mDirSourceFrames * DIR_FRAC_ONE);
		mDirWindowStart = 0;
		mDirWindowFrames = 0;

		unsigned int bytesPerSample = mDirFloat ? (unsigned int)sizeof(float) : (unsigned int)sizeof(short);
		mDirWindow = new float[DIR_WINDOW_FRAMES * mChannels];
		mDirRawBuf = new unsigned char[DIR_WINDOW_FRAMES * mChannels * bytesPerSample];
		if (!mDirWindow || !mDirRawBuf)
		{
			delete[] mDirWindow;
			delete[] mDirRawBuf;
			mDirWindow = 0;
			mDirRawBuf = 0;
			return OUT_OF_MEMORY;
		}

		// The legacy decode window and resampler leftovers are obsolete now.
		mSrcOffset = 0;
		mLeftoverSamples = 0;
		mDirEnabled = true;
		return SO_NO_ERROR;
	}

	result WavStreamInstance::setWavStreamDirectionInternal(int aDirection)
	{
		if (!mDirEnabled)
			return INVALID_PARAMETER;
		if (aDirection != 1 && aDirection != -1)
			return INVALID_PARAMETER;
		// x is kept untouched, including its fractional part; setting the same
		// direction again leaves the state identical.
		mDirDirection = aDirection;
		return SO_NO_ERROR;
	}

	bool WavStreamInstance::dirFillWindow(unsigned int aFrameStart)
	{
		if (aFrameStart >= mDirSourceFrames)
			return false;
		unsigned int frames = mDirSourceFrames - aFrameStart;
		if (frames > DIR_WINDOW_FRAMES)
			frames = DIR_WINDOW_FRAMES;

		if (!drwav_seek_to_pcm_frame(mCodec.mWav, aFrameStart))
			return false;
		unsigned int read = (unsigned int)drwav_read_pcm_frames(mCodec.mWav, frames, mDirRawBuf);
		if (read != frames)
			return false;

		for (unsigned int c = 0; c < mChannels; c++)
		{
			float *dst = mDirWindow + c * DIR_WINDOW_FRAMES;
			for (unsigned int f = 0; f < frames; f++)
			{
				unsigned int idx = f * mChannels + c;
				if (mDirFloat)
					dst[f] = ((const float *)mDirRawBuf)[idx];
				else
					dst[f] = (float)(((const short *)mDirRawBuf)[idx]) / 32768.0f;
			}
		}
		mDirWindowStart = aFrameStart;
		mDirWindowFrames = frames;
		return true;
	}

	void WavStreamInstance::dirEnsureWindow(unsigned int aFrameLo, unsigned int aFrameHi)
	{
		if (mDirWindowFrames > 0 &&
			aFrameLo >= mDirWindowStart &&
			aFrameHi < mDirWindowStart + mDirWindowFrames)
		{
			return;
		}
		// Place the fixed window in the direction of travel so it always holds the
		// interpolation right neighbour (lo+1) and the playhead can walk a full
		// window (up to 2048 source frames) before the next refill. Continuous
		// reverse playback therefore never seeks the file per output frame, and the
		// per-voice decoded PCM stays bounded to one 2048-frame window.
		unsigned int start;
		if (mDirDirection < 0)
		{
			// Window ends at the high coordinate and extends backward.
			start = (aFrameHi + 1 >= DIR_WINDOW_FRAMES) ? aFrameHi + 1 - DIR_WINDOW_FRAMES : 0;
		}
		else
		{
			// Window starts at the low coordinate and extends forward.
			start = aFrameLo;
			if (start + DIR_WINDOW_FRAMES > mDirSourceFrames)
			{
				start = mDirSourceFrames > DIR_WINDOW_FRAMES ? mDirSourceFrames - DIR_WINDOW_FRAMES : 0;
			}
		}
		dirFillWindow(start);
	}

	float WavStreamInstance::dirSample(unsigned int aChannel, unsigned int aFrame) const
	{
		return mDirWindow[aChannel * DIR_WINDOW_FRAMES + (aFrame - mDirWindowStart)];
	}

	unsigned int WavStreamInstance::renderBidirectional(float *aScratch, unsigned int aBufferSize, unsigned int aSamplesToRead,
		unsigned int aOutOffset, unsigned int aStepFixed, unsigned int aResampler)
	{
		for (unsigned int c = 0; c < mChannels; c++)
		{
			memset(aScratch + c * aBufferSize + aOutOffset, 0,
				sizeof(float) * (aSamplesToRead - aOutOffset));
		}
		if (!mDirEnabled || aStepFixed == 0)
			return 0;

		const unsigned long long endFixed = (unsigned long long)mDirSourceFrames * DIR_FRAC_ONE;
		unsigned int sounded = 0;

		for (unsigned int i = aOutOffset; i < aSamplesToRead; i++)
		{
			// Sample first, then advance by direction * h.
			if (mDirXFixed >= 0 && (unsigned long long)mDirXFixed < endFixed)
			{
				unsigned int lo = (unsigned int)((unsigned long long)mDirXFixed >> DIR_FRAC_BITS);
				unsigned int frac = (unsigned int)((unsigned long long)mDirXFixed & (DIR_FRAC_ONE - 1));
				unsigned int hi = lo + 1;
				dirEnsureWindow(lo, hi < mDirSourceFrames ? hi : lo);

				for (unsigned int c = 0; c < mChannels; c++)
				{
					float v;
					if (aResampler == Soloud::RESAMPLER_POINT)
					{
						v = dirSample(c, lo);
					}
					else
					{
						// LINEAR (and any other resampler): interpolate in material
						// space; the last frame copies its right neighbour.
						float s0 = dirSample(c, lo);
						float s1 = (hi < mDirSourceFrames) ? dirSample(c, hi) : s0;
						v = s0 + (s1 - s0) * ((float)frac / (float)DIR_FRAC_ONE);
					}
					aScratch[c * aBufferSize + i] = v;
				}
				sounded++;
			}

			// The fractional part always belongs to the material coordinate; only
			// the sign of the step changes when reversing.
			mDirXFixed += (signed long long)mDirDirection * (signed long long)aStepFixed;
		}

		mDirEnded = !(mDirXFixed >= 0 && (unsigned long long)mDirXFixed < endFixed);
		return sounded;
	}

	time WavStreamInstance::getBidirectionalPosition() const
	{
		if (!mDirEnabled || mBaseSamplerate == 0.0f)
			return mStreamPosition;
		unsigned long long endFixed = (unsigned long long)mDirSourceFrames * DIR_FRAC_ONE;
		double x;
		if (mDirXFixed <= 0)
		{
			x = 0.0;
		}
		else if ((unsigned long long)mDirXFixed >= endFixed)
		{
			// Clamped to the material's end endpoint once x walks out of range.
			x = (double)mDirSourceFrames;
		}
		else
		{
			x = (double)(unsigned long long)mDirXFixed / (double)DIR_FRAC_ONE;
		}
		return x / (double)mBaseSamplerate;
	}
};
