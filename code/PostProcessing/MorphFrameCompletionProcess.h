/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the following
conditions are met:

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
---------------------------------------------------------------------------
*/

/** @file Defines a post processing step to complete missing normal and
    tangent frames of morph targets (aiAnimMesh).*/
#ifndef AI_MORPHFRAMECOMPLETIONPROCESS_H_INC
#define AI_MORPHFRAMECOMPLETIONPROCESS_H_INC

#include "Common/BaseProcess.h"

struct aiMesh;
struct aiAnimMesh;

namespace Assimp {

// ---------------------------------------------------------------------------
/** The MorphFrameCompletionProcess completes missing vertex normals and
 *  missing tangent/bitangent frames of morph targets (aiAnimMesh) so that
 *  every target carries the same per-vertex streams as its host mesh.
 *
 *  The step is optional: it only runs when the importer property
 *  AI_CONFIG_PP_MORPH_FRAME_COMPLETION is set to true. Even then it only
 *  acts on the streams selected by the aiProcess_GenSmoothNormals and
 *  aiProcess_CalcTangentSpace flags, and it never touches the base mesh
 *  streams itself - those are still produced by GenVertexNormalsProcess
 *  and CalcTangentsProcess under their original rules.
 *
 *  The smoothing kernels are shared with those two steps, but the spatial
 *  neighborhood is always rebuilt from the positions of the target that
 *  is being completed, never from the base pose. Frames that are already
 *  present on a target (artist-authored or generated) are kept untouched,
 *  and every completed array is owned by its own target.
 */
class ASSIMP_API_WINONLY MorphFrameCompletionProcess final : public BaseProcess {
public:
    MorphFrameCompletionProcess();
    ~MorphFrameCompletionProcess() override = default;

    // -------------------------------------------------------------------
    /** Returns whether the processing step is present in the given flag.
     * The step hooks into the GenSmoothNormals/CalcTangentSpace flags; the
     * AI_CONFIG_PP_MORPH_FRAME_COMPLETION property decides whether Execute()
     * actually completes the morph target frames.
     * @param pFlags The processing flags the importer was called with.
     *   A bitwise combination of #aiPostProcessSteps.
     * @return true if the process is present in this flag fields,
     *   false if not.
     */
    bool IsActive(unsigned int pFlags) const override;

    // -------------------------------------------------------------------
    /** Called prior to ExecuteOnScene().
     * The function is a request to the process to update its configuration
     * basing on the Importer's configuration property list.
     */
    void SetupProperties(const Importer *pImp) override;

    // -------------------------------------------------------------------
    /** Executes the post processing step on the given imported data.
     * @param pScene The imported data to work at.
     */
    void Execute(aiScene *pScene) override;

protected:
    // -------------------------------------------------------------------
    /** Generates smooth vertex normals for a single morph target.
     * Mirrors GenVertexNormalsProcess::GenMeshVertexNormals, but evaluates
     * the faces against the target's own positions and stores the result
     * on the target. Does nothing if the target already has normals.
     * @param pMesh Host mesh, provides faces and fallback streams.
     * @param pAnimMesh The morph target to complete.
     * @return true if normals have been generated.
     */
    bool GenAnimMeshNormals(aiMesh *pMesh, aiAnimMesh *pAnimMesh);

    // -------------------------------------------------------------------
    /** Generates tangents and bitangents for a single morph target.
     * Mirrors CalcTangentsProcess::ProcessMesh, but evaluates the target's
     * positions, the target's normals (inherited from the base mesh when
     * absent) and the base mesh's first UV channel. Does nothing if the
     * target already has tangents or bitangents.
     * @param pMesh Host mesh, provides faces and fallback streams.
     * @param pAnimMesh The morph target to complete.
     * @return true if tangents and bitangents have been generated.
     */
    bool ProcessAnimMeshTangents(aiMesh *pMesh, aiAnimMesh *pAnimMesh);

private:
    /** Configuration option: maximum smoothing angle for normals, in radians */
    ai_real configNormalsMaxAngle;
    /** Configuration option: maximum smoothing angle for tangents, in radians */
    ai_real configTangentsMaxAngle;
    /** Configuration option: AI_CONFIG_PP_MORPH_FRAME_COMPLETION */
    bool configEnabled;
    mutable bool genNormalsRequested_ = false;
    mutable bool calcTangentsRequested_ = false;
    mutable bool flippedWindingOrder_ = false;
    mutable bool leftHanded_ = false;
};

} // end of namespace Assimp

#endif // AI_MORPHFRAMECOMPLETIONPROCESS_H_INC
