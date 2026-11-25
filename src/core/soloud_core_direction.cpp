/*
SoLoud audio engine
Copyright (c) 2013-2020 Jari Komppa

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

#include "soloud_internal.h"

// Bidirectional WavStream transport control.

namespace SoLoud
{
	result Soloud::setWavStreamDirection(handle aVoiceHandle, int aDirection)
	{
		// Only the two real transport directions are accepted.
		if (aDirection != 1 && aDirection != -1)
		{
			return INVALID_PARAMETER;
		}

		// Voice groups never address a single bidirectional voice.
		if ((aVoiceHandle & 0xfffff000) == 0xfffff000)
		{
			return INVALID_PARAMETER;
		}

		lockAudioMutex_internal();

		int ch = getVoiceFromHandle_internal(aVoiceHandle);
		if (ch == -1)
		{
			unlockAudioMutex_internal();
			return INVALID_PARAMETER;
		}

		AudioSourceInstance *voice = mVoice[ch];

		if (voice->isBidirectional())
		{
			// Already bidirectional: x is preserved, only the sign of the step changes.
			result res = voice->setWavStreamDirectionInternal(aDirection);
			unlockAudioMutex_internal();
			return res;
		}

		// The first successful call enables the bidirectional state. Unsupported
		// sources or configurations are rejected and keep playing the legacy path.
		if (!voice->supportsWavStreamDirection())
		{
			unlockAudioMutex_internal();
			return INVALID_PARAMETER;
		}

		result res = voice->enableWavStreamDirection();
		if (res != SO_NO_ERROR)
		{
			unlockAudioMutex_internal();
			return res;
		}

		res = voice->setWavStreamDirectionInternal(aDirection);
		unlockAudioMutex_internal();
		return res;
	}
};
