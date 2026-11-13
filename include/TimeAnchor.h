////////////////////////////////////////////////////////////////////////////////
///
/// TimeAnchor : a single anchor point for the anchored time-stretch mode.
///
/// An anchor pairs an input frame position 'inputFrame' with the exact
/// output frame position 'outputFrame' where that input frame has to end up.
/// A list of anchors defines a finite, piecewise affine time mapping, and the
/// 'P = protectFrames' samples around every anchor are protected against any
/// WSOLA seeking, cross-fading or resampling: they are copied to the output
/// verbatim and exactly once.
///
/// See SoundTouch::setTimeAnchors() for the full list validity rules.
///
////////////////////////////////////////////////////////////////////////////////

#ifndef TimeAnchor_H
#define TimeAnchor_H

#include <stdint.h>

namespace soundtouch
{

struct TimeAnchor
{
    /// Frame position in the input stream (per channel; stereo interleaving
    /// does not change the counting).
    uint64_t inputFrame;

    /// Exact frame position where the above input frame must appear in the
    /// output stream.
    uint64_t outputFrame;
};

}

#endif  // TimeAnchor_H
