/*
 * Copyright (C) 2012, Tomas Davidovic (http://www.davidovic.cz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * (The above is MIT License: http://en.wikipedia.org/wiki/MIT_License)
 */

#ifndef __VERTEXCM_HXX__
#define __VERTEXCM_HXX__

#include <vector>
#include <cmath>
#include <cassert>
#include "renderer.hxx"
#include "bsdf.hxx"
#include "rng.hxx"
#include "hashgrid.hxx"
#include "lighttree.hxx"

////////////////////////////////////////////////////////////////////////////////
// A NOTE ON PATH MIS WEIGHT EVALUATION
////////////////////////////////////////////////////////////////////////////////
//
// We compute path MIS weights iteratively as we trace the light and eye
// sub-paths. We cache three floating points quantities at each sub-path vertex:
//
//   dVCM  dVC  dVM
//
// These quantities represent partial weights associated with the sub-path. When
// we connect or merge one vertex to another, we use these quantities to quickly
// evaluate the MIS weight for the full path we have constructed. This scheme
// is presented in the technical report
//
//   "Implementing Vertex Connection and Merging"
//   http://www.iliyan.com/publications/ImplementingVCM
//
// The MIS code in the VertexCM class references the corresponding equations in
// the report in the form
//
//   [tech. rep. (##)]
//
// where ## is the equation number.
//
////////////////////////////////////////////////////////////////////////////////
// NON-UNIFORM LIGHT SELECTION (light tree)
////////////////////////////////////////////////////////////////////////////////
//
// Light sub-path emission always uses the uniform discrete probability
// qE(light) = 1/N.  The optional static light tree only changes the direct
// illumination selection probability qD(x, light), which depends on the
// receiving point x.  Of all bidirectional sampling strategies, exactly ONE
// per full path uses qD: the next-event-estimation (NEE) technique that
// connects the first camera surface vertex directly to the emitter.  Every
// other strategy (light emission, vertex connection, merging, BSDF hitting a
// light, escaping to the background, light tracing) keeps using qE = 1/N.
//
// Because the original recurrence assumes the NEE light pick equals the
// emission pick (both 1/N) and lets them cancel, we additionally propagate an
// isolated NEE-only partial weight (mNeeD*) on light vertices: it follows the
// same recurrence as dVCM/dVC/dVM but with every newer-technique birth
// constant removed.  At a strategy site the relative density of the NEE
// competitor differs from the uniform assumption by
//
//   r(x, light) = qD(x, light) / qE(light) = N * qD(x, light),
//
// so the balance-heuristic denominator gains an additive (r - 1) * neeTerm.
// With the tree disabled r == 1 everywhere and every result is bit-identical
// to the original uniform implementation.

class VertexCM : public AbstractRenderer
{
    // Grants the transport unit tests read-only access to the real MIS state
    // (partial weights, NEE monomials, light IDs) so they can validate the
    // production recurrence against an independent enumeration.
    friend struct VertexMisAdapter;

    // The sole point of this structure is to make carrying around the ray baggage easier.
    struct SubPathState
    {
        Vec3f mOrigin;             // Path origin
        Vec3f mDirection;          // Where to go next
        Vec3f mThroughput;         // Path throughput
        uint  mPathLength    : 30; // Number of path segments, including this
        uint  mIsFiniteLight :  1; // Just generate by finite light
        uint  mSpecularPath  :  1; // All scattering events so far were specular

        float dVCM; // MIS quantity used for vertex connection and merging
        float dVC;  // MIS quantity used for vertex connection
        float dVM;  // MIS quantity used for vertex merging

        // NEE-only counterparts (light sub-paths; kept zero on camera paths).
        float mNeeDVCM;
        float mNeeDVC;
        float mNeeDVM;

        // Original light-table ID of the light sub-path root; -1 on camera
        // sub-paths.
        int   mLightID;

        // Constant r = qD(x1, light)/qE(light) of the competing s = 1 (NEE)
        // technique.  In that technique the eye sub-path reaches x1, the first
        // light-side surface vertex (adjacent to the emitter x0), and samples
        // the emitter from there.  x1 is fixed for the whole light sub-path,
        // hence the ratio does not depend on the later connection/merge
        // boundary.  Equals 1 with the tree disabled.
        float mR;
    };

    // Path vertex, used for merging and connection
    template<bool tFromLight>
    struct PathVertex
    {
        Vec3f mHitpoint;   // Position of the vertex
        Vec3f mThroughput; // Path throughput (including emission)
        uint  mPathLength; // Number of segments between source and vertex

        // Stores all required local information, including incoming direction.
        BSDF<tFromLight> mBsdf;

        float dVCM; // MIS quantity used for vertex connection and merging
        float dVC;  // MIS quantity used for vertex connection
        float dVM;  // MIS quantity used for vertex merging

        // NEE-only partial weights (light vertices; zero for camera vertices).
        float mNeeDVCM;
        float mNeeDVC;
        float mNeeDVM;

        // Original light-table ID this light sub-path was emitted from.
        int   mLightID;

        // Per-light-subpath constant r = qD(x1, light)/qE(light).
        float mR;

        // Used by HashGrid
        const Vec3f& GetPosition() const
        {
            return mHitpoint;
        }
    };

    typedef PathVertex<false> CameraVertex;
    typedef PathVertex<true>  LightVertex;

    typedef BSDF<false>       CameraBSDF;
    typedef BSDF<true>        LightBSDF;

    // Range query used for PPM, BPT, and VCM. When HashGrid finds a vertex
    // within range -- Process() is called and vertex
    // merging is performed. BSDF of the camera vertex is used.
    class RangeQuery
    {
    public:

        RangeQuery(
            const VertexCM     &aVertexCM,
            const Vec3f        &aCameraPosition,
            const CameraBSDF   &aCameraBsdf,
            const SubPathState &aCameraState
        ) :
            mVertexCM(aVertexCM),
            mCameraPosition(aCameraPosition),
            mCameraBsdf(aCameraBsdf),
            mCameraState(aCameraState),
            mContrib(0)
        {}

        const Vec3f& GetPosition() const { return mCameraPosition; }

        const Vec3f& GetContrib() const { return mContrib; }

        void Process(const LightVertex& aLightVertex)
        {
            // Reject if full path length below/above min/max path length
            if((aLightVertex.mPathLength + mCameraState.mPathLength > mVertexCM.mMaxPathLength) ||
               (aLightVertex.mPathLength + mCameraState.mPathLength < mVertexCM.mMinPathLength))
                 return;

            // Retrieve light incoming direction in world coordinates
            const Vec3f lightDirection = aLightVertex.mBsdf.WorldDirFix();

            float cosCamera, cameraBsdfDirPdfW, cameraBsdfRevPdfW;
            const Vec3f cameraBsdfFactor = mCameraBsdf.Evaluate(
                mVertexCM.mScene, lightDirection, cosCamera, &cameraBsdfDirPdfW,
                &cameraBsdfRevPdfW);

            if(cameraBsdfFactor.IsZero())
                return;

            cameraBsdfDirPdfW *= mCameraBsdf.ContinuationProb();

            // Even though this is pdf from camera BSDF, the continuation probability
            // must come from light BSDF, because that would govern it if light path
            // actually continued
            cameraBsdfRevPdfW *= aLightVertex.mBsdf.ContinuationProb();

            // Partial light sub-path MIS weight [tech. rep. (38)]
            float wLight = aLightVertex.dVCM * mVertexCM.mMisVcWeightFactor +
                aLightVertex.dVM * mVertexCM.Mis(cameraBsdfDirPdfW);

            // The NEE competitor's relative density is scaled by its
            // per-light-subpath ratio.  Skipped with the tree off to preserve
            // the exact original arithmetic.
            if(mVertexCM.mUseTree)
            {
                const float neeTerm = aLightVertex.mNeeDVCM *
                    mVertexCM.mMisVcWeightFactor +
                    aLightVertex.mNeeDVM * mVertexCM.Mis(cameraBsdfDirPdfW);
                wLight += (aLightVertex.mR - 1.f) * neeTerm;
            }

            // Partial eye sub-path MIS weight [tech. rep. (39)]
            const float wCamera = mCameraState.dVCM * mVertexCM.mMisVcWeightFactor +
                mCameraState.dVM * mVertexCM.Mis(cameraBsdfRevPdfW);

            // Full path MIS weight [tech. rep. (37)]. No MIS for PPM
            const float misWeight = mVertexCM.mPpm ?
                1.f :
                1.f / (wLight + 1.f + wCamera);

            mContrib += misWeight * cameraBsdfFactor * aLightVertex.mThroughput;
        }

    private:

        const VertexCM     &mVertexCM;
        const Vec3f        &mCameraPosition;
        const CameraBSDF   &mCameraBsdf;
        const SubPathState &mCameraState;
        Vec3f              mContrib;
    };

public:

    enum AlgorithmType
    {
        // light vertices contribute to camera,
        // No MIS weights (dVCM, dVM, dVC all ignored)
        kLightTrace = 0,

        // Camera and light vertices merged on first non-specular surface from camera.
        // Cannot handle mixed specular + non-specular materials.
        // No MIS weights (dVCM, dVM, dVC all ignored)
        kPpm,

        // Camera and light vertices merged on along full path.
        // dVCM and dVM used for MIS
        kBpm,

        // Standard bidirectional path tracing
        // dVCM and dVC used for MIS
        kBpt,

        // Vertex connection and mering
        // dVCM, dVM, and dVC used for MIS
        kVcm
    };

public:

    VertexCM(
        const Scene&  aScene,
        AlgorithmType aAlgorithm,
        const float   aRadiusFactor,
        const float   aRadiusAlpha,
        int           aSeed = 1234
    ) :
        AbstractRenderer(aScene),
        mUseVM(false),
        mUseVC(false),
        mLightTraceOnly(false),
        mPpm(false),
        mUseTree(false),
        mUniformPickProb(0.f),
        mRng(aSeed)
    {
        switch(aAlgorithm)
        {
        case kLightTrace:
            mLightTraceOnly = true;
            break;
        case kPpm:
            mPpm   = true;
            mUseVM = true;
            break;
        case kBpm:
            mUseVM = true;
            break;
        case kBpt:
            mUseVC = true;
            break;
        case kVcm:
            mUseVC = true;
            mUseVM = true;
            break;
        default:
            printf("Unknown algorithm requested\n");
            break;
        }

        if(mPpm)
        {
            // We will check the scene to make sure it does not contain mixed
            // specular and non-specular materials
            for(int i = 0; i < mScene.GetMaterialCount(); ++i)
            {
                const Material &mat = mScene.GetMaterial(i);

                const bool hasNonSpecular =
                    (mat.mDiffuseReflectance.Max() > 0) ||
                    (mat.mPhongReflectance.Max() > 0);

                const bool hasSpecular =
                    (mat.mMirrorReflectance.Max() > 0) ||
                    (mat.mIOR > 0);

                if(hasNonSpecular && hasSpecular)
                {
                    printf(
                        "*WARNING* Our PPM implementation cannot handle materials mixing\n"
                        "Specular and NonSpecular BSDFs. The extension would be\n"
                        "fairly straightforward. In SampleScattering for camera sub-paths\n"
                        "limit the considered events to Specular only.\n"
                        "Merging will use non-specular components, scattering will be specular.\n"
                        "If there is no specular component, the ray will terminate.\n\n");

                    printf("We are now switching from *PPM* to *BPM*, which can handle the scene\n\n");

                    mPpm = false;
                    break;
                }
            }
        }

        mBaseRadius  = aRadiusFactor * mScene.mSceneSphere.mSceneRadius;
        mRadiusAlpha = aRadiusAlpha;
    }

    virtual void RunIteration(int aIteration)
    {
        // While we have the same number of pixels (camera paths)
        // and light paths, we do keep them separate for clarity reasons
        const int resX = int(mScene.mCamera.mResolution.x);
        const int resY = int(mScene.mCamera.mResolution.y);
        const int pathCount = resX * resY;
        mScreenPixelCount = float(resX * resY);
        mLightSubPathCount   = float(resX * resY);

        // Light sub-paths are always emitted uniformly (qE = 1/N).  The tree
        // only changes the direct-light selection probability qD(x, light).
        mUseTree = mScene.UseLightTree();
        mUniformPickProb = mScene.GetLightCount() > 0 ?
            1.f / float(mScene.GetLightCount()) : 0.f;

        // Setup our radius, 1st iteration has aIteration == 0, thus offset
        float radius = mBaseRadius;
        radius /= std::pow(float(aIteration + 1), 0.5f * (1 - mRadiusAlpha));
        // Purely for numeric stability
        radius = std::max(radius, 1e-7f);
        const float radiusSqr = Sqr(radius);

        // Factor used to normalise vertex merging contribution.
        // We divide the summed up energy by disk radius and number of light paths
        mVmNormalization = 1.f / (radiusSqr * PI_F * mLightSubPathCount);

        // MIS weight constant [tech. rep. (20)], with n_VC = 1 and n_VM = mLightPathCount
        const float etaVCM = (PI_F * radiusSqr) * mLightSubPathCount;
        mMisVmWeightFactor = mUseVM ? Mis(etaVCM)       : 0.f;
        mMisVcWeightFactor = mUseVC ? Mis(1.f / etaVCM) : 0.f;

        // Clear path ends, nothing ends anywhere
        mPathEnds.resize(pathCount);
        memset(&mPathEnds[0], 0, mPathEnds.size() * sizeof(int));

        // Remove all light vertices and reserve space for some
        mLightVertices.reserve(pathCount);
        mLightVertices.clear();

        //////////////////////////////////////////////////////////////////////////
        // Generate light paths
        //////////////////////////////////////////////////////////////////////////
        for(int pathIdx = 0; pathIdx < pathCount; pathIdx++)
        {
            SubPathState lightState;
            GenerateLightSample(lightState);

            //////////////////////////////////////////////////////////////////////////
            // Trace light path
            for(;; ++lightState.mPathLength)
            {
                // Offset ray origin instead of setting tmin due to numeric
                // issues in ray-sphere intersection. The isect.dist has to be
                // extended by this EPS_RAY after hit point is determined
                Ray ray(lightState.mOrigin + lightState.mDirection * EPS_RAY,
                    lightState.mDirection, 0);
                Isect isect(1e36f);

                if(!mScene.Intersect(ray, isect))
                    break;

                const Vec3f hitPoint = ray.org + ray.dir * isect.dist;
                isect.dist += EPS_RAY;

                LightBSDF bsdf(ray, isect, mScene);
                if(!bsdf.IsValid())
                    break;

                // Update the MIS quantities before storing them at the vertex.
                // These updates follow the initialization in GenerateLightSample() or
                // SampleScattering(), and together implement equations [tech. rep. (31)-(33)]
                // or [tech. rep. (34)-(36)], respectively.
                {
                    // Infinite lights use MIS handled via solid angle integration,
                    // so do not divide by the distance for such lights [tech. rep. Section 5.1]
                    if(lightState.mPathLength > 1 || lightState.mIsFiniteLight == 1)
                    {
                        lightState.dVCM     *= Mis(Sqr(isect.dist));
                        lightState.mNeeDVCM *= Mis(Sqr(isect.dist));
                    }

                    const float absCos = Mis(std::abs(bsdf.CosThetaFix()));
                    lightState.dVCM     /= absCos;
                    lightState.dVC      /= absCos;
                    lightState.dVM      /= absCos;
                    lightState.mNeeDVCM /= absCos;
                    lightState.mNeeDVC  /= absCos;
                    lightState.mNeeDVM  /= absCos;
                }

                // At the first real surface x1 after the emitter, fix the
                // constant r = qD(x1, light)/qE(light) of every competing
                // s = 1 (NEE) technique built from this light sub-path.  qE
                // stays uniform 1/N; only direct light selection uses qD.
                if(lightState.mPathLength == 1)
                    lightState.mR = LightPickRatio(hitPoint, lightState.mLightID);

                // Store vertex, unless BSDF is purely specular, which prevents
                // vertex connections and merging
                if(!bsdf.IsDelta() && (mUseVC || mUseVM))
                {
                    LightVertex lightVertex;
                    lightVertex.mHitpoint   = hitPoint;
                    lightVertex.mThroughput = lightState.mThroughput;
                    lightVertex.mPathLength = lightState.mPathLength;
                    lightVertex.mBsdf       = bsdf;

                    lightVertex.dVCM = lightState.dVCM;
                    lightVertex.dVC  = lightState.dVC;
                    lightVertex.dVM  = lightState.dVM;

                    lightVertex.mNeeDVCM = lightState.mNeeDVCM;
                    lightVertex.mNeeDVC  = lightState.mNeeDVC;
                    lightVertex.mNeeDVM  = lightState.mNeeDVM;
                    lightVertex.mLightID = lightState.mLightID;
                    lightVertex.mR       = lightState.mR;

                    mLightVertices.push_back(lightVertex);
                }

                // Connect to camera, unless BSDF is purely specular
                if(!bsdf.IsDelta() && (mUseVC || mLightTraceOnly))
                {
                    if(lightState.mPathLength + 1 >= mMinPathLength)
                        ConnectToCamera(lightState, hitPoint, bsdf);
                }

                // Terminate if the path would become too long after scattering
                if(lightState.mPathLength + 2 > mMaxPathLength)
                    break;

                // Continue random walk
                if(!SampleScattering(bsdf, hitPoint, lightState))
                    break;
            }

            mPathEnds[pathIdx] = (int)mLightVertices.size();
        }

        //////////////////////////////////////////////////////////////////////////
        // Build hash grid
        //////////////////////////////////////////////////////////////////////////

        // Only build grid when merging (VCM, BPM, and PPM)
        if(mUseVM)
        {
            // The number of cells is somewhat arbitrary, but seems to work ok
            mHashGrid.Reserve(pathCount);
            mHashGrid.Build(mLightVertices, radius);
        }

        //////////////////////////////////////////////////////////////////////////
        // Generate camera paths
        //////////////////////////////////////////////////////////////////////////

        // Unless rendering with traditional light tracing
        for(int pathIdx = 0; (pathIdx < pathCount) && (!mLightTraceOnly); ++pathIdx)
        {
            SubPathState cameraState;
            const Vec2f screenSample = GenerateCameraSample(pathIdx, cameraState);
            Vec3f color(0);

            // Immediately preceding camera surface vertex.  When a BSDF-sampled
            // ray hits a light or escapes to the background, the competing
            // next-event-estimation technique would have sampled that light
            // from this very point, so its qD must be evaluated here.
            Vec3f prevPoint(0.f);
            bool  hasPrevPoint = false;

            //////////////////////////////////////////////////////////////////////
            // Trace camera path
            for(;; ++cameraState.mPathLength)
            {
                // Offset ray origin instead of setting tmin due to numeric
                // issues in ray-sphere intersection. The isect.dist has to be
                // extended by this EPS_RAY after hit point is determined
                Ray ray(cameraState.mOrigin + cameraState.mDirection * EPS_RAY,
                    cameraState.mDirection, 0);

                Isect isect(1e36f);

                // Get radiance from environment
                if(!mScene.Intersect(ray, isect))
                {
                    if(mScene.GetBackground() != NULL)
                    {
                        if(cameraState.mPathLength >= mMinPathLength)
                        {
                            color += cameraState.mThroughput *
                                GetLightRadiance(mScene.GetBackground(),
                                mScene.GetBackgroundID(), cameraState,
                                hasPrevPoint ? prevPoint : cameraState.mOrigin,
                                Vec3f(0), ray.dir);
                        }
                    }

                    break;
                }

                const Vec3f hitPoint = ray.org + ray.dir * isect.dist;
                isect.dist += EPS_RAY;

                CameraBSDF bsdf(ray, isect, mScene);
                if(!bsdf.IsValid())
                    break;

                // Update the MIS quantities, following the initialization in
                // GenerateLightSample() or SampleScattering(). Implement equations
                // [tech. rep. (31)-(33)] or [tech. rep. (34)-(36)], respectively.
                {
                    cameraState.dVCM *= Mis(Sqr(isect.dist));
                    cameraState.dVCM /= Mis(std::abs(bsdf.CosThetaFix()));
                    cameraState.dVC  /= Mis(std::abs(bsdf.CosThetaFix()));
                    cameraState.dVM  /= Mis(std::abs(bsdf.CosThetaFix()));
                }

                // Light source has been hit; terminate afterwards, since
                // our light sources do not have reflective properties
                if(isect.lightID >= 0)
                {
                    const AbstractLight *light = mScene.GetLightPtr(isect.lightID);

                    if(cameraState.mPathLength >= mMinPathLength)
                    {
                        color += cameraState.mThroughput *
                            GetLightRadiance(light, isect.lightID, cameraState,
                            hasPrevPoint ? prevPoint : hitPoint,
                            hitPoint, ray.dir);
                    }

                    break;
                }

                // Terminate if eye sub-path is too long for connections or merging
                if(cameraState.mPathLength >= mMaxPathLength)
                    break;

                ////////////////////////////////////////////////////////////////
                // Vertex connection: Connect to a light source
                if(!bsdf.IsDelta() && mUseVC)
                {
                    if(cameraState.mPathLength + 1>= mMinPathLength)
                    {
                        color += cameraState.mThroughput *
                            DirectIllumination(cameraState, hitPoint, bsdf);
                    }
                }

                ////////////////////////////////////////////////////////////////
                // Vertex connection: Connect to light vertices
                if(!bsdf.IsDelta() && mUseVC)
                {
                    // For VC, each light sub-path is assigned to a particular eye
                    // sub-path, as in traditional BPT. It is also possible to
                    // connect to vertices from any light path, but MIS should
                    // be revisited.
                    const Vec2i range(
                        (pathIdx == 0) ? 0 : mPathEnds[pathIdx-1],
                        mPathEnds[pathIdx]);

                    for(int i = range.x; i < range.y; i++)
                    {
                        const LightVertex &lightVertex = mLightVertices[i];

                        if(lightVertex.mPathLength + 1 +
                           cameraState.mPathLength < mMinPathLength)
                            continue;

                        // Light vertices are stored in increasing path length
                        // order; once we go above the max path length, we can
                        // skip the rest
                        if(lightVertex.mPathLength + 1 +
                           cameraState.mPathLength > mMaxPathLength)
                            break;

                        color += cameraState.mThroughput * lightVertex.mThroughput *
                            ConnectVertices(lightVertex, bsdf, hitPoint,
                            cameraState);
                    }
                }

                ////////////////////////////////////////////////////////////////
                // Vertex merging: Merge with light vertices
                if(!bsdf.IsDelta() && mUseVM)
                {
                    RangeQuery query(*this, hitPoint, bsdf, cameraState);
                    mHashGrid.Process(mLightVertices, query);
                    color += cameraState.mThroughput * mVmNormalization * query.GetContrib();

                    // PPM merges only at the first non-specular surface from camera
                    if(mPpm) break;
                }

                // This surface becomes the previous point for the next ray,
                // which is where a competing NEE technique would sample from.
                prevPoint = hitPoint;
                hasPrevPoint = true;

                if(!SampleScattering(bsdf, hitPoint, cameraState))
                    break;
            }

            mFramebuffer.AddColor(screenSample, color);
        }

        mIterations++;
    }

private:

    // Mis power, we use balance heuristic
    float Mis(float aPdf) const
    {
        //return std::pow(aPdf, /*power*/);
        return aPdf;
    }

    // Actual discrete direct-light probability qD(x, light).  Without the
    // tree this is the uniform 1/N; consumes no random numbers.
    float LightPickProbability(
        const Vec3f &aReceivingPosition,
        int          aLightID) const
    {
        if(mUseTree)
            return float(mScene.GetLightTree().Pdf(aReceivingPosition, aLightID));
        return mUniformPickProb;
    }

    // r(x, light) = qD(x, light) / qE(light) = N * qD(x, light).  Equals 1
    // whenever the light tree is disabled, which makes every correction
    // below vanish and reproduces the uniform implementation exactly.
    float LightPickRatio(
        const Vec3f &aReceivingPosition,
        int          aLightID) const
    {
        if(!mUseTree || mUniformPickProb <= 0.f)
            return 1.f;
        return LightPickProbability(aReceivingPosition, aLightID) /
            mUniformPickProb;
    }

    // Picks a light for next event estimation.  Both branches consume exactly
    // one random number, so disabling the tree preserves the RNG stream.
    // Returns the original light ID and the actual qD used.
    int SampleDirectLight(
        const Vec3f &aReceivingPosition,
        float       &oPickProb)
    {
        if(mUseTree)
        {
            const float rnd = mRng.GetFloat();
            const int lightID = mScene.GetLightTree().Sample(
                aReceivingPosition, rnd);
            oPickProb = LightPickProbability(aReceivingPosition, lightID);
            return lightID;
        }

        oPickProb = mUniformPickProb;
        return int(mRng.GetFloat() * mScene.GetLightCount());
    }

    //////////////////////////////////////////////////////////////////////////
    // Camera tracing methods
    //////////////////////////////////////////////////////////////////////////

    // Generates new camera sample given a pixel index
    Vec2f GenerateCameraSample(
        const int    aPixelIndex,
        SubPathState &oCameraState)
    {
        const Camera &camera = mScene.mCamera;
        const int resX = int(camera.mResolution.x);

        // Determine pixel (x, y)
        const int x = aPixelIndex % resX;
        const int y = aPixelIndex / resX;

        // Jitter pixel position
        const Vec2f sample = Vec2f(float(x), float(y)) + mRng.GetVec2f();

        // Generate ray
        const Ray primaryRay = camera.GenerateRay(sample);

        // Compute pdf conversion factor from area on image plane to solid angle on ray
        const float cosAtCamera = Dot(camera.mForward, primaryRay.dir);
        const float imagePointToCameraDist = camera.mImagePlaneDist / cosAtCamera;
        const float imageToSolidAngleFactor = Sqr(imagePointToCameraDist) / cosAtCamera;

        // We put the virtual image plane at such a distance from the camera origin
        // that the pixel area is one and thus the image plane sampling pdf is 1.
        // The solid angle ray pdf is then equal to the conversion factor from
        // image plane area density to ray solid angle density
        const float cameraPdfW = imageToSolidAngleFactor;

        oCameraState.mOrigin       = primaryRay.org;
        oCameraState.mDirection    = primaryRay.dir;
        oCameraState.mThroughput   = Vec3f(1);

        oCameraState.mPathLength   = 1;
        oCameraState.mSpecularPath = 1;

        // Eye sub-path MIS quantities. Implements [tech. rep. (31)-(33)] partially.
        // The evaluation is completed after tracing the camera ray in the eye sub-path loop.
        oCameraState.dVCM = Mis(mLightSubPathCount / cameraPdfW);
        oCameraState.dVC  = 0;
        oCameraState.dVM  = 0;

        // Camera sub-paths never carry light-side NEE monomials.
        oCameraState.mNeeDVCM = 0;
        oCameraState.mNeeDVC  = 0;
        oCameraState.mNeeDVM  = 0;
        oCameraState.mLightID = -1;
        oCameraState.mR       = 1.f;

        return sample;
    }

    // Returns the radiance of a light source when hit by a random ray,
    // multiplied by MIS weight. Can be used for both Background and Area lights.
    //
    // For Background lights:
    //    Has to be called BEFORE updating the MIS quantities.
    //    Value of aHitpoint is irrelevant (passing Vec3f(0))
    //
    // For Area lights:
    //    Has to be called AFTER updating the MIS quantities.
    Vec3f GetLightRadiance(
        const AbstractLight *aLight,
        int                  aLightID,
        const SubPathState  &aCameraState,
        const Vec3f         &aNeePoint,
        const Vec3f         &aHitpoint,
        const Vec3f         &aRayDirection) const
    {
        float directPdfA, emissionPdfW;
        const Vec3f radiance = aLight->GetRadiance(mScene.mSceneSphere,
            aRayDirection, aHitpoint, &directPdfA, &emissionPdfW);

        if(radiance.IsZero())
            return Vec3f(0);

        // If we see light source directly from camera, no weighting is required
        if(aCameraState.mPathLength == 1)
            return radiance;

        // When using only vertex merging, we want purely specular paths
        // to give radiance (cannot get it otherwise). Rest is handled
        // by merging and we should return 0.
        if(mUseVM && !mUseVC)
            return aCameraState.mSpecularPath ? radiance : Vec3f(0);

        // The emission-based competitor keeps the uniform emission pick qE.
        emissionPdfW *= mUniformPickProb;

        // The NEE competitor uses the position-dependent qD, evaluated at the
        // first camera surface vertex that would perform the direct sampling.
        const float qD = LightPickProbability(aNeePoint, aLightID);
        directPdfA   *= qD;

        // Partial eye sub-path MIS weight [tech. rep. (43)].
        // If the last hit was specular, then dVCM == 0.
        const float wCamera = Mis(directPdfA) * aCameraState.dVCM +
            Mis(emissionPdfW) * aCameraState.dVC;

        // Partial light sub-path weight is 0 [tech. rep. (42)].

        // Full path MIS weight [tech. rep. (37)].
        const float misWeight = 1.f / (1.f + wCamera);

        return misWeight * radiance;
    }

    // Connects camera vertex to randomly chosen light point.
    // Returns emitted radiance multiplied by path MIS weight.
    // Has to be called AFTER updating the MIS quantities.
    Vec3f DirectIllumination(
        const SubPathState &aCameraState,
        const Vec3f        &aHitpoint,
        const CameraBSDF   &aBsdf)
    {
        // Keep the original RNG consumption order: the light pick consumes one
        // number before the two light-position numbers, so disabling the tree
        // leaves the random stream bit-identical.
        float lightPickProb;
        const int lightID = SampleDirectLight(aHitpoint, lightPickProb);
        const Vec2f rndPosSamples = mRng.GetVec2f();

        const AbstractLight *light = mScene.GetLightPtr(lightID);

        Vec3f directionToLight;
        float distance;
        float directPdfW, emissionPdfW, cosAtLight;
        const Vec3f radiance = light->Illuminate(mScene.mSceneSphere, aHitpoint,
            rndPosSamples, directionToLight, distance, directPdfW,
            &emissionPdfW, &cosAtLight);

        // If radiance == 0, other values are undefined, so have to early exit
        if(radiance.IsZero())
            return Vec3f(0);

        float bsdfDirPdfW, bsdfRevPdfW, cosToLight;
        const Vec3f bsdfFactor = aBsdf.Evaluate(mScene,
            directionToLight, cosToLight, &bsdfDirPdfW, &bsdfRevPdfW);

        if(bsdfFactor.IsZero())
            return Vec3f(0);

        const float continuationProbability = aBsdf.ContinuationProb();

        // If the light is delta light, we can never hit it
        // by BSDF sampling, so the probability of this path is 0
        bsdfDirPdfW *= light->IsDelta() ? 0.f : continuationProbability;

        bsdfRevPdfW *= continuationProbability;

        // Partial light sub-path MIS weight [tech. rep. (44)].  The light
        // pick here is the actual qD of the current NEE technique, which keeps
        // this formula valid for non-uniform selection as well.
        const float wLight = Mis(bsdfDirPdfW / (lightPickProb * directPdfW));

        // Partial eye sub-path MIS weight [tech. rep. (45)].  Both pdfs carry
        // the same light pick, which cancels, so this term is pick independent.
        const float wCamera = Mis(emissionPdfW * cosToLight / (directPdfW * cosAtLight)) * (
            mMisVmWeightFactor + aCameraState.dVCM + aCameraState.dVC * Mis(bsdfRevPdfW));

        // Full path MIS weight [tech. rep. (37)]
        const float misWeight = 1.f / (wLight + 1.f + wCamera);

        const Vec3f contrib =
            (misWeight * cosToLight / (lightPickProb * directPdfW)) * (radiance * bsdfFactor);

        if(contrib.IsZero() || mScene.Occluded(aHitpoint, directionToLight, distance))
            return Vec3f(0);

        return contrib;
    }

    // Connects an eye and a light vertex. Result multiplied by MIS weight, but
    // not multiplied by vertex throughputs. Has to be called AFTER updating MIS
    // constants. 'direction' is FROM eye TO light vertex.
    Vec3f ConnectVertices(
        const LightVertex  &aLightVertex,
        const CameraBSDF   &aCameraBsdf,
        const Vec3f        &aCameraHitpoint,
        const SubPathState &aCameraState) const
    {
        // Get the connection
        Vec3f direction   = aLightVertex.mHitpoint - aCameraHitpoint;
        const float dist2 = direction.LenSqr();
        float  distance   = std::sqrt(dist2);
        direction        /= distance;

        // Evaluate BSDF at camera vertex
        float cosCamera, cameraBsdfDirPdfW, cameraBsdfRevPdfW;
        const Vec3f cameraBsdfFactor = aCameraBsdf.Evaluate(
            mScene, direction, cosCamera, &cameraBsdfDirPdfW,
            &cameraBsdfRevPdfW);

        if(cameraBsdfFactor.IsZero())
            return Vec3f(0);

        // Camera continuation probability (for Russian roulette)
        const float cameraCont = aCameraBsdf.ContinuationProb();
        cameraBsdfDirPdfW *= cameraCont;
        cameraBsdfRevPdfW *= cameraCont;

        // Evaluate BSDF at light vertex
        float cosLight, lightBsdfDirPdfW, lightBsdfRevPdfW;
        const Vec3f lightBsdfFactor = aLightVertex.mBsdf.Evaluate(
            mScene, -direction, cosLight, &lightBsdfDirPdfW,
            &lightBsdfRevPdfW);

        if(lightBsdfFactor.IsZero())
            return Vec3f(0);

        // Light continuation probability (for Russian roulette)
        const float lightCont = aLightVertex.mBsdf.ContinuationProb();
        lightBsdfDirPdfW *= lightCont;
        lightBsdfRevPdfW *= lightCont;

        // Compute geometry term
        const float geometryTerm = cosLight * cosCamera / dist2;
        if(geometryTerm < 0)
            return Vec3f(0);

        // Convert pdfs to area pdf
        const float cameraBsdfDirPdfA = PdfWtoA(cameraBsdfDirPdfW, distance, cosLight);
        const float lightBsdfDirPdfA  = PdfWtoA(lightBsdfDirPdfW,  distance, cosCamera);

        // Partial light sub-path MIS weight [tech. rep. (40)]
        float wLight = Mis(cameraBsdfDirPdfA) * (
            mMisVmWeightFactor + aLightVertex.dVCM + aLightVertex.dVC * Mis(lightBsdfRevPdfW));

        // Correct the embedded NEE competitor by its per-light-subpath qD/qE
        // ratio.  Skipped with the tree off to preserve the original arithmetic.
        if(mUseTree)
        {
            const float neeLight = Mis(cameraBsdfDirPdfA) * (
                aLightVertex.mNeeDVCM +
                aLightVertex.mNeeDVC * Mis(lightBsdfRevPdfW));
            wLight += (aLightVertex.mR - 1.f) * neeLight;
        }

        // Partial eye sub-path MIS weight [tech. rep. (41)]
        const float wCamera = Mis(lightBsdfDirPdfA) * (
            mMisVmWeightFactor + aCameraState.dVCM + aCameraState.dVC * Mis(cameraBsdfRevPdfW));

        // Full path MIS weight [tech. rep. (37)]
        const float misWeight = 1.f / (wLight + 1.f + wCamera);

        const Vec3f contrib = (misWeight * geometryTerm) * cameraBsdfFactor * lightBsdfFactor;

        if(contrib.IsZero() || mScene.Occluded(aCameraHitpoint, direction, distance))
            return Vec3f(0);

        return contrib;
    }

    //////////////////////////////////////////////////////////////////////////
    // Light tracing methods
    //////////////////////////////////////////////////////////////////////////

    // Samples light emission.  Light sub-paths always select lights uniformly
    // (qE = 1/N) regardless of the light tree; only NEE uses qD.
    void GenerateLightSample(SubPathState &oLightState)
    {
        // We sample lights uniformly for emission.
        const int   lightCount    = mScene.GetLightCount();
        const float lightPickProb = 1.f / lightCount;

        const int   lightID       = int(mRng.GetFloat() * lightCount);
        const Vec2f rndDirSamples = mRng.GetVec2f();
        const Vec2f rndPosSamples = mRng.GetVec2f();

        const AbstractLight *light = mScene.GetLightPtr(lightID);

        float emissionPdfW, directPdfA, cosLight;
        oLightState.mThroughput = light->Emit(mScene.mSceneSphere, rndDirSamples, rndPosSamples,
            oLightState.mOrigin, oLightState.mDirection,
            emissionPdfW, &directPdfA, &cosLight);

        emissionPdfW *= lightPickProb;
        directPdfA   *= lightPickProb;

        oLightState.mThroughput    /= emissionPdfW;
        oLightState.mPathLength    = 1;
        oLightState.mIsFiniteLight = light->IsFinite() ? 1 : 0;
        oLightState.mLightID       = lightID;
        // Default ratio 1; overwritten at the first surface hit when the tree
        // is active.  A light path that never hits a surface has no NEE
        // competitor anyway.
        oLightState.mR             = 1.f;

        // Light sub-path MIS quantities. Implements [tech. rep. (31)-(33)] partially.
        // The evaluation is completed after tracing the emission ray in the light sub-path loop.
        // Delta lights are handled as well [tech. rep. (48)-(50)].
        {
            oLightState.dVCM = Mis(directPdfA / emissionPdfW);

            if(!light->IsDelta())
            {
                const float usedCosLight = light->IsFinite() ? cosLight : 1.f;
                oLightState.dVC = Mis(usedCosLight / emissionPdfW);
            }
            else
            {
                oLightState.dVC = 0.f;
            }

            oLightState.dVM = oLightState.dVC * mMisVcWeightFactor;
        }

        // NEE-only birth values.  At the root the dVCM ratio is pick
        // independent (both pdfs carry the same pick), while dVC/dVM carry a
        // 1/pick term that is identical under the uniform assumption; the
        // (r - 1) correction at usage sites turns these into the qD weights.
        oLightState.mNeeDVCM = oLightState.dVCM;
        oLightState.mNeeDVC  = oLightState.dVC;
        oLightState.mNeeDVM  = oLightState.dVM;
    }

    // Computes contribution of light sample to camera by splatting is onto the
    // framebuffer. Multiplied by throughput (obviously, as nothing is returned).
    void ConnectToCamera(
        const SubPathState &aLightState,
        const Vec3f        &aHitpoint,
        const LightBSDF    &aBsdf)
    {
        const Camera &camera    = mScene.mCamera;
        Vec3f directionToCamera = camera.mPosition - aHitpoint;

        // Check point is in front of camera
        if(Dot(camera.mForward, -directionToCamera) <= 0.f)
            return;

        // Check it projects to the screen (and where)
        const Vec2f imagePos = camera.WorldToRaster(aHitpoint);
        if(!camera.CheckRaster(imagePos))
            return;

        // Compute distance and normalize direction to camera
        const float distEye2 = directionToCamera.LenSqr();
        const float distance = std::sqrt(distEye2);
        directionToCamera   /= distance;

        // Get the BSDF
        float cosToCamera, bsdfDirPdfW, bsdfRevPdfW;
        const Vec3f bsdfFactor = aBsdf.Evaluate(mScene,
            directionToCamera, cosToCamera, &bsdfDirPdfW, &bsdfRevPdfW);

        if(bsdfFactor.IsZero())
            return;

        bsdfRevPdfW *= aBsdf.ContinuationProb();

        // Compute pdf conversion factor from image plane area to surface area
        const float cosAtCamera = Dot(camera.mForward, -directionToCamera);
        const float imagePointToCameraDist = camera.mImagePlaneDist / cosAtCamera;
        const float imageToSolidAngleFactor = Sqr(imagePointToCameraDist) / cosAtCamera;
        const float imageToSurfaceFactor = imageToSolidAngleFactor * std::abs(cosToCamera) / Sqr(distance);

        // We put the virtual image plane at such a distance from the camera origin
        // that the pixel area is one and thus the image plane sampling pdf is 1.
        // The area pdf of aHitpoint as sampled from the camera is then equal to
        // the conversion factor from image plane area density to surface area density
        const float cameraPdfA = imageToSurfaceFactor;

        // Partial light sub-path weight [tech. rep. (46)]. Note the division by
        // mLightPathCount, which is the number of samples this technique uses.
        // This division also appears a few lines below in the framebuffer accumulation.
        float wLight = Mis(cameraPdfA / mLightSubPathCount) * (
            mMisVmWeightFactor + aLightState.dVCM + aLightState.dVC * Mis(bsdfRevPdfW));

        // The NEE competitor receives the emitter at the light sub-path's
        // first surface vertex x1, captured once as mR.  Skipped with the
        // tree off to preserve the exact original arithmetic.
        if(mUseTree)
        {
            const float neeLight = Mis(cameraPdfA / mLightSubPathCount) *
                aLightState.mNeeDVC * Mis(bsdfRevPdfW);
            wLight += (aLightState.mR - 1.f) * neeLight;
        }

        // Partial eye sub-path weight is 0 [tech. rep. (47)]

        // Full path MIS weight [tech. rep. (37)]. No MIS for traditional light tracing.
        const float misWeight = mLightTraceOnly ? 1.f : (1.f / (wLight + 1.f));

        const float surfaceToImageFactor = 1.f / imageToSurfaceFactor;

        // We divide the contribution by surfaceToImageFactor to convert the (already
        // divided) pdf from surface area to image plane area, w.r.t. which the
        // pixel integral is actually defined. We also divide by the number of samples
        // this technique makes, which is equal to the number of light sub-paths
        const Vec3f contrib = misWeight * aLightState.mThroughput * bsdfFactor /
            (mLightSubPathCount * surfaceToImageFactor);

        if(!contrib.IsZero())
        {
            if(mScene.Occluded(aHitpoint, directionToCamera, distance))
                return;

            mFramebuffer.AddColor(imagePos, contrib);
        }
    }

    // Samples a scattering direction camera/light sample according to BSDF.
    // Returns false for termination
    template<bool tLightSample>
    bool SampleScattering(
        const BSDF<tLightSample> &aBsdf,
        const Vec3f              &aHitPoint,
        SubPathState             &aoState)
    {
        // x,y for direction, z for component. No rescaling happens
        Vec3f rndTriplet  = mRng.GetVec3f();
        float bsdfDirPdfW, cosThetaOut;
        uint  sampledEvent;

        Vec3f bsdfFactor = aBsdf.Sample(mScene, rndTriplet, aoState.mDirection,
            bsdfDirPdfW, cosThetaOut, &sampledEvent);

        if(bsdfFactor.IsZero())
            return false;

        // If we sampled specular event, then the reverse probability
        // cannot be evaluated, but we know it is exactly the same as
        // forward probability, so just set it. If non-specular event happened,
        // we evaluate the pdf
        float bsdfRevPdfW = bsdfDirPdfW;
        if((sampledEvent & LightBSDF::kSpecular) == 0)
            bsdfRevPdfW = aBsdf.Pdf(mScene, aoState.mDirection, true);

        // Russian roulette
        const float contProb = aBsdf.ContinuationProb();
        if(mRng.GetFloat() > contProb)
            return false;

        bsdfDirPdfW *= contProb;
        bsdfRevPdfW *= contProb;

        // Sub-path MIS quantities for the next vertex. Only partial - the
        // evaluation is completed when the actual hit point is known,
        // i.e. after tracing the ray, in the sub-path loop.

        if(sampledEvent & LightBSDF::kSpecular)
        {
            // Specular scattering case [tech. rep. (53)-(55)] (partially, as noted above)
            aoState.dVCM = 0.f;
            assert(bsdfDirPdfW == bsdfRevPdfW);
            aoState.dVC *= Mis(cosThetaOut);
            aoState.dVM *= Mis(cosThetaOut);

            // The NEE monomial has no dVCM branch and is merely carried.
            aoState.mNeeDVCM = 0.f;
            aoState.mNeeDVC *= Mis(cosThetaOut);
            aoState.mNeeDVM *= Mis(cosThetaOut);

            aoState.mSpecularPath &= 1;
        }
        else
        {
            // Implements [tech. rep. (34)-(36)] (partially, as noted above)
            aoState.dVC = Mis(cosThetaOut / bsdfDirPdfW) * (
                aoState.dVC * Mis(bsdfRevPdfW) +
                aoState.dVCM + mMisVmWeightFactor);

            aoState.dVM = Mis(cosThetaOut / bsdfDirPdfW) * (
                aoState.dVM * Mis(bsdfRevPdfW) +
                aoState.dVCM * mMisVcWeightFactor + 1.f);

            aoState.dVCM = Mis(1.f / bsdfDirPdfW);

            // NEE-only recurrence: keep solely the carried monomial and drop
            // every birth constant (those introduce s >= 2 techniques).
            if(tLightSample)
            {
                aoState.mNeeDVC = Mis(cosThetaOut / bsdfDirPdfW) *
                    (aoState.mNeeDVC * Mis(bsdfRevPdfW));
                aoState.mNeeDVM = Mis(cosThetaOut / bsdfDirPdfW) *
                    (aoState.mNeeDVM * Mis(bsdfRevPdfW));
                aoState.mNeeDVCM = 0.f;
            }

            aoState.mSpecularPath &= 0;
        }

        aoState.mOrigin  = aHitPoint;
        aoState.mThroughput *= bsdfFactor * (cosThetaOut / bsdfDirPdfW);

        return true;
    }

private:

    bool  mUseVM;             // Vertex merging (of some form) is used
    bool  mUseVC;             // Vertex connection (BPT) is used
    bool  mLightTraceOnly;    // Do only light tracing
    bool  mPpm;               // Do PPM, same terminates camera after first merge
    bool  mUseTree;           // Position-dependent light selection enabled
    float mUniformPickProb;   // qE = 1/N, cached per iteration

    float mRadiusAlpha;       // Radius reduction rate parameter
    float mBaseRadius;        // Initial merging radius
    float mMisVmWeightFactor; // Weight of vertex merging (used in VC)
    float mMisVcWeightFactor; // Weight of vertex connection (used in VM)
    float mScreenPixelCount;  // Number of pixels
    float mLightSubPathCount; // Number of light sub-paths
    float mVmNormalization;   // 1 / (Pi * radius^2 * light_path_count)

    std::vector<LightVertex> mLightVertices; //!< Stored light vertices

    // For light path belonging to pixel index [x] it stores
    // where it's light vertices end (begin is at [x-1])
    std::vector<int> mPathEnds;
    HashGrid         mHashGrid;

    Rng              mRng;
};

#endif //__VERTEXCM_HXX__
