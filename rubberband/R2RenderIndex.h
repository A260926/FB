/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Rubber Band Library
    An audio time-stretching and pitch-shifting library.
    Copyright 2007-2024 Particular Programs Ltd.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.

    Alternatively, if you have a valid commercial licence for the
    Rubber Band Library obtained by agreement with the copyright
    holders, you may redistribute and/or modify it under the terms
    described in that licence.
*/

#ifndef RUBBERBAND_R2RENDERINDEX_H
#define RUBBERBAND_R2RENDERINDEX_H

#include <cstddef>
#include <iosfwd>
#include <map>
#include <memory>

namespace RubberBand
{

/**
 * Offline, serialisable render index for the R2 (OptionEngineFaster)
 * time stretcher.
 *
 * An index is built once per source track by analysing the complete
 * source material.  It stores the full hop schedule derived from the
 * key-frame map and sparsely stored synthesis boundary state, so that
 * any number of independent workers can render arbitrary output
 * ranges -- out of order, repeatedly, or in parallel -- and the
 * concatenated results are bit-for-bit (within floating point
 * rounding) identical to processing the whole track sequentially
 * through R2 from the start.
 *
 * The supported configuration is fixed: built-in FFT, R2 engine,
 * OptionThreadingNever, OptionPhaseIndependent,
 * OptionTransientsCrisp, standard analysis/synthesis windows,
 * time-domain smoothing off, pitch scale 1.0, 48 kHz, one or two
 * channels.
 *
 * The index is immutable once built, holds no reader, context, PCM,
 * or pointer to any analysis object, and is safe to share between
 * threads rendering with their own readers and output buffers.
 */
class R2RenderIndex
{
public:
    /**
     * Reader callback.  Writes the half-open absolute source frame
     * range [first, first+count), for every channel of the source,
     * into the planar output buffers (output[c][0..count-1]).
     * Coordinates are never negative and never extend past the end of
     * the source.  Return false on failure; the render call then
     * fails and its destination buffers are left untouched.
     */
    typedef bool (*ReadAt)(void *context,
                          size_t first,
                          size_t count,
                          float *const *output);

    virtual ~R2RenderIndex() { }

    /**
     * Analyse the complete source material and build a render index.
     *
     * \param sampleRate source sample rate (must be 48000)
     * \param channels   channel count (must be 1 or 2)
     * \param inputFrames total number of source frames
     * \param timeRatio  nominal time ratio; per-segment slopes of the
     *                   key-frame map must lie in [0.5, 2]
     * \param stride     output-frame spacing of the stored boundary
     *                   states, in [4096, 65536]
     * \param keyFrames  strictly increasing source/output key frames,
     *                   beginning at (0,0) and ending at
     *                   (inputFrames, llround(inputFrames*timeRatio))
     * \param reader     source reader used during analysis
     * \param context    opaque context passed to the reader
     *
     * Returns a null pointer if the configuration is invalid or the
     * reader fails.  The analysis pass performs forward FFTs only; it
     * never performs inverse FFTs or synthesises PCM.
     */
    static std::shared_ptr<const R2RenderIndex>
    build(size_t sampleRate,
          size_t channels,
          size_t inputFrames,
          double timeRatio,
          size_t stride,
          const std::map<size_t, size_t> &keyFrames,
          ReadAt reader,
          void *context);

    /** Number of source frames described by this index. */
    virtual size_t getInputFrames() const = 0;

    /** Number of output frames produced for the whole track. */
    virtual size_t getOutputFrames() const = 0;

    /**
     * Write the index as a self-contained binary stream.  Throws
     * std::runtime_error if the stream cannot be written.  The stream
     * is flushed before return.
     */
    virtual void save(std::ostream &os) const = 0;

    /**
     * Read an index previously written by save().  Returns a null
     * pointer on any truncation, version/config/length
     * inconsistency, or stream failure.  The format only needs to be
     * compatible within the same build and architecture.
     */
    static std::shared_ptr<const R2RenderIndex>
    load(std::istream &is);

    /**
     * Render the half-open output range [first, first+count) into the
     * planar output buffers, using reader/context to fetch source
     * frames.  On success exactly count frames are written to every
     * channel.  Returns false (without touching the destination
     * buffers) if the range is out of bounds, the reader is null, or
     * a reader call fails.  A legal zero-length request succeeds
     * without reading any source or writing any output.
     */
    virtual bool render(size_t first,
                        size_t count,
                        ReadAt reader,
                        void *context,
                        float *const *output) const = 0;
};

}

#endif
