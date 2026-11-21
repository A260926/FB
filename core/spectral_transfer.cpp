
/*
    pbrt source code Copyright(c) 1998-2010 Matt Pharr and Greg Humphreys.

    This file is part of pbrt.

    pbrt is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 */

// spectral_transfer.cpp*
#include "spectral_transfer.h"

// SpectralTransfer Method Definitions
bool SpectrumIsFinite(const Spectrum &s) {
	for (int i = 0; i < COLOR_SAMPLES; ++i)
		if (isnan(s[i]) || isinf(s[i])) return false;
	return true;
}
