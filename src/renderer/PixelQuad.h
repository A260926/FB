/*
MIT License

Copyright (c) 2017-2020 Markus Trenkwalder

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

/** @file */

#include <cstdint>
#include <stdexcept>

#include "PixelData.h"

namespace swr {

#pragma warning(push)
#pragma warning(disable: 26495) // variable in uninitialized

/// A 2x2 group of pixels handed to a quad pixel shader.
/** Lane order is:
  - 0: top-left
  - 1: top-right
  - 2: bottom-left
  - 3: bottom-right

  The quad origin is the even integer screen coordinate of the top-left
  pixel. Every lane, including helpers, carries data extrapolated from the
  plane of the same original input triangle. */
struct PixelQuad {
	PixelData lanes[4]; ///< The four lanes (top-left, top-right, bottom-left, bottom-right).

	/// Bit mask of the lanes that are actually covered by the original
	/// triangle and pass the scissor test. All other lanes are helpers.
	unsigned coverageMask;

	/// Zero based index of the original input triangle inside the draw call.
	uint64_t primitiveIndex;
};

#pragma warning(pop)

/// Fine screen x derivative. Right minus left, evaluated on the lane's own row.
/** The two lanes of a row always return the same value. */
inline float quadDdxFine(const float values[4], unsigned lane)
{
	if (lane < 2)
		return values[1] - values[0];
	return values[3] - values[2];
}

/// Fine screen y derivative. Bottom minus top, evaluated on the lane's own column.
/** The two lanes of a column always return the same value. */
inline float quadDdyFine(const float values[4], unsigned lane)
{
	if (lane == 0 || lane == 2)
		return values[2] - values[0];
	return values[3] - values[1];
}

/// Coarse screen x derivative. Every lane returns the top-left row difference.
inline float quadDdxCoarse(const float values[4], unsigned /*lane*/)
{
	return values[1] - values[0];
}

/// Coarse screen y derivative. Every lane returns the top-left column difference.
inline float quadDdyCoarse(const float values[4], unsigned /*lane*/)
{
	return values[2] - values[0];
}

} // end namespace swr
