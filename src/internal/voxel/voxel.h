/**
 *  Copyright (c) 2024 Le Juez Victor
 *
 *  This software is provided "as-is", without any express or implied warranty. In no event
 *  will the authors be held liable for any damages arising from the use of this software.
 *
 *  Permission is granted to anyone to use this software for any purpose, including commercial
 *  applications, and to alter it and redistribute it freely, subject to the following restrictions:
 *
 *  1. The origin of this software must not be misrepresented; you must not claim that you
 *  wrote the original software. If you use this software in a product, an acknowledgment
 *  in the product documentation would be appreciated but is not required.
 *
 *  2. Altered source versions must be plainly marked as such, and must not be misrepresented
 *  as being the original software.
 *
 *   3. This notice may not be removed or altered from any source distribution.
 */

#ifndef PF_INTERNAL_VOXEL_H
#define PF_INTERNAL_VOXEL_H

#include "../../pixelforge.h"
#include "../../pfm.h"

/*
    Voxel cone tracing global illumination.
    A voxel volume stores premultiplied RGBA (float) at several LODs,
    the finest level having 'resolution' voxels per axis.
*/

#define PF_VOXEL_MAX_LEVELS 8

/**
 * @brief Per-vertex data used to recover world position and world normal
 *        at a fragment with reciprocal clip-w interpolation.
 */
typedef struct {
    PFMvec3 posOverW;       ///< World position divided by clip w
    PFMvec3 nrmOverW;       ///< World normal (inverse transpose model) divided by clip w
    PFfloat invW;           ///< Reciprocal of clip w
} PFIgiVertex;

typedef struct PFIvoxelvolume PFIvoxelvolume;

struct PFIvoxelvolume {
    void                    *owner;         ///< Owning context (PFIctx*, stored opaquely)
    PFIvoxelvolume          *next;          ///< Next volume owned by the same context
    PFfloat                 min[3];         ///< Minimal corner of the cubic volume
    PFfloat                 side;           ///< Edge length of the volume
    PFfloat                 h;              ///< Base voxel edge length (side / resolution)
    PFsizei                 resolution;     ///< Finest level resolution (power of two)
    PFsizei                 levels;         ///< Number of LOD levels (log2(resolution) + 1)
    PFboolean               capturing;      ///< A Begin/End capture is currently in progress
    PFboolean               complete;       ///< All mip levels are up to date, sampling is allowed
    PFfloat                 *level[PF_VOXEL_MAX_LEVELS]; ///< RGBA premultiplied grids, level 0 is the finest
};

/**
 * @brief Rasterizes a world-space triangle into the volume currently being captured.
 *
 * @param volume    Volume receiving the triangle.
 * @param positions Model-space triangle vertices (w = 1).
 * @param model     Current model transformation (identity when no model stack is used).
 * @param emission  Emission color of the material bound for the submission.
 */
void
pfiVoxelCaptureTriangle(PFIvoxelvolume *volume,
                        const PFMvec4 positions[3],
                        const PFMmat4 model,
                        const PFcolor emission);

/**
 * @brief Samples the volume at a world position with trilinear filtering and LOD blending.
 *        Out-of-volume texels evaluate to zero. The volume must be complete.
 */
void
pfiVoxelSample(const PFIvoxelvolume *volume,
               const PFfloat position[3], PFfloat lod,
               PFfloat rgba[4]);

/**
 * @brief Integrates a single cone (half angle pi/12) using the shared stepping logic.
 */
void
pfiVoxelTraceCone(const PFIvoxelvolume *volume,
                  const PFMvec3 origin, const PFMvec3 unitDirection,
                  PFfloat maxDistance, PFfloat rgb[3]);

/**
 * @brief Tells whether the voxel GI contribution must be evaluated for the
 *        fragments of the triangle currently being rasterized.
 */
PFboolean
pfiVoxelGIActive(void);

/**
 * @brief Evaluates the five-cone voxel GI additive contribution for one fragment.
 *        Returns zero (without touching the output) when the normal has zero length.
 *
 * @param worldPos  Interpolated fragment world position.
 * @param worldN    Interpolated fragment world normal (normalized internally).
 * @param baseRGB   Diffuse base color actually entering the existing lighting, in [0,1].
 * @param outAdd    Additive RGB radiance, already scaled by the base color and strength.
 */
void
pfiVoxelGIFragment(const PFMvec3 worldPos, const PFMvec3 worldN,
                   const PFfloat baseRGB[3], PFfloat outAdd[3]);

#if PF_SIMD_SUPPORT
/**
 * @brief SIMD variant of pfiVoxelGIFragment applied lane by lane.
 *        'baseF' holds the diffuse base color (in [0,1]) that enters the existing
 *        lighting; interpolated world quantities are perspective-correct values
 *        divided by clip w. Only lanes enabled by 'mask' produce a non-zero add.
 *        The additive RGB (already scaled by base color and strength) is written
 *        to 'addOut'; alpha is never produced.
 */
void
pfiVoxelGIAdd_simd(PFIsimdv3f addOut, const PFIsimdv4f baseF,
                   const PFIsimdv3f posOverW, const PFIsimdv3f nrmOverW,
                   PFIsimdvf invW, PFIsimdvi mask);
#endif //PF_SIMD_SUPPORT

#endif //PF_INTERNAL_VOXEL_H
