// Copyright (c) The HLSL2GLSLFork Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.txt file.

#ifndef HLSL_LOWER_DYNAMIC_SAMPLERS_H
#define HLSL_LOWER_DYNAMIC_SAMPLERS_H

struct TIntermNode;
class TInfoSink;

/// Rewrites dynamic indexing of global fixed-length sampler2D arrays used
/// directly as the sampler argument of a supported texture built-in
/// (tex2D, tex2Dbias, tex2Dlod and the four-argument gradient form of
/// tex2D).  Every dynamic sample is replaced by a call to a generated
/// helper that samples all array elements with constant indices and
/// selects the result, so the original uniform declaration and reflection
/// are preserved.
///
/// Any dynamic sampler use that cannot be lowered (escape into a function
/// argument, assignment, an unsupported texture built-in, a non-global or
/// non-fixed-length array, ...) is reported as an error carrying the
/// source location.  Returns true on success and false if any error was
/// reported; on failure the tree must not be used for code generation.
bool LowerDynamicSamplers(TIntermNode* root, TInfoSink& infoSink);

#endif //HLSL_LOWER_DYNAMIC_SAMPLERS_H
