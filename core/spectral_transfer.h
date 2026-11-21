
/*
    pbrt source code Copyright(c) 1998-2010 Matt Pharr and Greg Humphreys.

    This file is part of pbrt.

    pbrt is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 */

#ifndef PBRT_SPECTRAL_TRANSFER_H
#define PBRT_SPECTRAL_TRANSFER_H
// spectral_transfer.h*
#include "pbrt.h"
#include "color.h"

// SpectralTransfer Declarations
//
// A discrete linear operator on the COLOR_SAMPLES channels of a
// Spectrum.  Coefficients are stored row-major as a square matrix C:
// output channel i receives C(i,j) times input channel j, i.e.
// (C * v)[i] = sum_j C(i,j) v[j].  Row i / column j therefore means
// "incident channel j to outgoing channel i", matching the
// "transfer" parameter of the spectralmix material.
//
// The class owns its values and supports the algebra needed by the
// BxDF/BSDF operator paths: diagonal adaptation from a Spectrum,
// addition, scalar scaling, matrix composition, action on a Spectrum
// and an all-zero predicate.  Only the fixed COLOR_SAMPLES discrete
// channel set is represented; continuous wavelength sampling is not
// extended.
class COREDLL SpectralTransfer {
public:
	// SpectralTransfer Public Methods
	SpectralTransfer() {
		for (int i = 0; i < COLOR_SAMPLES*COLOR_SAMPLES; ++i)
			m[i] = 0.f;
	}
	explicit SpectralTransfer(const Spectrum &diag) {
		for (int i = 0; i < COLOR_SAMPLES; ++i)
			for (int j = 0; j < COLOR_SAMPLES; ++j)
				(*this)(i, j) = (i == j) ? diag[i] : 0.f;
	}
	explicit SpectralTransfer(const float *coeffs) {
		for (int i = 0; i < COLOR_SAMPLES*COLOR_SAMPLES; ++i)
			m[i] = coeffs[i];
	}
	static SpectralTransfer Identity() {
		SpectralTransfer r;
		for (int i = 0; i < COLOR_SAMPLES; ++i)
			r(i, i) = 1.f;
		return r;
	}
	float &operator()(int i, int j) {
		return m[i*COLOR_SAMPLES + j];
	}
	float operator()(int i, int j) const {
		return m[i*COLOR_SAMPLES + j];
	}
	const float *Rows() const { return m; }
	Spectrum Diagonal() const {
		Spectrum r;
		for (int i = 0; i < COLOR_SAMPLES; ++i)
			r[i] = (*this)(i, i);
		return r;
	}
	bool IsZero() const {
		for (int i = 0; i < COLOR_SAMPLES*COLOR_SAMPLES; ++i)
			if (m[i] != 0.f) return false;
		return true;
	}
	bool IsNaN() const {
		for (int i = 0; i < COLOR_SAMPLES*COLOR_SAMPLES; ++i)
			if (isnan(m[i])) return true;
		return false;
	}
	bool AllFinite() const {
		for (int i = 0; i < COLOR_SAMPLES*COLOR_SAMPLES; ++i)
			if (isnan(m[i]) || isinf(m[i])) return false;
		return true;
	}
	SpectralTransfer operator+(const SpectralTransfer &t2) const {
		SpectralTransfer r;
		for (int i = 0; i < COLOR_SAMPLES*COLOR_SAMPLES; ++i)
			r.m[i] = m[i] + t2.m[i];
		return r;
	}
	SpectralTransfer &operator+=(const SpectralTransfer &t2) {
		for (int i = 0; i < COLOR_SAMPLES*COLOR_SAMPLES; ++i)
			m[i] += t2.m[i];
		return *this;
	}
	SpectralTransfer operator*(float s) const {
		SpectralTransfer r;
		for (int i = 0; i < COLOR_SAMPLES*COLOR_SAMPLES; ++i)
			r.m[i] = m[i] * s;
		return r;
	}
	SpectralTransfer &operator*=(float s) {
		for (int i = 0; i < COLOR_SAMPLES*COLOR_SAMPLES; ++i)
			m[i] *= s;
		return *this;
	}
	// Matrix composition: returns this * t2, so a vector is acted on
	// by t2 first and then by this.  Path throughput accumulated from
	// the eye towards the light is updated as T = T * F.
	SpectralTransfer operator*(const SpectralTransfer &t2) const {
		SpectralTransfer r;
		for (int i = 0; i < COLOR_SAMPLES; ++i)
			for (int j = 0; j < COLOR_SAMPLES; ++j) {
				float v = 0.f;
				for (int k = 0; k < COLOR_SAMPLES; ++k)
					v += (*this)(i, k) * t2(k, j);
				r(i, j) = v;
			}
		return r;
	}
	// Apply the operator to a Spectrum: r[i] = sum_j C(i,j) v[j].
	Spectrum operator*(const Spectrum &v) const {
		Spectrum r;
		for (int i = 0; i < COLOR_SAMPLES; ++i) {
			float s = 0.f;
			for (int j = 0; j < COLOR_SAMPLES; ++j)
				s += (*this)(i, j) * v[j];
			r[i] = s;
		}
		return r;
	}
private:
	// SpectralTransfer Private Data
	float m[COLOR_SAMPLES*COLOR_SAMPLES];
};

inline SpectralTransfer operator*(float s, const SpectralTransfer &t) {
	return t * s;
}

// True when every channel of the Spectrum is finite.
COREDLL bool SpectrumIsFinite(const Spectrum &s);
#endif // PBRT_SPECTRAL_TRANSFER_H
