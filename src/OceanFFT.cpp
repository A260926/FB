#include "OceanFFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <glm/glm.hpp>
#include <random>
#include <vector>

namespace {
using cf = std::complex<float>;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kGravity = 9.81f;

// Ocean spectrum parameters. `size` is the physical side length of the periodic
// patch, so the fundamental wavevector spacing is 2*pi/size.
constexpr float kWindSpeed = 12.0f;          // V, metres/second
constexpr glm::vec2 kWindDir = {1.0f, 0.6f}; // prevailing wind heading
constexpr float kPhillipsAmp = 0.0009f;      // A, overall spectral energy
constexpr float kSmallWaveCut = 0.6f;        // suppress ripples below this length
constexpr float kHeightScale = 0.06f;        // target RMS height as a fraction of size

// Phillips spectrum P(k): energy density of a wind-driven sea at wavevector k.
float phillips(const glm::vec2 &k, const glm::vec2 &windDir) {
	float k2 = glm::dot(k, k);
	if (k2 < 1e-12f)
		return 0.0f;
	float k4 = k2 * k2;
	float bigL = kWindSpeed * kWindSpeed / kGravity; // largest wind-driven wave
	glm::vec2 kHat = k / std::sqrt(k2);
	float kDotW = glm::dot(kHat, windDir);
	float directional = kDotW * kDotW; // waves travelling with the wind dominate
	float p = kPhillipsAmp * std::exp(-1.0f / (k2 * bigL * bigL)) / k4 * directional;
	// Damp the very shortest wavelengths so the surface stays smooth.
	p *= std::exp(-k2 * kSmallWaveCut * kSmallWaveCut);
	return p;
}
} // namespace

std::vector<ModelTriangle> generateOceanFFT(int gridN, float size, float time) {
	int n = gridN;
	glm::vec2 windDir = glm::normalize(kWindDir);

	// --- Fixed-seed Gaussian draws for the initial spectrum -----------------
	// A single deterministic seed makes the base amplitudes time-independent, so
	// the surface animates coherently rather than reshuffling every frame.
	std::mt19937 rng(1337u);
	std::normal_distribution<float> gauss(0.0f, 1.0f);
	std::vector<cf> noise(static_cast<size_t>(n) * n);
	for (int a = 0; a < n; a++)
		for (int b = 0; b < n; b++)
			noise[static_cast<size_t>(a) * n + b] = cf(gauss(rng), gauss(rng));

	// Wavevector for spectral index (a, b), centred so a,b in [0,n) map to modes
	// from -n/2 .. n/2-1. dk is the fundamental spacing of the periodic patch.
	float dk = 2.0f * kPi / size;
	auto waveVec = [&](int a, int b) {
		float kx = (a - n / 2) * dk;
		float kz = (b - n / 2) * dk;
		return glm::vec2(kx, kz);
	};

	// h0(k) = (1/sqrt2)(xi_r + i xi_i) sqrt(Phillips(k)).
	std::vector<cf> h0(static_cast<size_t>(n) * n);
	const float invSqrt2 = 1.0f / std::sqrt(2.0f);
	for (int a = 0; a < n; a++) {
		for (int b = 0; b < n; b++) {
			glm::vec2 k = waveVec(a, b);
			float amp = std::sqrt(phillips(k, windDir));
			h0[static_cast<size_t>(a) * n + b] = invSqrt2 * noise[static_cast<size_t>(a) * n + b] * amp;
		}
	}

	// --- Time evolution: h(k,t) = h0(k) e^{i w t} + conj(h0(-k)) e^{-i w t} ---
	// The conjugate/mirror term keeps the field Hermitian so the inverse
	// transform yields a real height. -k for index (a,b) lives at ((n-a)%n,
	// (n-b)%n), which corresponds to the negated mode.
	std::vector<cf> ht(static_cast<size_t>(n) * n);
	for (int a = 0; a < n; a++) {
		for (int b = 0; b < n; b++) {
			glm::vec2 k = waveVec(a, b);
			float kLen = std::sqrt(glm::dot(k, k));
			float omega = std::sqrt(kGravity * kLen); // deep-water dispersion
			cf phase(std::cos(omega * time), std::sin(omega * time));
			int am = (n - a) % n;
			int bm = (n - b) % n;
			cf h0k = h0[static_cast<size_t>(a) * n + b];
			cf h0mk = h0[static_cast<size_t>(am) * n + bm];
			ht[static_cast<size_t>(a) * n + b] = h0k * phase + std::conj(h0mk) * std::conj(phase);
		}
	}

	// --- Inverse DFT to the spatial height field ----------------------------
	// A direct inverse transform is O(N^4) but fine for modest grids; it also
	// lets us sample the periodic field at the (n+1)^2 vertex positions directly.
	// Precompute per-column complex exponentials to avoid redundant trig.
	std::vector<std::vector<glm::vec3>> pos(n + 1, std::vector<glm::vec3>(n + 1));
	std::vector<std::vector<float>> heights(n + 1, std::vector<float>(n + 1, 0.0f));
	float invN2 = 1.0f / static_cast<float>(n * n);

	for (int i = 0; i <= n; i++) {
		float x = (static_cast<float>(i) / n - 0.5f) * size;
		for (int j = 0; j <= n; j++) {
			float z = (static_cast<float>(j) / n - 0.5f) * size;
			cf sum(0.0f, 0.0f);
			for (int a = 0; a < n; a++) {
				for (int b = 0; b < n; b++) {
					glm::vec2 k = waveVec(a, b);
					float arg = k.x * x + k.y * z;
					cf e(std::cos(arg), std::sin(arg));
					sum += ht[static_cast<size_t>(a) * n + b] * e;
				}
			}
			heights[i][j] = sum.real() * invN2;
		}
	}

	// Normalise to a target RMS so the visible amplitude is stable regardless of
	// the absolute spectral energy, while preserving relative wave structure.
	double sumSq = 0.0;
	for (int i = 0; i <= n; i++)
		for (int j = 0; j <= n; j++)
			sumSq += static_cast<double>(heights[i][j]) * heights[i][j];
	float rms = std::sqrt(static_cast<float>(sumSq / ((n + 1.0) * (n + 1.0))));
	float scale = rms > 1e-8f ? (kHeightScale * size) / rms : 1.0f;

	for (int i = 0; i <= n; i++) {
		float x = (static_cast<float>(i) / n - 0.5f) * size;
		for (int j = 0; j <= n; j++) {
			float z = (static_cast<float>(j) / n - 0.5f) * size;
			pos[i][j] = glm::vec3(x, heights[i][j] * scale, z);
		}
	}

	// Smooth normals from height-field finite differences (same scheme as the
	// Gerstner ocean).
	std::vector<std::vector<glm::vec3>> nrm(n + 1, std::vector<glm::vec3>(n + 1, glm::vec3(0, 1, 0)));
	for (int i = 0; i <= n; i++) {
		for (int j = 0; j <= n; j++) {
			glm::vec3 dx = pos[std::min(i + 1, n)][j] - pos[std::max(i - 1, 0)][j];
			glm::vec3 dz = pos[i][std::min(j + 1, n)] - pos[i][std::max(j - 1, 0)];
			glm::vec3 nn = glm::normalize(glm::cross(dz, dx));
			if (nn.y < 0.0f)
				nn = -nn;
			nrm[i][j] = nn;
		}
	}

	std::vector<ModelTriangle> tris;
	tris.reserve(static_cast<size_t>(n) * n * 2);
	Colour water(70, 120, 180);
	auto quad = [&](int i0, int j0, int i1, int j1, int i2, int j2) {
		ModelTriangle t;
		t.vertices = {pos[i0][j0], pos[i1][j1], pos[i2][j2]};
		t.vertexNormals = {nrm[i0][j0], nrm[i1][j1], nrm[i2][j2]};
		t.colour = water;
		t.material = Material::Glass;
		t.normal = glm::normalize(glm::cross(t.vertices[1] - t.vertices[0], t.vertices[2] - t.vertices[0]));
		if (t.normal.y < 0.0f)
			t.normal = -t.normal;
		tris.push_back(t);
	};
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			quad(i, j, i + 1, j, i + 1, j + 1);
			quad(i, j, i + 1, j + 1, i, j + 1);
		}
	}
	return tris;
}
