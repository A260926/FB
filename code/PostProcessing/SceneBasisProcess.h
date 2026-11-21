/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
copyright notice, this list of conditions and the
following disclaimer.

* Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the
following disclaimer in the documentation and/or other
materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
contributors may be used to endorse or promote products
derived from this software without specific prior
written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

/** @file  SceneBasisProcess.h
 *  @brief Re-expresses an entire scene in a different local coordinate
 *         basis.  See Assimp::ChangeSceneBasis().
 */
#pragma once
#ifndef INCLUDED_AI_SCENE_BASIS_PROCESS_H
#define INCLUDED_AI_SCENE_BASIS_PROCESS_H

#include <assimp/defs.h>
#include <assimp/matrix4x4.h>

struct aiScene;

namespace Assimp {

// ---------------------------------------------------------------------------
/** @brief Change the local coordinate basis of a complete scene.
 *
 *  The scene is conjugated by @p basis: every original semantic node keeps its
 *  world transform up to the similarity
 *
 *      G'(t) = basis * G(t) * inverse(basis)
 *
 *  at any animation time, mesh positions become @p basis * p and bone offset
 *  matrices become basis * O * inverse(basis).  Consequently world space
 *  vertices (both rigidly transformed and linear-blend-skinned) are the
 *  original ones multiplied by @p basis on the left.
 *
 *  @p basis may contain translation, non-uniform scaling, shear and
 *  reflection.  It must be a finite, invertible affine matrix.  The function
 *  either succeeds completely or throws Assimp::DeadlyImportError without
 *  modifying the scene.  Allocation failures are not rolled back.
 *
 *  @param scene Scene to modify in place.  Cameras, lights and aiSkeleton
 *         arrays are not supported and are rejected.
 *  @param basis The column-vector basis matrix B.
 *  @throws DeadlyImportError if the scene or the basis is unsupported/invalid.
 */
ASSIMP_API void ChangeSceneBasis( aiScene &scene, const aiMatrix4x4 &basis );

} // namespace Assimp

#endif // INCLUDED_AI_SCENE_BASIS_PROCESS_H
