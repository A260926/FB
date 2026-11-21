/*
 * Read-only test adapter into the private MIS bookkeeping of VertexCM.
 *
 * The production renderer keeps its partial MIS weights (dVCM/dVC/dVM), the
 * isolated NEE monomials and the light sub-path root IDs private.  The
 * transport tests need the real values to compare the iterative recurrence
 * against an independent, brute-force enumeration of every feasible path
 * generation strategy.  VertexCM grants this single struct friend access;
 * production code never includes or uses it.
 */

#ifndef __VERTEXCM_ADAPTER_HXX__
#define __VERTEXCM_ADAPTER_HXX__

#include "vertexcm.hxx"

struct VertexMisAdapter
{
    struct LightVertexInfo
    {
        Vec3f position;
        uint  pathLength;
        int   lightID;
        float dVCM;
        float dVC;
        float dVM;
        float neeDVCM;
        float neeDVC;
        float neeDVM;
        float r;
    };

    static int LightVertexCount(const VertexCM &aRenderer)
    {
        return int(aRenderer.mLightVertices.size());
    }

    static LightVertexInfo GetLightVertex(
        const VertexCM &aRenderer,
        int             aIndex)
    {
        const VertexCM::LightVertex &v = aRenderer.mLightVertices[aIndex];
        LightVertexInfo info;
        info.position = v.mHitpoint;
        info.pathLength = v.mPathLength;
        info.lightID = v.mLightID;
        info.dVCM = v.dVCM;
        info.dVC  = v.dVC;
        info.dVM  = v.dVM;
        info.neeDVCM = v.mNeeDVCM;
        info.neeDVC  = v.mNeeDVC;
        info.neeDVM  = v.mNeeDVM;
        info.r = v.mR;
        return info;
    }

    static bool UsesTree(const VertexCM &aRenderer)
    {
        return aRenderer.mUseTree;
    }

    static float UniformPickProb(const VertexCM &aRenderer)
    {
        return aRenderer.mUniformPickProb;
    }

    // r(x, light) = qD(x, light) / qE(light) as used inside the renderer.
    static float LightPickRatio(
        const VertexCM &aRenderer,
        const Vec3f    &aReceivingPosition,
        int             aLightID)
    {
        return aRenderer.LightPickRatio(aReceivingPosition, aLightID);
    }

    static float LightPickProbability(
        const VertexCM &aRenderer,
        const Vec3f    &aReceivingPosition,
        int             aLightID)
    {
        return aRenderer.LightPickProbability(aReceivingPosition, aLightID);
    }

    struct CameraVertexInfo
    {
        Vec3f hitpoint;
        bool  valid;
        bool  isDelta;
        float dVCM;
        float dVC;
        float dVM;
    };

    // Traces the camera ray of one pixel to its first surface vertex and
    // returns the production partial MIS weights evaluated there.
    static CameraVertexInfo TraceFirstCameraVertex(
        VertexCM &aRenderer,
        int       aPixelIndex)
    {
        VertexCM::SubPathState state;
        const Vec2f screen = aRenderer.GenerateCameraSample(aPixelIndex, state);
        (void)screen;

        CameraVertexInfo info;
        info.valid = false;
        info.isDelta = true;
        info.hitpoint = Vec3f(0.f);
        info.dVCM = info.dVC = info.dVM = 0.f;

        Ray ray(state.mOrigin + state.mDirection * EPS_RAY,
            state.mDirection, 0);
        Isect isect(1e36f);
        if(!aRenderer.mScene.Intersect(ray, isect))
            return info;
        if(isect.lightID >= 0)
            return info;

        const Vec3f hitPoint = ray.org + ray.dir * isect.dist;
        isect.dist += EPS_RAY;
        VertexCM::CameraBSDF bsdf(ray, isect, aRenderer.mScene);
        if(!bsdf.IsValid())
            return info;

        state.dVCM *= aRenderer.Mis(Sqr(isect.dist));
        const float absCos = aRenderer.Mis(std::abs(bsdf.CosThetaFix()));
        state.dVCM /= absCos;
        state.dVC  /= absCos;
        state.dVM  /= absCos;

        info.valid = true;
        info.isDelta = bsdf.IsDelta();
        info.hitpoint = hitPoint;
        info.dVCM = state.dVCM;
        info.dVC  = state.dVC;
        info.dVM  = state.dVM;
        return info;
    }

    struct ConnectionInfo
    {
        float misWeight;
        float wLight;
        float wCamera;
        float r;
        float neeLight;
        Vec3f contribution;
    };

    // Runs the production vertex-connection routine for one stored light
    // vertex against a camera hit point, exposing the MIS internals.
    static ConnectionInfo ConnectVertices(
        VertexCM &aRenderer,
        int       aLightVertexIndex,
        const Vec3f &aCameraHitpoint,
        const VertexCM::CameraBSDF &aCameraBsdf,
        const VertexCM::SubPathState &aCameraState)
    {
        const VertexCM::LightVertex &lv =
            aRenderer.mLightVertices[aLightVertexIndex];
        ConnectionInfo out;
        out.contribution = aRenderer.ConnectVertices(lv, aCameraBsdf,
            aCameraHitpoint, aCameraState);
        out.r = lv.mR;
        out.misWeight = 0.f;
        out.wLight = out.wCamera = out.neeLight = 0.f;
        return out;
    }

    struct DirectInfo
    {
        int   lightID;
        float lightPickProb;   // actual qD used by the production NEE
        float directPdfW;      // light's solid-angle direct pdf (no pick)
        float emissionPdfW;    // light's emission pdf (no pick)
        float bsdfDirPdfW;     // bsdf pdf toward the light (with continuation)
        float bsdfRevPdfW;
        float cosToLight;
        float cosAtLight;
        float wLight;
        float wCamera;
        float misWeight;
        Vec3f radiance;
        Vec3f bsdfFactor;
        bool  valid;
    };

    // Drives the production next-event-estimation code path at one camera
    // vertex and independently recomputes the same balance terms, exposing
    // both so tests can compare against a hand enumeration.
    static DirectInfo DirectIlluminationBreakdown(
        VertexCM &aRenderer,
        const VertexCM::SubPathState &aCameraState,
        const Vec3f &aHitpoint,
        const VertexCM::CameraBSDF &aBsdf)
    {
        DirectInfo out;
        out.valid = false;
        out.lightID = -1;
        out.lightPickProb = out.directPdfW = out.emissionPdfW = 0.f;
        out.bsdfDirPdfW = out.bsdfRevPdfW = 0.f;
        out.cosToLight = out.cosAtLight = 0.f;
        out.wLight = out.wCamera = out.misWeight = 0.f;

        // Reproduce the exact production light pick (one RNG number).
        float pickProb;
        const int lightID = aRenderer.SampleDirectLight(aHitpoint, pickProb);
        const Vec2f rndPos = aRenderer.mRng.GetVec2f();
        const AbstractLight *light =
            aRenderer.mScene.GetLightPtr(lightID);

        Vec3f dirToLight;
        float distance, directPdfW, emissionPdfW, cosAtLight;
        const Vec3f radiance = light->Illuminate(
            aRenderer.mScene.mSceneSphere, aHitpoint, rndPos,
            dirToLight, distance, directPdfW, &emissionPdfW, &cosAtLight);
        if(radiance.IsZero())
            return out;

        float bsdfDirPdfW, bsdfRevPdfW, cosToLight;
        const Vec3f factor = aBsdf.Evaluate(aRenderer.mScene, dirToLight,
            cosToLight, &bsdfDirPdfW, &bsdfRevPdfW);
        if(factor.IsZero())
            return out;

        const float cont = aBsdf.ContinuationProb();
        const float evalDir = bsdfDirPdfW *
            (light->IsDelta() ? 0.f : cont);
        const float evalRev = bsdfRevPdfW * cont;

        const float wLight = aRenderer.Mis(
            evalDir / (pickProb * directPdfW));
        const float wCamera = aRenderer.Mis(
            emissionPdfW * cosToLight / (directPdfW * cosAtLight)) * (
            aRenderer.mMisVmWeightFactor + aCameraState.dVCM +
            aCameraState.dVC * aRenderer.Mis(evalRev));
        const float misWeight = 1.f / (wLight + 1.f + wCamera);

        out.valid = true;
        out.lightID = lightID;
        out.lightPickProb = pickProb;
        out.directPdfW = directPdfW;
        out.emissionPdfW = emissionPdfW;
        out.bsdfDirPdfW = evalDir;
        out.bsdfRevPdfW = evalRev;
        out.cosToLight = cosToLight;
        out.cosAtLight = cosAtLight;
        out.wLight = wLight;
        out.wCamera = wCamera;
        out.misWeight = misWeight;
        out.radiance = radiance;
        out.bsdfFactor = factor;
        return out;
    }

    // Builds a diffuse camera vertex at aHitpoint (upward normal) and runs the
    // production next-event-estimation breakdown there, keeping all private
    // nested types inside this friend.
    static DirectInfo DirectIlluminationAt(
        VertexCM      &aRenderer,
        const Vec3f   &aHitpoint,
        const Vec3f   &aNormal,
        int            aMaterialID)
    {
        Ray ray;
        ray.org = aHitpoint + aNormal * 0.1f;
        ray.dir = -aNormal;
        ray.tmin = 0.f;
        Isect isect(0.1f);
        isect.matID = aMaterialID;
        isect.normal = aNormal;
        isect.lightID = -1;
        VertexCM::CameraBSDF bsdf(ray, isect, aRenderer.mScene);
        if(!bsdf.IsValid())
        {
            DirectInfo invalid;
            invalid.valid = false;
            return invalid;
        }

        VertexCM::SubPathState state;
        state.dVCM = 1.0f;
        state.dVC = 0.f;
        state.dVM = 0.f;
        return DirectIlluminationBreakdown(
            aRenderer, state, aHitpoint, bsdf);
    }
};

#endif //__VERTEXCM_ADAPTER_HXX__
