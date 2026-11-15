#pragma once
#include "core/scene.h"
#include "core/camera.h"
#include "core/film.h"
#include "core/volume.h"
#include "core/sampling.h"
#include "core/texture.h"
extern "C" Shape *CreateTriangleMeshShape(const Transform&, bool, const ParamSet&);
extern "C" Light *CreateAreaLight(const Transform&, const ParamSet&, const Reference<Shape>&);
extern "C" Material *CreateMatteMaterial(const Transform&, const TextureParams&);
extern "C" Primitive *CreateGridAccelerator(const vector<Reference<Primitive>>&, const ParamSet&);
extern "C" Primitive *CreateKDTreeAccelerator(const vector<Reference<Primitive>>&, const ParamSet&);
extern "C" SurfaceIntegrator *CreateDirectIntegrator(const ParamSet&);
extern "C" SurfaceIntegrator *CreatePathIntegrator(const ParamSet&);
extern "C" Camera *CreatePerspectiveCamera(const ParamSet&, const Transform&, Film*);
class TaskEmptyVolume final : public VolumeIntegrator {
public:
 Spectrum Li(const Scene*,const RayDifferential&,const Sample*,float*)const override{return Spectrum(0.f);}
 Spectrum Transmittance(const Scene*,const Ray&,const Sample*,float*)const override{return Spectrum(1.f);}
};

// Samples are supplied explicitly to Scene::Li; only RoundSize is queried.
class TaskFixedSampler final : public Sampler {
public:
 TaskFixedSampler():Sampler(0,1,0,1,1){}
 bool GetNextSample(Sample*)override{return false;}
 int RoundSize(int n)const override{return n;}
};
