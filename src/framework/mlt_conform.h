/**
 * \file mlt_conform.h
 * \brief frame-rate and audio-rate conforming producer
 *
 * Copyright (C) 2026 Meltytech, LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef MLT_CONFORM_H
#define MLT_CONFORM_H

#include "mlt_export.h"
#include "mlt_producer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create a producer that presents a source at a different frame rate and
 * audio sample rate.
 *
 * The returned producer is an ordinary producer and can be placed in a
 * playlist or tractor. The output profile is copied, the source producer
 * reference is retained, and the source's own profile, speed, and in/out
 * points are never changed.
 *
 * Only plain sources (not a cut, in == 0, out == length - 1, speed == 1) that
 * are not edited after construction are supported. The source length and
 * frame rate must be positive, as must both sample rates and the channel
 * count. Lengths and allocations that cannot be represented safely are
 * rejected.
 *
 * Video output frame n maps to source frame
 * floor(n * source_fps / output_fps) and keeps output position n. The output
 * length is ceil(source_length * output_fps / source_fps).
 *
 * Audio is produced as interleaved f32le through a lazy frame callback. The
 * global output sample k maps to source sample position
 * k * source_audio_rate / output_audio_rate and is linearly interpolated
 * between the two adjacent source samples. Samples past the end of the source
 * material, including an interpolation neighbor, are zero.
 *
 * \public \memberof mlt_producer_s
 * \param output_profile the output profile (copied)
 * \param source the source producer
 * \param source_audio_rate the source sample rate in Hz
 * \param output_audio_rate the output sample rate in Hz
 * \param channels the number of interleaved audio channels
 * \return a new producer or NULL on failure
 */
MLT_EXPORT mlt_producer mlt_producer_conform(mlt_profile output_profile,
                                            mlt_producer source,
                                            int source_audio_rate,
                                            int output_audio_rate,
                                            int channels);

#ifdef __cplusplus
}
#endif

#endif
