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

#include "./voxel.h"
#include "../context/context.h"
#include "../simd.h"
#include "../color.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Small helpers ---------- */

static PFboolean
pfiIsFinite3(const PFfloat v[3])
{
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static PFsizei
pfiVoxelLevelResolution(const PFIvoxelvolume *vol, PFsizei level)
{
    return vol->resolution >> level;
}

static PFfloat*
pfiVoxelTexel(const PFIvoxelvolume *vol, PFsizei level,
              PFsizei x, PFsizei y, PFsizei z)
{
    PFsizei r = pfiVoxelLevelResolution(vol, level);
    return vol->level[level] + ((x * r + y) * r + z) * 4;
}

/* ---------- Triangle / closed voxel cube intersection (SAT) ---------- */

static PFboolean
pfiAxisSeparates(const PFMvec3 axis, const PFMvec3 tri[3],
                 const PFMvec3 boxCenter, const PFMvec3 boxHalf)
{
    PFfloat axisLenSq = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
    if (axisLenSq < 1e-12f) {
        return PF_FALSE; // Degenerate axis, cannot separate anything
    }

    PFfloat t0 = tri[0][0] * axis[0] + tri[0][1] * axis[1] + tri[0][2] * axis[2];
    PFfloat t1 = tri[1][0] * axis[0] + tri[1][1] * axis[1] + tri[1][2] * axis[2];
    PFfloat t2 = tri[2][0] * axis[0] + tri[2][1] * axis[1] + tri[2][2] * axis[2];

    PFfloat tMin = PF_MIN(t0, PF_MIN(t1, t2));
    PFfloat tMax = PF_MAX(t0, PF_MAX(t1, t2));

    PFfloat c = boxCenter[0] * axis[0] + boxCenter[1] * axis[1] + boxCenter[2] * axis[2];
    PFfloat r = fabsf(boxHalf[0] * axis[0]) + fabsf(boxHalf[1] * axis[1]) + fabsf(boxHalf[2] * axis[2]);

    // Touching a face or an edge is an intersection, only a strict gap separates
    PFfloat tol = 1e-6f * PF_MAX(1.0f, PF_MAX(fabsf(tMin), fabsf(c + r)));
    return (tMin > c + r + tol) || (tMax < c - r - tol);
}

static PFboolean
pfiTriangleIntersectsVoxel(const PFMvec3 tri[3],
                           const PFfloat min[3], PFfloat h,
                           PFsizei i, PFsizei j, PFsizei k)
{
    PFMvec3 center = {
        min[0] + (i + 0.5f) * h,
        min[1] + (j + 0.5f) * h,
        min[2] + (k + 0.5f) * h
    };
    PFMvec3 half = { 0.5f * h, 0.5f * h, 0.5f * h };

    // The three face normals of the voxel cube
    const PFMvec3 axes[3] = {
        { 1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f }
    };
    for (int a = 0; a < 3; ++a) {
        if (pfiAxisSeparates(axes[a], tri, center, half)) return PF_FALSE;
    }

    // The triangle plane normal
    PFMvec3 e1, e2, normal;
    pfmVec3SubR(e1, tri[1], tri[0]);
    pfmVec3SubR(e2, tri[2], tri[0]);
    pfmVec3CrossR(normal, e1, e2);
    if (pfiAxisSeparates(normal, tri, center, half)) return PF_FALSE;

    // The nine edge cross products
    PFMvec3 e3;
    pfmVec3SubR(e3, tri[2], tri[1]);
    const PFMvec3 edges[3] = {
        { e1[0], e1[1], e1[2] },
        { e2[0], e2[1], e2[2] },
        { e3[0], e3[1], e3[2] }
    };

    for (int e = 0; e < 3; ++e) {
        for (int a = 0; a < 3; ++a) {
            PFMvec3 cross;
            pfmVec3CrossR(cross, edges[e], axes[a]);
            if (pfiAxisSeparates(cross, tri, center, half)) return PF_FALSE;
        }
    }

    return PF_TRUE;
}

void
pfiVoxelCaptureTriangle(PFIvoxelvolume *volume,
                        const PFMvec4 positions[3],
                        const PFMmat4 model,
                        const PFcolor emission)
{
    // Transform the submission to world space with the current model transform
    PFMvec3 tri[3];
    for (int i = 0; i < 3; ++i) {
        pfmVec3Transform(tri[i], positions[i], model);
    }

    PFfloat bMin[3] = { tri[0][0], tri[0][1], tri[0][2] };
    PFfloat bMax[3] = { bMin[0], bMin[1], bMin[2] };
    for (int i = 1; i < 3; ++i) {
        for (int a = 0; a < 3; ++a) {
            bMin[a] = PF_MIN(bMin[a], tri[i][a]);
            bMax[a] = PF_MAX(bMax[a], tri[i][a]);
        }
    }

    PFsizei res = volume->resolution;
    PFfloat h = volume->h;
    PFsizei lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        // A closed voxel cube touches the triangle AABB whenever
        // (i+1)*h >= bMin and i*h <= bMax, so a triangle lying exactly on a
        // cell face covers both adjacent cells. The SAT rejects extra candidates.
        PFfloat flo = floorf((bMin[a] - volume->min[a]) / h) - 1.0f;
        PFfloat fhi = floorf((bMax[a] - volume->min[a]) / h);
        PFsizei i0 = (flo < 0.0f) ? 0 : (PFsizei)flo;
        PFsizei i1 = (fhi < 0.0f) ? 0 : (PFsizei)fhi;
        lo[a] = PF_MIN(i0, res - 1);
        hi[a] = PF_MIN(i1, res - 1);
    }

    PFfloat er = emission.r * PF_INV_255;
    PFfloat eg = emission.g * PF_INV_255;
    PFfloat eb = emission.b * PF_INV_255;

    for (PFsizei x = lo[0]; x <= hi[0]; ++x) {
        for (PFsizei y = lo[1]; y <= hi[1]; ++y) {
            for (PFsizei z = lo[2]; z <= hi[2]; ++z) {
                if (!pfiTriangleIntersectsVoxel(tri, volume->min, h, x, y, z)) {
                    continue;
                }
                PFfloat *texel = pfiVoxelTexel(volume, 0, x, y, z);
                texel[3] = 1.0f; // Non emissive geometry still writes opacity
                texel[0] = PF_MAX(texel[0], er);
                texel[1] = PF_MAX(texel[1], eg);
                texel[2] = PF_MAX(texel[2], eb);
            }
        }
    }
}

/* ---------- Mip hierarchy ---------- */

static void
pfiVoxelBuildMips(PFIvoxelvolume *volume)
{
    for (PFsizei level = 1; level < volume->levels; ++level) {
        PFsizei r = pfiVoxelLevelResolution(volume, level);
        PFsizei prevR = r * 2;
        const PFfloat *prev = volume->level[level - 1];

        for (PFsizei x = 0; x < r; ++x) {
            for (PFsizei y = 0; y < r; ++y) {
                for (PFsizei z = 0; z < r; ++z) {
                    PFfloat acc[4] = { 0, 0, 0, 0 };
                    for (PFsizei dx = 0; dx < 2; ++dx) {
                        for (PFsizei dy = 0; dy < 2; ++dy) {
                            for (PFsizei dz = 0; dz < 2; ++dz) {
                                const PFfloat *child =
                                    prev + (((2 * x + dx) * prevR + (2 * y + dy)) * prevR + (2 * z + dz)) * 4;
                                for (int c = 0; c < 4; ++c) acc[c] += child[c];
                            }
                        }
                    }
                    PFfloat *out = pfiVoxelTexel(volume, level, x, y, z);
                    for (int c = 0; c < 4; ++c) out[c] = acc[c] * 0.125f;
                }
            }
        }
    }
}

/* ---------- Sampling ---------- */

static void
pfiVoxelSampleLevel(const PFIvoxelvolume *volume, PFsizei level,
                    const PFfloat position[3], PFfloat rgba[4])
{
    PFsizei r = pfiVoxelLevelResolution(volume, level);
    PFfloat hL = volume->side / (PFfloat)r;

    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;

    PFint base[3];
    PFfloat f[3];
    for (int a = 0; a < 3; ++a) {
        PFfloat u = (position[a] - volume->min[a]) / hL;          // Texel centers at half a cell
        PFfloat fi = floorf(u - 0.5f);
        base[a] = (PFint)fi; // Negative base indices are skipped below (out-of-volume texels are zero)
        f[a] = u - (fi + 0.5f);
    }

    for (int cx = 0; cx < 2; ++cx) {
        PFint ix = base[0] + cx;
        if (ix < 0 || ix >= (PFint)r) continue;
        PFfloat wx = cx ? f[0] : (1.0f - f[0]);
        for (int cy = 0; cy < 2; ++cy) {
            PFint iy = base[1] + cy;
            if (iy < 0 || iy >= (PFint)r) continue;
            PFfloat wxy = wx * (cy ? f[1] : (1.0f - f[1]));
            for (int cz = 0; cz < 2; ++cz) {
                PFint iz = base[2] + cz;
                if (iz < 0 || iz >= (PFint)r) continue;
                PFfloat w = wxy * (cz ? f[2] : (1.0f - f[2]));
                const PFfloat *texel = pfiVoxelTexel(volume, level,
                                                      (PFsizei)ix, (PFsizei)iy, (PFsizei)iz);
                for (int c = 0; c < 4; ++c) rgba[c] += w * texel[c];
            }
        }
    }
}

void
pfiVoxelSample(const PFIvoxelvolume *volume,
               const PFfloat position[3], PFfloat lod,
               PFfloat rgba[4])
{
    PFfloat maxLod = (PFfloat)(volume->levels - 1);
    if (lod < 0.0f) lod = 0.0f;
    if (lod > maxLod) lod = maxLod;

    PFsizei l0 = (PFsizei)lod;
    PFfloat t = lod - (PFfloat)l0;

    PFfloat a[4], b[4];
    pfiVoxelSampleLevel(volume, l0, position, a);

    if (t > 0.0f && l0 + 1 < volume->levels) {
        pfiVoxelSampleLevel(volume, l0 + 1, position, b);
        for (int c = 0; c < 4; ++c) rgba[c] = (1.0f - t) * a[c] + t * b[c];
    } else {
        for (int c = 0; c < 4; ++c) rgba[c] = a[c];
    }
}

/* ---------- Cone integration ---------- */

void
pfiVoxelTraceCone(const PFIvoxelvolume *volume,
                  const PFMvec3 origin, const PFMvec3 unitDirection,
                  PFfloat maxDistance, PFfloat rgb[3])
{
    const PFfloat h = volume->h;
    const PFfloat tanHalf = tanf(PFM_PI / 12.0f);
    const PFfloat maxLod = (PFfloat)(volume->levels - 1);

    PFfloat T = 1.0f;
    rgb[0] = rgb[1] = rgb[2] = 0.0f;

    PFfloat t = h;
    while (t < maxDistance) {
        PFMvec3 p;
        for (int a = 0; a < 3; ++a) {
            p[a] = origin[a] + t * unitDirection[a];
            if (p[a] < volume->min[a] || p[a] > volume->min[a] + volume->side) {
                return; // Sample point left the volume
            }
        }

        PFfloat d = PF_MAX(h, 2.0f * t * tanHalf);
        PFfloat lod = log2f(d / h);
        if (lod < 0.0f) lod = 0.0f;
        if (lod > maxLod) lod = maxLod;

        PFfloat step = PF_MAX(h, d * 0.5f);
        PFfloat remain = maxDistance - t;
        if (step > remain) step = remain;

        PFfloat rgba[4];
        pfiVoxelSample(volume, p, lod, rgba);
        PFfloat a = rgba[3];
        if (a > 0.0f) {
            PFfloat denom = h * exp2f(lod);
            PFfloat aCorrected = 1.0f - powf(1.0f - a, step / denom);
            PFfloat invA = 1.0f / a;
            rgb[0] += T * aCorrected * rgba[0] * invA;
            rgb[1] += T * aCorrected * rgba[1] * invA;
            rgb[2] += T * aCorrected * rgba[2] * invA;
            T *= (1.0f - aCorrected);
            if (T <= 1e-4f) return;
        }

        t += step;
    }
}

/* ---------- Per fragment five-cone GI ---------- */

PFboolean
pfiVoxelGIActive(void)
{
    PFIctx *ctx = G_currentCtx;
    if (ctx == NULL || ctx->boundVolume == NULL) return PF_FALSE;
    if (ctx->voxelStrength <= 0.0f) return PF_FALSE;
    if (!(ctx->state & PF_LIGHTING)) return PF_FALSE;
    if (ctx->shadingMode != PF_SMOOTH || ctx->lightingMode != PF_PHONG) return PF_FALSE;
    return ctx->boundVolume->complete;
}

void
pfiVoxelGIFragment(const PFMvec3 worldPos, const PFMvec3 worldN,
                   const PFfloat baseRGB[3], PFfloat outAdd[3])
{
    outAdd[0] = outAdd[1] = outAdd[2] = 0.0f;

    PFIctx *ctx = G_currentCtx;
    PFIvoxelvolume *vol = ctx->boundVolume;

    PFMvec3 N;
    PFfloat nLenSq = pfmVec3LengthSq(worldN);
    if (nLenSq <= 0.0f) return; // Zero length normal: no contribution
    pfmVec3Normalize(N, worldN);

    // U uses the coordinate axis having the smallest absolute dot product with N,
    // ties resolved in X, then Y, then Z order.
    const PFMvec3 axisX = { 1, 0, 0 };
    const PFMvec3 axisY = { 0, 1, 0 };
    const PFMvec3 axisZ = { 0, 0, 1 };
    PFfloat adX = fabsf(pfmVec3Dot(N, axisX));
    PFfloat adY = fabsf(pfmVec3Dot(N, axisY));
    PFfloat adZ = fabsf(pfmVec3Dot(N, axisZ));

    const PFMvec3 *axis = &axisX;
    if (adY < adX) axis = &axisY;
    if (adZ < fabsf(pfmVec3Dot(N, *axis))) axis = &axisZ;

    PFMvec3 U, V;
    pfmVec3CrossR(U, *axis, N);
    PFfloat uLenSq = pfmVec3LengthSq(U);
    if (uLenSq <= 0.0f) return;
    pfmVec3Normalize(U, U);
    pfmVec3CrossR(V, N, U);

    PFMvec3 dirs[5];
    pfmVec3Copy(dirs[0], N);
    PFfloat k = sqrtf(3.0f);
    for (int i = 0; i < 3; ++i) {
        dirs[1][i] = N[i] + k * U[i];
        dirs[2][i] = N[i] - k * U[i];
        dirs[3][i] = N[i] + k * V[i];
        dirs[4][i] = N[i] - k * V[i];
    }
    for (int i = 1; i < 5; ++i) {
        pfmVec3Normalize(dirs[i], dirs[i]);
    }

    PFMvec3 origin;
    for (int i = 0; i < 3; ++i) {
        origin[i] = worldPos[i] + 2.0f * vol->h * N[i];
    }

    PFfloat radiance[3] = { 0, 0, 0 };
    for (int c = 0; c < 5; ++c) {
        PFfloat cone[3];
        pfiVoxelTraceCone(vol, origin, dirs[c], ctx->voxelMaxDistance, cone);
        radiance[0] += cone[0];
        radiance[1] += cone[1];
        radiance[2] += cone[2];
    }

    PFfloat strength = ctx->voxelStrength * 0.2f; // Equal weight over the five cones
    outAdd[0] = radiance[0] * strength * baseRGB[0];
    outAdd[1] = radiance[1] * strength * baseRGB[1];
    outAdd[2] = radiance[2] * strength * baseRGB[2];
}

#if PF_SIMD_SUPPORT

/* Stores a SIMD float register into a small stack array to read one lane.
 * This keeps the extraction correct on SSE2 where pfiSimdExtract_F32 yields
 * the integer bit pattern rather than a float. */
static PFfloat
pfiVoxelExtractF(PFIsimdvf v, int lane)
{
    PFfloat values[PF_SIMD_SIZE];
    pfiSimdStore_F32(values, v);
    return values[lane];
}

void
pfiVoxelGIAdd_simd(PFIsimdv3f addOut, const PFIsimdv4f baseF,
                   const PFIsimdv3f posOverW, const PFIsimdv3f nrmOverW,
                   PFIsimdvf invW, PFIsimdvi mask)
{
    PFfloat outR[PF_SIMD_SIZE], outG[PF_SIMD_SIZE], outB[PF_SIMD_SIZE];

    for (int lane = 0; lane < PF_SIMD_SIZE; ++lane) {
        PFfloat ar = 0.0f, ag = 0.0f, ab = 0.0f;

        if (pfiSimdExtractVarIdx_I32(mask, lane) != 0) {
            PFfloat iw = pfiVoxelExtractF(invW, lane);
            if (iw > 0.0f) {
                PFMvec3 worldPos, worldN;
                for (int a = 0; a < 3; ++a) {
                    worldPos[a] = pfiVoxelExtractF(posOverW[a], lane) / iw;
                    worldN[a]   = pfiVoxelExtractF(nrmOverW[a], lane) / iw;
                }

                PFfloat base[3] = {
                    pfiVoxelExtractF(baseF[0], lane),
                    pfiVoxelExtractF(baseF[1], lane),
                    pfiVoxelExtractF(baseF[2], lane)
                };
                PFfloat add[3];
                pfiVoxelGIFragment(worldPos, worldN, base, add);
                ar = add[0];
                ag = add[1];
                ab = add[2];
            }
        }

        outR[lane] = ar;
        outG[lane] = ag;
        outB[lane] = ab;
    }

    addOut[0] = pfiSimdLoad_F32(outR);
    addOut[1] = pfiSimdLoad_F32(outG);
    addOut[2] = pfiSimdLoad_F32(outB);
}

#endif //PF_SIMD_SUPPORT

/* ---------- Public API ---------- */

PFvoxelvolume
pfGenVoxelVolume(const PFfloat min[3], PFfloat side, PFsizei resolution)
{
    PFIctx *ctx = G_currentCtx;
    if (ctx == NULL) return NULL;

    if (min == NULL || !pfiIsFinite3(min) || !isfinite(side) || side <= 0.0f) {
        ctx->errCode = PF_INVALID_VALUE;
        return NULL;
    }

    PFboolean pot = PF_FALSE;
    switch (resolution) {
        case 8: case 16: case 32: case 64: case 128: pot = PF_TRUE; break;
        default: break;
    }
    if (!pot) {
        ctx->errCode = PF_INVALID_VALUE;
        return NULL;
    }

    PFIvoxelvolume *vol = (PFIvoxelvolume*)PF_CALLOC(1, sizeof(PFIvoxelvolume));
    if (vol == NULL) {
        ctx->errCode = PF_ERROR_OUT_OF_MEMORY;
        return NULL;
    }

    PFsizei levels = 1;
    for (PFsizei r = resolution; r > 1; r >>= 1) ++levels;

    PFboolean failed = PF_FALSE;
    for (PFsizei level = 0; level < levels; ++level) {
        PFsizei r = resolution >> level;
        size_t count = (size_t)r * r * r * 4;
        vol->level[level] = (PFfloat*)PF_CALLOC(count, sizeof(PFfloat));
        if (vol->level[level] == NULL) {
            failed = PF_TRUE;
            break;
        }
    }

    if (failed) {
        for (PFsizei level = 0; level < PF_VOXEL_MAX_LEVELS; ++level) {
            if (vol->level[level]) PF_FREE(vol->level[level]);
        }
        PF_FREE(vol);
        ctx->errCode = PF_ERROR_OUT_OF_MEMORY;
        return NULL;
    }

    for (int a = 0; a < 3; ++a) vol->min[a] = min[a];
    vol->side = side;
    vol->h = side / (PFfloat)resolution;
    vol->resolution = resolution;
    vol->levels = levels;
    vol->capturing = PF_FALSE;
    vol->complete = PF_FALSE; // Sampling is illegal until the first capture ends
    vol->owner = ctx;

    vol->next = ctx->voxelVolumes;
    ctx->voxelVolumes = vol;

    return (PFvoxelvolume)vol;
}

void
pfDeleteVoxelVolume(PFvoxelvolume *handle)
{
    if (handle == NULL) return;
    PFIvoxelvolume *vol = (PFIvoxelvolume*)*handle;
    if (vol == NULL) return;

    PFIctx *ctx = G_currentCtx;
    if (ctx == NULL || vol->owner != ctx) {
        if (ctx) ctx->errCode = PF_INVALID_OPERATION;
        return;
    }
    if (vol->capturing) {
        ctx->errCode = PF_INVALID_OPERATION;
        return;
    }

    if (ctx->boundVolume == vol) ctx->boundVolume = NULL;
    if (ctx->captureVolume == vol) ctx->captureVolume = NULL;

    PFIvoxelvolume **node = &ctx->voxelVolumes;
    while (*node != NULL && *node != vol) node = &(*node)->next;
    if (*node == vol) *node = vol->next;

    for (PFsizei level = 0; level < PF_VOXEL_MAX_LEVELS; ++level) {
        if (vol->level[level]) PF_FREE(vol->level[level]);
    }
    PF_FREE(vol);
    *handle = NULL;
}

void
pfBeginVoxelVolume(PFvoxelvolume handle)
{
    PFIctx *ctx = G_currentCtx;
    if (ctx == NULL) return;

    PFIvoxelvolume *vol = (PFIvoxelvolume*)handle;
    if (vol == NULL) {
        ctx->errCode = PF_INVALID_VALUE;
        return;
    }
    if (vol->owner != ctx) {
        ctx->errCode = PF_INVALID_OPERATION;
        return;
    }
    if (ctx->captureVolume != NULL || vol->capturing) {
        ctx->errCode = PF_INVALID_OPERATION; // Only one active capture, no nesting
        return;
    }
    if (ctx->currentRenderList != NULL) {
        ctx->errCode = PF_INVALID_OPERATION; // Cannot start a capture while recording a list
        return;
    }

    PFsizei count = (size_t)vol->resolution * vol->resolution * vol->resolution * 4;
    memset(vol->level[0], 0, count * sizeof(PFfloat));

    // The capture must leave the caller's materials, texture and current
    // vertex attributes untouched; back them up in a dedicated slot so a
    // render list replay during the capture keeps its own backup intact.
    pfiMakeContextBackupTo(&ctx->captureBackup);

    vol->capturing = PF_TRUE;
    vol->complete = PF_FALSE;
    ctx->captureVolume = vol;
}

void
pfEndVoxelVolume(void)
{
    PFIctx *ctx = G_currentCtx;
    if (ctx == NULL) return;

    PFIvoxelvolume *vol = ctx->captureVolume;
    if (vol == NULL) {
        ctx->errCode = PF_INVALID_OPERATION;
        return;
    }

    pfiVoxelBuildMips(vol);
    vol->capturing = PF_FALSE;
    vol->complete = PF_TRUE;
    ctx->captureVolume = NULL;

    pfiRestoreContextFrom(&ctx->captureBackup);
}

void
pfBindVoxelVolume(PFvoxelvolume handle)
{
    PFIctx *ctx = G_currentCtx;
    if (ctx == NULL) return;

    if (handle == NULL) {
        ctx->boundVolume = NULL; // Binding NULL disables the contribution
        return;
    }

    PFIvoxelvolume *vol = (PFIvoxelvolume*)handle;
    if (vol->owner != ctx) {
        ctx->errCode = PF_INVALID_OPERATION;
        return;
    }
    if (vol->capturing || !vol->complete) {
        // Using an unfinished volume is invalid; the existing binding is kept
        ctx->errCode = PF_INVALID_OPERATION;
        return;
    }
    ctx->boundVolume = vol;
}

void
pfVoxelGI(PFfloat strength, PFfloat maxDistance)
{
    PFIctx *ctx = G_currentCtx;
    if (ctx == NULL) return;

    if (!isfinite(strength) || strength < 0.0f ||
        !isfinite(maxDistance) || maxDistance <= 0.0f) {
        ctx->errCode = PF_INVALID_VALUE;
        return;
    }

    ctx->voxelStrength = strength;
    ctx->voxelMaxDistance = maxDistance;
}

void
pfSampleVoxelVolume(PFvoxelvolume handle, const PFfloat position[3],
                    PFfloat lod, PFfloat rgba[4])
{
    PFIctx *ctx = G_currentCtx;
    if (ctx == NULL) return;

    PFIvoxelvolume *vol = (PFIvoxelvolume*)handle;
    if (vol == NULL || position == NULL || rgba == NULL) {
        ctx->errCode = PF_INVALID_VALUE;
        return;
    }
    if (vol->owner != ctx || vol->capturing || !vol->complete) {
        ctx->errCode = PF_INVALID_OPERATION;
        return;
    }
    if (!pfiIsFinite3(position) || !isfinite(lod)) {
        ctx->errCode = PF_INVALID_VALUE;
        return;
    }

    pfiVoxelSample(vol, position, lod, rgba);
}

void
pfTraceVoxelCone(PFvoxelvolume handle, const PFfloat origin[3],
                 const PFfloat direction[3], PFfloat maxDistance,
                 PFfloat rgb[3])
{
    PFIctx *ctx = G_currentCtx;
    if (ctx == NULL) return;

    PFIvoxelvolume *vol = (PFIvoxelvolume*)handle;
    if (vol == NULL || origin == NULL || direction == NULL || rgb == NULL) {
        ctx->errCode = PF_INVALID_VALUE;
        return;
    }
    if (vol->owner != ctx || vol->capturing || !vol->complete) {
        ctx->errCode = PF_INVALID_OPERATION;
        return;
    }
    if (!pfiIsFinite3(origin) || !pfiIsFinite3(direction) ||
        !isfinite(maxDistance) || maxDistance <= 0.0f) {
        ctx->errCode = PF_INVALID_VALUE;
        return;
    }

    PFfloat lenSq = direction[0] * direction[0]
                  + direction[1] * direction[1]
                  + direction[2] * direction[2];
    if (fabsf(lenSq - 1.0f) > 1e-4f) {
        ctx->errCode = PF_INVALID_VALUE;
        return;
    }

    PFMvec3 dir;
    PFfloat invLen = 1.0f / sqrtf(lenSq);
    dir[0] = direction[0] * invLen;
    dir[1] = direction[1] * invLen;
    dir[2] = direction[2] * invLen;

    pfiVoxelTraceCone(vol, origin, dir, maxDistance, rgb);
}
