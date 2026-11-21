
/*
    pbrt source code Copyright(c) 1998-2010 Matt Pharr and Greg Humphreys.

    This file is part of pbrt.

    pbrt is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 */

// spectralmix.cpp*
#include "pbrt.h"
#include "material.h"
#include "spectral_transfer.h"
#include <cmath>

// SpectralMix Class Declarations
//
// A coating that moves energy between the discrete color channels.
// The "transfer" parameter is a row-major COLOR_SAMPLES x
// COLOR_SAMPLES matrix C (row i, column j: incident channel j to
// outgoing channel i); its reflection function is C / pi, sampled
// with the same cosine-weighted hemisphere sampling as a Lambertian
// lobe.  "Kd" is an additional ordinary Lambertian component that
// defaults to zero.  The two lobes live in one real BSDF and are
// mixed by the standard BSDF component selection.
class SpectralMix : public Material {
public:
	// SpectralMix Public Methods
	SpectralMix(const SpectralTransfer &transfer,
			const Spectrum &kd,
			Reference<Texture<float> > bump) {
		C = transfer;
		Kd = kd;
		bumpMap = bump;
	}
	BSDF *GetBSDF(const DifferentialGeometry &dgGeom,
	              const DifferentialGeometry &dgShading) const;
private:
	// SpectralMix Private Data
	SpectralTransfer C;
	Spectrum Kd;
	Reference<Texture<float> > bumpMap;
};
// SpectralMix Method Definitions
BSDF *SpectralMix::GetBSDF(const DifferentialGeometry &dgGeom,
		const DifferentialGeometry &dgShading) const {
	// Allocate _BSDF_, possibly doing bump-mapping with _bumpMap_
	DifferentialGeometry dgs;
	if (bumpMap)
		Bump(bumpMap, dgGeom, dgShading, &dgs);
	else
		dgs = dgShading;
	BSDF *bsdf = BSDF_ALLOC(BSDF)(dgs, dgGeom.nn);
	// The transfer lobe is always present: a purely off-diagonal
	// response is still a real response, never absorption.
	bsdf->Add(BSDF_ALLOC(TransferBxDF)(C));
	if (!Kd.Black())
		bsdf->Add(BSDF_ALLOC(Lambertian)(Kd));
	return bsdf;
}
extern "C" DLLEXPORT Material *CreateSpectralMixMaterial(
		const Transform &xform, const TextureParams &mp) {
	// Read the row-major transfer matrix; it must have exactly
	// COLOR_SAMPLES*COLOR_SAMPLES entries.
	int nTransfer = 0;
	const float *transfer =
		mp.GetMaterialParams().FindFloat("transfer", &nTransfer);
	if (!transfer)
		transfer = mp.GetGeomParams().FindFloat("transfer",
			&nTransfer);
	if (!transfer || nTransfer != COLOR_SAMPLES*COLOR_SAMPLES) {
		Error("SpectralMix: \"transfer\" must have exactly %d "
			"entries (row-major %dx%d matrix); got %d.",
			COLOR_SAMPLES*COLOR_SAMPLES, COLOR_SAMPLES,
			COLOR_SAMPLES, transfer ? nTransfer : 0);
		return NULL;
	}
	SpectralTransfer C(transfer);
	if (!C.AllFinite()) {
		Error("SpectralMix: \"transfer\" contains a non-finite "
			"coefficient.");
		return NULL;
	}
	// Kd is an additional ordinary diffuse reflectance, default zero
	Spectrum Kd = mp.FindSpectrum("Kd", Spectrum(0.f));
	if (!SpectrumIsFinite(Kd)) {
		Error("SpectralMix: \"Kd\" contains a non-finite component.");
		return NULL;
	}
	// Everything must be non-negative
	for (int i = 0; i < COLOR_SAMPLES*COLOR_SAMPLES; ++i) {
		if (C.Rows()[i] < 0.f) {
			Error("SpectralMix: \"transfer\" coefficients must be "
				"non-negative.");
			return NULL;
		}
	}
	for (int j = 0; j < COLOR_SAMPLES; ++j) {
		if (Kd[j] < 0.f) {
			Error("SpectralMix: \"Kd\" components must be "
				"non-negative.");
			return NULL;
		}
	}
	// Energy conservation per incident channel: the column sum of C
	// plus the corresponding Kd component must not exceed one.
	for (int j = 0; j < COLOR_SAMPLES; ++j) {
		float columnSum = 0.f;
		for (int i = 0; i < COLOR_SAMPLES; ++i)
			columnSum += C(i, j);
		if (columnSum + Kd[j] > 1.f + 1e-6f) {
			Error("SpectralMix: energy not conserved for incident "
				"channel %d: column sum %f plus Kd %f exceeds 1.",
				j, columnSum, Kd[j]);
			return NULL;
		}
	}
	Reference<Texture<float> > bumpMap =
		mp.GetFloatTexture("bumpmap", 0.f);
	return new SpectralMix(C, Kd, bumpMap);
}
