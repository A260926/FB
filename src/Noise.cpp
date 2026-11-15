#include "Noise.h"

#include <array>
#include <cmath>

// Ken Perlin's improved-noise permutation table, doubled to avoid wrapping.
static const std::array<int, 512> P = [] {
	const int perm[256] = {
	    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,  69,  142,
	    8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203,
	    117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136, 171, 168, 68,  175, 74,  165,
	    71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,
	    55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187, 208, 89,
	    18,  169, 200, 196, 135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250,
	    124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,  182, 189,
	    28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,
	    129, 22,  39,  253, 19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,
	    242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,
	    181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,  222, 114,
	    67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180};
	std::array<int, 512> p{};
	for (int i = 0; i < 256; i++) {
		p[i] = perm[i];
		p[256 + i] = perm[i];
	}
	return p;
}();

static float fade(float t) {
	return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float lerp(float t, float a, float b) {
	return a + t * (b - a);
}

static float grad(int hash, float x, float y, float z) {
	int h = hash & 15;
	float u = h < 8 ? x : y;
	float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
	return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

float perlin(float x, float y, float z) {
	int X = static_cast<int>(std::floor(x)) & 255;
	int Y = static_cast<int>(std::floor(y)) & 255;
	int Z = static_cast<int>(std::floor(z)) & 255;
	x -= std::floor(x);
	y -= std::floor(y);
	z -= std::floor(z);
	float u = fade(x);
	float v = fade(y);
	float w = fade(z);
	int A = P[X] + Y, AA = P[A] + Z, AB = P[A + 1] + Z;
	int B = P[X + 1] + Y, BA = P[B] + Z, BB = P[B + 1] + Z;
	return lerp(w,
	            lerp(v, lerp(u, grad(P[AA], x, y, z), grad(P[BA], x - 1, y, z)),
	                 lerp(u, grad(P[AB], x, y - 1, z), grad(P[BB], x - 1, y - 1, z))),
	            lerp(v, lerp(u, grad(P[AA + 1], x, y, z - 1), grad(P[BA + 1], x - 1, y, z - 1)),
	                 lerp(u, grad(P[AB + 1], x, y - 1, z - 1), grad(P[BB + 1], x - 1, y - 1, z - 1))));
}

// Derivative of the quintic fade function fade(t) = 6t^5 - 15t^4 + 10t^3.
// d/dt fade(t) = 30t^4 - 60t^3 + 30t^2.
static float fadeDeriv(float t) {
	return t * t * (t * (t * 30.0f - 60.0f) + 30.0f);
}

// Returns the actual gradient vector for a given hash, chosen so that
// dot(gradVec(hash), (x, y, z)) is identical to grad(hash, x, y, z) above.
// This lets us reuse the exact same gradient scheme while also exposing the
// per-axis components needed for the analytical derivative.
static glm::vec3 gradVec(int hash) {
	int h = hash & 15;
	float su = ((h & 1) == 0) ? 1.0f : -1.0f; // sign applied to the u term
	float sv = ((h & 2) == 0) ? 1.0f : -1.0f; // sign applied to the v term
	glm::vec3 g(0.0f, 0.0f, 0.0f);
	// u is x when h < 8, otherwise y.
	if (h < 8)
		g.x += su;
	else
		g.y += su;
	// v is y when h < 4, x when h == 12 or 14, otherwise z.
	if (h < 4)
		g.y += sv;
	else if (h == 12 || h == 14)
		g.x += sv;
	else
		g.z += sv;
	return g;
}

glm::vec4 perlinNoiseD(const glm::vec3 &p) {
	float x = p.x, y = p.y, z = p.z;
	int X = static_cast<int>(std::floor(x)) & 255;
	int Y = static_cast<int>(std::floor(y)) & 255;
	int Z = static_cast<int>(std::floor(z)) & 255;
	x -= std::floor(x);
	y -= std::floor(y);
	z -= std::floor(z);

	// Quintic fade weights and their derivatives on each axis.
	float u = fade(x), v = fade(y), w = fade(z);
	float du = fadeDeriv(x), dv = fadeDeriv(y), dw = fadeDeriv(z);

	int A = P[X] + Y, AA = P[A] + Z, AB = P[A + 1] + Z;
	int B = P[X + 1] + Y, BA = P[B] + Z, BB = P[B + 1] + Z;

	// Gradient vectors at the 8 cube corners (same hashing as perlin()).
	glm::vec3 g000 = gradVec(P[AA]);
	glm::vec3 g100 = gradVec(P[BA]);
	glm::vec3 g010 = gradVec(P[AB]);
	glm::vec3 g110 = gradVec(P[BB]);
	glm::vec3 g001 = gradVec(P[AA + 1]);
	glm::vec3 g101 = gradVec(P[BA + 1]);
	glm::vec3 g011 = gradVec(P[AB + 1]);
	glm::vec3 g111 = gradVec(P[BB + 1]);

	// Corner noise values: dot(gradient, offset from that corner).
	float a = glm::dot(g000, glm::vec3(x, y, z));
	float b = glm::dot(g100, glm::vec3(x - 1.0f, y, z));
	float c = glm::dot(g010, glm::vec3(x, y - 1.0f, z));
	float d = glm::dot(g110, glm::vec3(x - 1.0f, y - 1.0f, z));
	float e = glm::dot(g001, glm::vec3(x, y, z - 1.0f));
	float f = glm::dot(g101, glm::vec3(x - 1.0f, y, z - 1.0f));
	float gg = glm::dot(g011, glm::vec3(x, y - 1.0f, z - 1.0f));
	float hh = glm::dot(g111, glm::vec3(x - 1.0f, y - 1.0f, z - 1.0f));

	// Rewrite the trilinear interpolation as a polynomial in (u, v, w):
	// V = k0 + k1 u + k2 v + k3 w + k4 uv + k5 vw + k6 uw + k7 uvw.
	float k0 = a;
	float k1 = b - a;
	float k2 = c - a;
	float k3 = e - a;
	float k4 = a - b - c + d;
	float k5 = a - c - e + gg;
	float k6 = a - b - e + f;
	float k7 = -a + b + c - d + e - f - gg + hh;

	float value = k0 + k1 * u + k2 * v + k3 * w + k4 * u * v + k5 * v * w + k6 * u * w + k7 * u * v * w;

	// The same k combinations, but of the gradient vectors, give the direct
	// position dependence of the corner values (each corner value is linear in
	// position with slope equal to its gradient vector).
	glm::vec3 K0 = g000;
	glm::vec3 K1 = g100 - g000;
	glm::vec3 K2 = g010 - g000;
	glm::vec3 K3 = g001 - g000;
	glm::vec3 K4 = g000 - g100 - g010 + g110;
	glm::vec3 K5 = g000 - g010 - g001 + g011;
	glm::vec3 K6 = g000 - g100 - g001 + g101;
	glm::vec3 K7 = -g000 + g100 + g010 - g110 + g001 - g101 - g011 + g111;

	// Direct term: partial derivative through the corner values (u, v, w held
	// fixed). This is exactly the k-polynomial evaluated on the gradient vectors.
	glm::vec3 direct = K0 + K1 * u + K2 * v + K3 * w + K4 * (u * v) + K5 * (v * w) + K6 * (u * w) + K7 * (u * v * w);

	// Fade-chain term: partial derivative through the fade weights, dV/du * du/dx
	// etc. dV/du = k1 + k4 v + k6 w + k7 vw, and similarly for v and w.
	glm::vec3 gradient;
	gradient.x = direct.x + (k1 + k4 * v + k6 * w + k7 * v * w) * du;
	gradient.y = direct.y + (k2 + k4 * u + k5 * w + k7 * u * w) * dv;
	gradient.z = direct.z + (k3 + k5 * v + k6 * u + k7 * u * v) * dw;

	return glm::vec4(value, gradient.x, gradient.y, gradient.z);
}

glm::vec4 fractalNoiseD(const glm::vec3 &p, int octaves) {
	float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, norm = 0.0f;
	glm::vec3 gradSum(0.0f, 0.0f, 0.0f);
	for (int i = 0; i < octaves; i++) {
		glm::vec4 nd = perlinNoiseD(p * frequency);
		sum += amplitude * nd.x;
		// Chain rule: d/dp of noise(p * frequency) picks up a factor of frequency.
		gradSum += (amplitude * frequency) * glm::vec3(nd.y, nd.z, nd.w);
		norm += amplitude;
		amplitude *= 0.5f;
		frequency *= 2.0f;
	}
	// Match fractalNoise's mapping of ~[-1,1] to [0,1]; the gradient of that
	// affine map is simply 0.5 / norm times the accumulated gradient.
	float value = 0.5f + 0.5f * (sum / norm);
	glm::vec3 gradient = (0.5f / norm) * gradSum;
	return glm::vec4(value, gradient.x, gradient.y, gradient.z);
}

// Hashes an integer lattice point to a pseudo-random value in [0, 1]. We reuse
// the same permutation table P as the Perlin code, folding the three integer
// coordinates through it the way Perlin folds corner indices. The final table
// entry lies in [0, 255], which we normalise to [0, 1]. Because P is a fixed
// permutation, this hash is deterministic and stable across calls.
static float latticeValue(int X, int Y, int Z) {
	int h = P[P[P[X & 255] + (Y & 255)] + (Z & 255)];
	return static_cast<float>(h) / 255.0f;
}

float valueNoise(const glm::vec3 &p) {
	// Integer lattice cell containing p, and the fractional position within it.
	int X = static_cast<int>(std::floor(p.x));
	int Y = static_cast<int>(std::floor(p.y));
	int Z = static_cast<int>(std::floor(p.z));
	float x = p.x - std::floor(p.x);
	float y = p.y - std::floor(p.y);
	float z = p.z - std::floor(p.z);

	// Quintic-faded interpolation weights on each axis (same fade as Perlin).
	float u = fade(x);
	float v = fade(y);
	float w = fade(z);

	// Hashed random values at the 8 corners of the lattice cell.
	float v000 = latticeValue(X, Y, Z);
	float v100 = latticeValue(X + 1, Y, Z);
	float v010 = latticeValue(X, Y + 1, Z);
	float v110 = latticeValue(X + 1, Y + 1, Z);
	float v001 = latticeValue(X, Y, Z + 1);
	float v101 = latticeValue(X + 1, Y, Z + 1);
	float v011 = latticeValue(X, Y + 1, Z + 1);
	float v111 = latticeValue(X + 1, Y + 1, Z + 1);

	// Trilinear interpolation of the corner values. Each input is already in
	// [0, 1] and the weights are in [0, 1], so the result stays in [0, 1].
	float x00 = lerp(u, v000, v100);
	float x10 = lerp(u, v010, v110);
	float x01 = lerp(u, v001, v101);
	float x11 = lerp(u, v011, v111);
	float y0 = lerp(v, x00, x10);
	float y1 = lerp(v, x01, x11);
	return lerp(w, y0, y1);
}

float fractalValueNoise(const glm::vec3 &p, int octaves) {
	// fBm sum of value noise. valueNoise is already in [0, 1], so the amplitude
	// weighted sum divided by the summed amplitudes stays in [0, 1] directly
	// (no [-1,1] -> [0,1] remap is needed, unlike the gradient fractalNoise).
	float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, norm = 0.0f;
	for (int i = 0; i < octaves; i++) {
		sum += amplitude * valueNoise(p * frequency);
		norm += amplitude;
		amplitude *= 0.5f;
		frequency *= 2.0f;
	}
	return sum / norm;
}

float fractalNoise(const glm::vec3 &p, int octaves) {
	float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, norm = 0.0f;
	for (int i = 0; i < octaves; i++) {
		sum += amplitude * perlin(p.x * frequency, p.y * frequency, p.z * frequency);
		norm += amplitude;
		amplitude *= 0.5f;
		frequency *= 2.0f;
	}
	return 0.5f + 0.5f * (sum / norm); // map ~[-1,1] to [0,1]
}
