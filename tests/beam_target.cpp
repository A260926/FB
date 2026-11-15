// Beam volume target tests. Reference values are computed from independent small per-column
// arrays rather than by reusing the production implementation.
#include "beam_volume.h"
#include "beam_render.h"
#include "storage.h"
#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace Cubiquity;

namespace
{
	constexpr double kTol = 1e-9;

	typedef std::array<OpticalMaterial, Internals::MaterialCount> Palette;

	// Half of the emission is lost per occupied voxel: C = emission * (1 - T) / ln(2).
	const double kHalf = 0.5 / std::log(2.0);

	Palette blackPalette()
	{
		Palette palette{};
		for (auto& material : palette)
		{
			material.extinction = 0.0;
			material.emission = vec3d(0.0, 0.0, 0.0);
		}
		return palette;
	}

	bool close(double a, double b, double tol = kTol)
	{
		const double scale = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
		return std::fabs(a - b) <= tol * scale;
	}

	bool close(const vec3d& a, const vec3d& b, double tol = kTol)
	{
		return close(a.x, b.x, tol) && close(a.y, b.y, tol) && close(a.z, b.z, tol);
	}

	struct ReferenceColumn
	{
		double area = 1.0;
		std::vector<OpticalMaterial> segments; // in walking order, each of unit length
	};

	struct ReferenceResult
	{
		vec3d radiance = vec3d(0.0, 0.0, 0.0);
		double transmittance = 1.0;
	};

	// Compose the segments of every column and average the results by covered area.
	ReferenceResult referenceBeam(const Volume& volume, const Palette& palette,
		int axis, bool reverse,
		const std::array<i64, 2>& lo, const std::array<i64, 2>& hi,
		i64 depthLo, i64 depthHi)
	{
		ReferenceResult result;
		if (lo[0] >= hi[0] || lo[1] >= hi[1] || depthLo >= depthHi)
		{
			return result;
		}

		const int t0 = (axis == 0) ? 1 : 0;
		const int t1 = (axis == 2) ? 1 : 2;

		vec3d sumRadiance(0.0, 0.0, 0.0);
		double sumTransmittance = 0.0;
		double totalArea = 0.0;

		for (i64 a = lo[0]; a < hi[0]; ++a)
		{
			for (i64 b = lo[1]; b < hi[1]; ++b)
			{
				std::vector<i64> depths;
				for (i64 d = depthLo; d < depthHi; ++d)
				{
					depths.push_back(d);
				}
				if (reverse)
				{
					std::reverse(depths.begin(), depths.end());
				}

				double transmittance = 1.0;
				vec3d radiance(0.0, 0.0, 0.0);

				for (i64 d : depths)
				{
					i32 position[3] = { 0, 0, 0 };
					position[t0] = static_cast<i32>(a);
					position[t1] = static_cast<i32>(b);
					position[axis] = static_cast<i32>(d);

					const OpticalMaterial& material = palette[volume.voxel(position[0], position[1], position[2])];

					double localT = 1.0;
					vec3d localC(0.0, 0.0, 0.0);
					if (material.extinction == 0.0)
					{
						localC = material.emission;
					}
					else
					{
						localT = std::exp(-material.extinction);
						localC = material.emission * ((1.0 - localT) / material.extinction);
					}

					radiance = radiance + transmittance * localC;
					transmittance = transmittance * localT;
				}

				sumRadiance += radiance;
				sumTransmittance += transmittance;
				totalArea += 1.0;
			}
		}

		if (totalArea <= 0.0)
		{
			return result;
		}

		result.radiance = sumRadiance / totalArea;
		result.transmittance = sumTransmittance / totalArea;
		return result;
	}

	bool queryMatchesReference(const BeamVolume& beam, const Volume& volume, const Palette& palette,
		int axis, bool reverse,
		const std::array<i64, 2>& lo, const std::array<i64, 2>& hi,
		i64 depthLo, i64 depthHi)
	{
		const BeamResult actual = beam.query(axis, reverse, lo, hi, depthLo, depthHi);
		const ReferenceResult expected = referenceBeam(volume, palette, axis, reverse, lo, hi, depthLo, depthHi);
		if (!close(actual.radiance, expected.radiance, 1e-8)) return false;
		if (!close(actual.transmittance, expected.transmittance, 1e-8)) return false;
		return true;
	}

	// Irregular, asymmetric geometry spanning negative and positive coordinates.
	void fillComplexScene(Volume& volume)
	{
		for (i32 z = 0; z < 3; ++z)
		{
			for (i32 y = -3; y < 5; ++y)
			{
				for (i32 x = -5; x < 3; ++x)
				{
					if (((x * 3 + y * 5 + z * 7) & 3) == 0) volume.setVoxel(x, y, z, 1);
					else if (((x + y + z) & 1) == 0) volume.setVoxel(x, y, z, 2);
				}
			}
		}

		for (i32 z = -4; z < -1; ++z)
		{
			for (i32 y = 4; y < 8; ++y)
			{
				for (i32 x = -8; x < -2; ++x)
				{
					if ((((x + 2 * y + z) % 4) + 4) % 4 < 2) volume.setVoxel(x, y, z, 3);
				}
			}
		}

	}

	Palette makePalette()
	{
		Palette palette = blackPalette();
		palette[1].extinction = 0.35;
		palette[1].emission = vec3d(0.8, 0.3, 0.1);
		palette[2].extinction = 0.0; // Pure emission.
		palette[2].emission = vec3d(0.15, 0.45, 0.25);
		palette[3].extinction = 0.9;
		palette[3].emission = vec3d(0.0, 0.05, 0.7);
		return palette;
	}

	// Two layers on consecutive z slabs with ln(2) extinction each.
	void fillTwoLayers(Volume& volume, bool sameColumn)
	{
		volume.setVoxel(0, 0, 0, 1);
		volume.setVoxel(sameColumn ? 0 : 1, 0, 1, 1);
	}

	Palette makeHalfLifePalette()
	{
		Palette palette = blackPalette();
		palette[1].extinction = std::log(2.0);
		palette[1].emission = vec3d(1.0, 0.0, 0.0);
		return palette;
	}

	Palette makeRedBluePalette()
	{
		Palette palette = blackPalette();
		palette[1].extinction = std::log(2.0);
		palette[1].emission = vec3d(1.0, 0.0, 0.0);
		palette[2].extinction = std::log(2.0);
		palette[2].emission = vec3d(0.0, 0.0, 1.0);
		return palette;
	}

	// Reconstructs the example scene exactly as examples/beam_preview.cpp does.
	void fillExampleScene(Volume& volume)
	{
		const int cells = 8;
		const int cellSize = 4;

		for (int dz = 0; dz < 2; ++dz)
		{
			const i32 z = dz * cellSize;
			for (int cy = 0; cy < cells; ++cy)
			{
				for (int cx = 0; cx < cells; ++cx)
				{
					if (((cx + cy + dz) & 1) == 0) continue;

					const i64 offset = (dz == 0) ? 0 : cellSize / 2;
					const i32 x0 = static_cast<i32>(cx * cellSize + offset);
					const i32 y0 = static_cast<i32>(cy * cellSize + offset);

					for (i32 y = y0; y < y0 + cellSize / 2; ++y)
					{
						for (i32 x = x0; x < x0 + cellSize / 2; ++x)
						{
							volume.setVoxel(x, y, z, dz == 0 ? 1 : 2);
						}
					}
				}
			}
		}
	}

	Palette makeExamplePalette()
	{
		Palette palette = blackPalette();
		palette[1].extinction = std::log(2.0) * 2.0;
		palette[1].emission = vec3d(2.5, 0.15, 0.1);
		palette[2].extinction = std::log(2.0) * 2.0;
		palette[2].emission = vec3d(0.1, 0.2, 2.5);
		return palette;
	}

	void fillBlock(Volume& volume, int ox, int oy, int oz, MaterialId material)
	{
		for (int z = 0; z < 4; ++z)
		{
			for (int y = 0; y < 4; ++y)
			{
				for (int x = 0; x < 4; ++x)
				{
					const bool set = ((x + y + z) & 1) != 0;
					volume.setVoxel(ox + x, oy + y, oz + z, set ? material : 2);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// correlation
// ---------------------------------------------------------------------------------------------

static int correlation_basic_equations(void)
{
	const Palette palette = makeHalfLifePalette();

	// A single voxel with extinction ln(2): T = 1/2, C = emission * (1 - T) / extinction.
	{
		Volume volume;
		volume.setVoxel(0, 0, 0, 1);
		BeamVolume beam(volume, palette);

		const std::array<i64, 2> lo{ {0, 0} };
		const std::array<i64, 2> hi{ {1, 1} };
		const BeamResult result = beam.query(2, false, lo, hi, 0, 1);

		TASK_CHECK(close(result.transmittance, 0.5));
		TASK_CHECK(close(result.radiance, vec3d(kHalf, 0.0, 0.0)));
	}

	// Two columns, two layers, ln(2) per occupied voxel. Same-column occlusion: one column
	// holds both layers (T = 0.25), the other is empty (T = 1), so the average is 0.625.
	{
		Volume volume;
		fillTwoLayers(volume, true);
		BeamVolume beam(volume, palette);

		const std::array<i64, 2> lo{ {0, 0} };
		const std::array<i64, 2> hi{ {2, 1} };
		const BeamResult result = beam.query(2, false, lo, hi, 0, 2);

		TASK_CHECK(close(result.transmittance, 0.625));
		TASK_CHECK(close(result.radiance, vec3d(0.75 * kHalf, 0.0, 0.0)));
	}

	// Staggered columns: each column holds a single layer (T = 0.5), so the average is 0.5.
	// Equal mean density, different column correlation, different answer.
	{
		Volume volume;
		fillTwoLayers(volume, false);
		BeamVolume beam(volume, palette);

		const std::array<i64, 2> lo{ {0, 0} };
		const std::array<i64, 2> hi{ {2, 1} };
		const BeamResult result = beam.query(2, false, lo, hi, 0, 2);

		TASK_CHECK(close(result.transmittance, 0.5));
		TASK_CHECK(close(result.radiance, vec3d(kHalf, 0.0, 0.0)));
	}

	// Red nearer than blue versus blue nearer than red: transmittance is identical but the
	// emitted colours are not.
	{
		Volume volume;
		volume.setVoxel(0, 0, 0, 1); // red nearer
		volume.setVoxel(0, 0, 1, 2); // blue farther

		const Palette redBlue = makeRedBluePalette();
		BeamVolume beam(volume, redBlue);

		const std::array<i64, 2> lo{ {0, 0} };
		const std::array<i64, 2> hi{ {1, 1} };

		const BeamResult forward = beam.query(2, false, lo, hi, 0, 2);
		const BeamResult reversed = beam.query(2, true, lo, hi, 0, 2);

		TASK_CHECK(close(forward.transmittance, 0.25));
		TASK_CHECK(close(reversed.transmittance, 0.25));

		TASK_CHECK(close(forward.radiance, vec3d(kHalf, 0.0, 0.5 * kHalf)));
		TASK_CHECK(close(reversed.radiance, vec3d(0.5 * kHalf, 0.0, kHalf)));

		TASK_CHECK(forward.radiance.x > forward.radiance.z);
		TASK_CHECK(reversed.radiance.z > reversed.radiance.x);
		TASK_CHECK(close(forward.radiance.x, reversed.radiance.z));
		TASK_CHECK(close(forward.radiance.z, reversed.radiance.x));
	}

	return 0;
}

static int correlation_matches_reference(void)
{
	const Palette palette = makePalette();
	Volume volume;
		fillComplexScene(volume);
	BeamVolume beam(volume, palette);

	const std::array<std::array<i64, 2>, 6> origins{ {
		{ {-5, -3} }, { {3, 5} }, { {-8, 4} }, { {2, 6} }, { {-2, 0} }, { {-7, -1} }
	} };
	const std::array<std::array<i64, 2>, 4> sizes{ {
		{ {1, 1} }, { {2, 3} }, { {4, 2} }, { {6, 5} }
	} };

	for (int axis = 0; axis < 3; ++axis)
	{
		for (int reverse = 0; reverse < 2; ++reverse)
		{
			for (std::size_t f = 0; f < origins.size(); ++f)
			{
				const std::array<i64, 2> lo = origins[f];
				const std::array<i64, 2> size = sizes[f % sizes.size()];
				const std::array<i64, 2> hi{ {lo[0] + size[0], lo[1] + size[1]} };

				const i64 windows[3][2] = { { -5, 3 }, { -2, 1 }, { 0, 2 } };
				for (const auto& window : windows)
				{
					TASK_CHECK(queryMatchesReference(beam, volume, palette, axis, reverse != 0,
						lo, hi, window[0], window[1]));
				}
			}
		}
	}

	return 0;
}

static int correlation_empty_and_emission(void)
{
	const Palette palette = makePalette();
	Volume volume;
		fillComplexScene(volume);
	BeamVolume beam(volume, palette);

	// Empty footprint and empty depth window return the identity transfer.
	{
		const BeamResult emptyFootprint = beam.query(2, false,
			std::array<i64, 2>{ {0, 0} }, std::array<i64, 2>{ {0, 0} }, 0, 4);
		TASK_CHECK(close(emptyFootprint.transmittance, 1.0));
		TASK_CHECK(close(emptyFootprint.radiance, vec3d(0.0, 0.0, 0.0)));

		const BeamResult emptyDepth = beam.query(0, true,
			std::array<i64, 2>{ {-4, -2} }, std::array<i64, 2>{ {2, 3} }, 2, 2);
		TASK_CHECK(close(emptyDepth.transmittance, 1.0));
		TASK_CHECK(close(emptyDepth.radiance, vec3d(0.0, 0.0, 0.0)));
	}

	// Zero-extinction emission: C = emission * length, T = 1.
	{
		Volume emitter;
		emitter.setVoxel(0, 0, 0, 2);
		emitter.setVoxel(0, 0, 1, 2);
		emitter.setVoxel(0, 0, 2, 2);
		BeamVolume emitterBeam(emitter, palette);

		const BeamResult result = emitterBeam.query(2, false,
			std::array<i64, 2>{ {0, 0} }, std::array<i64, 2>{ {1, 1} }, 0, 3);

		TASK_CHECK(close(result.transmittance, 1.0));
		TASK_CHECK(close(result.radiance, palette[2].emission * 3.0));
	}

	// Background composition: pixel = radiance + transmittance * B.
	{
		Volume twoLayers;
		fillTwoLayers(twoLayers, true);
		BeamVolume twoLayerBeam(twoLayers, makeHalfLifePalette());

		const BeamResult result = twoLayerBeam.query(2, false,
			std::array<i64, 2>{ {0, 0} }, std::array<i64, 2>{ {2, 1} }, 0, 2);

		const vec3d background(0.2, 0.4, 0.6);
		const vec3d pixel = result.radiance + result.transmittance * background;
		TASK_CHECK(close(pixel, vec3d(0.75 * kHalf + 0.625 * 0.2, 0.625 * 0.4, 0.625 * 0.6)));
	}

	return 0;
}

static int correlation_two_objects(void)
{
	const Palette palette = makePalette();

	Volume a;
	a.setVoxel(0, 0, 0, 1);
	a.setVoxel(0, 0, 1, 2);

	Volume b;
	b.setVoxel(0, 0, 0, 3);
	b.setVoxel(1, 0, 0, 1);

	BeamVolume beamA(a, palette);
	BeamVolume beamB(b, palette);

	const std::array<i64, 2> lo{ {0, 0} };
	const std::array<i64, 2> hi{ {2, 1} };

	const BeamResult a1 = beamA.query(2, false, lo, hi, 0, 2);
	const BeamResult b1 = beamB.query(2, false, lo, hi, 0, 2);
	const BeamResult a2 = beamA.query(2, false, lo, hi, 0, 2);
	const BeamResult b2 = beamB.query(2, false, lo, hi, 0, 2);

	TASK_CHECK(close(a1.transmittance, a2.transmittance));
	TASK_CHECK(close(a1.radiance, a2.radiance));
	TASK_CHECK(close(b1.transmittance, b2.transmittance));
	TASK_CHECK(close(b1.radiance, b2.radiance));

	TASK_CHECK(queryMatchesReference(beamA, a, palette, 2, false, lo, hi, 0, 2));
	TASK_CHECK(queryMatchesReference(beamB, b, palette, 2, false, lo, hi, 0, 2));

	// The two objects are not aliased.
	TASK_CHECK(!close(a1.radiance, b1.radiance, 1e-6) ||
		!close(a1.transmittance, b1.transmittance, 1e-6));

	return 0;
}

// ---------------------------------------------------------------------------------------------
// clipping
// ---------------------------------------------------------------------------------------------

static int clipping_depth_windows(void)
{
	const Palette palette = makePalette();
	Volume volume;
		fillComplexScene(volume);
	BeamVolume beam(volume, palette);

	const i64 windows[][2] = {
		{ -5, -3 }, { -4, -1 }, { -2, 2 }, { 0, 1 }, { -3, 0 }, { 1, 4 }, { -6, 5 }, { 2, 3 }, { -5, -4 }
	};

	for (int axis = 0; axis < 3; ++axis)
	{
		for (int reverse = 0; reverse < 2; ++reverse)
		{
			for (const auto& window : windows)
			{
				const std::array<i64, 2> lo{ {-6, -4} };
				const std::array<i64, 2> hi{ {1, 1} };
				TASK_CHECK(queryMatchesReference(beam, volume, palette, axis, reverse != 0,
					lo, hi, window[0], window[1]));
			}
		}
	}

	return 0;
}

static int clipping_transverse_windows(void)
{
	const Palette palette = makePalette();
	Volume volume;
		fillComplexScene(volume);
	BeamVolume beam(volume, palette);

	const i64 boxes[][4] = {
		{ -7, -2, 0, 4 }, { -3, -1, 2, 6 }, { -8, -7, 4, 5 }, { -1, 2, 0, 1 },
		{ -5, 3, -3, 2 }, { -2, 3, 3, 8 }, { 1, 2, -4, -2 }, { -4, 1, 1, 2 }
	};

	for (int axis = 0; axis < 3; ++axis)
	{
		for (const auto& box : boxes)
		{
			const std::array<i64, 2> lo{ {box[0], box[2]} };
			const std::array<i64, 2> hi{ {box[1], box[3]} };
			for (int reverse = 0; reverse < 2; ++reverse)
			{
				TASK_CHECK(queryMatchesReference(beam, volume, palette, axis, reverse != 0,
					lo, hi, -4, 4));
			}
		}
	}

	return 0;
}

static int clipping_invalid_arguments(void)
{
	const Palette palette = makePalette();
	Volume volume;
		fillComplexScene(volume);
	BeamVolume beam(volume, palette);

	const std::array<i64, 2> lo{ {0, 0} };
	const std::array<i64, 2> hi{ {1, 1} };
	const i64 limit = i64(1) << 31;

	auto throws = [&](int axis, const std::array<i64, 2>& a, const std::array<i64, 2>& b, i64 dlo, i64 dhi) {
		try
		{
			beam.query(axis, false, a, b, dlo, dhi);
		}
		catch (const std::invalid_argument&)
		{
			return true;
		}
		catch (...)
		{
			return false;
		}
		return false;
	};

	TASK_CHECK(throws(3, lo, hi, 0, 1));
	TASK_CHECK(throws(-1, lo, hi, 0, 1));
	TASK_CHECK(throws(2, hi, lo, 0, 1));
	TASK_CHECK(throws(2, lo, hi, 2, 1));
	TASK_CHECK(throws(2, lo, hi, -limit - 1, 0));
	TASK_CHECK(throws(2, lo, hi, 0, limit + 1));
	TASK_CHECK(throws(2, std::array<i64, 2>{ {-limit - 1, 0} }, hi, 0, 1));

	// The extreme valid endpoints are accepted.
	const BeamResult result = beam.query(2, false,
		std::array<i64, 2>{ {-limit, -limit} }, std::array<i64, 2>{ {limit, limit} }, -limit, limit);
	TASK_CHECK(std::isfinite(result.transmittance));
	TASK_CHECK(result.transmittance >= 0.0 && result.transmittance <= 1.0);

	return 0;
}

static int clipping_huge_extents(void)
{
	// Tiny extinction over the full 2^32 path length must remain finite and accurate.
	{
		Palette palette = blackPalette();
		palette[1].extinction = 1e-12;
		palette[1].emission = vec3d(1e-6, 0.0, 0.0);

		Volume volume;
		volume.fill(1);

		BeamVolume beam(volume, palette);

		const i64 limit = i64(1) << 31;
		const BeamResult result = beam.query(0, false,
			std::array<i64, 2>{ {-limit, -limit} }, std::array<i64, 2>{ {limit, limit} },
			-limit, limit);

		const double path = 4294967296.0;
		const double expectedT = std::exp(-1e-12 * path);
		TASK_CHECK(std::isfinite(result.transmittance));
		TASK_CHECK(close(result.transmittance, expectedT, 1e-9));
		TASK_CHECK(result.transmittance > 0.0 && result.transmittance < 1.0);

		// C = emission * (1 - T) / extinction. The first-order approximation is inadequate at
		// this optical depth, so the reference keeps the exact expression.
		const double expectedC = 1e-6 * (1.0 - expectedT) / 1e-12;
		TASK_CHECK(std::isfinite(result.radiance.x));
		TASK_CHECK(close(result.radiance.x, expectedC, 1e-9));
	}

	// Extreme but valid palette coefficients must not produce infinities.
	{
		Palette palette = blackPalette();
		palette[1].extinction = 1.0e6;
		palette[1].emission = vec3d(1.0e6, 1.0e6, 0.0);

		Volume volume;
		volume.fill(1);

		BeamVolume beam(volume, palette);

		const i64 limit = i64(1) << 31;
		const BeamResult result = beam.query(2, false,
			std::array<i64, 2>{ {-limit, -limit} }, std::array<i64, 2>{ {limit, limit} },
			-limit, limit);

		TASK_CHECK(std::isfinite(result.transmittance));
		TASK_CHECK(result.transmittance >= 0.0);
		TASK_CHECK(std::isfinite(result.radiance.x));
		TASK_CHECK(std::isfinite(result.radiance.y));
		TASK_CHECK(std::isfinite(result.radiance.z));
	}

	return 0;
}

static int clipping_ppm_output(void)
{
	const std::string path = ".task-build/beam.ppm";

	std::ifstream file(path, std::ios::binary);
	TASK_CHECK(file.is_open());

	std::string magic;
	int width = 0;
	int height = 0;
	int maxValue = 0;
	file >> magic >> width >> height >> maxValue;
	TASK_CHECK(magic == "P6");
	TASK_CHECK(maxValue == 255);
	TASK_CHECK(width >= 32);
	TASK_CHECK(height >= 32);

	file.get(); // Exactly one whitespace byte separates the header from the payload.

	std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 3);
	file.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
	TASK_CHECK(static_cast<std::size_t>(file.gcount()) == pixels.size());

	// The image must contain some non-black pixels, and the scene geometry (not just the
	// background) must contribute to at least some of them.
	bool anyNonBlack = false;
	for (unsigned char value : pixels)
	{
		if (value != 0) { anyNonBlack = true; break; }
	}
	TASK_CHECK(anyNonBlack);

	const vec3d background(0.02, 0.03, 0.05);
	const std::array<unsigned char, 3> backgroundBytes = encodeLinearRGB(background);

	bool anyBrighterThanBackground = false;
	for (std::size_t i = 0; i + 2 < pixels.size(); i += 3)
	{
		if (pixels[i] > backgroundBytes[0] ||
			pixels[i + 1] > backgroundBytes[1] ||
			pixels[i + 2] > backgroundBytes[2])
		{
			anyBrighterThanBackground = true;
			break;
		}
	}
	TASK_CHECK(anyBrighterThanBackground);

	// Independent reference for the whole image, built without using BeamVolume.
	const Palette palette = makeExamplePalette();
	Volume volume;
	fillExampleScene(volume);

	const int exampleWidth = 48;
	const int exampleHeight = 48;
	TASK_CHECK(width == exampleWidth);
	TASK_CHECK(height == exampleHeight);

	// Every pixel must be finite (the encoding maps NaN to zero and clamps the rest).
	bool anyNonBlackReference = false;

	for (int py = 0; py < height; ++py)
	{
		for (int px = 0; px < width; ++px)
		{
			const ReferenceResult reference = referenceBeam(volume, palette, 2, false,
				std::array<i64, 2>{ {px, py} }, std::array<i64, 2>{ {px + 1, py + 1} }, 0, 8);

			const vec3d color = reference.radiance + reference.transmittance * background;
			const unsigned char expected[3] = {
				encodeLinearChannel(color.x),
				encodeLinearChannel(color.y),
				encodeLinearChannel(color.z)
			};

			const std::size_t offset = (static_cast<std::size_t>(py) * width + px) * 3;
			TASK_CHECK(pixels[offset + 0] == expected[0]);
			TASK_CHECK(pixels[offset + 1] == expected[1]);
			TASK_CHECK(pixels[offset + 2] == expected[2]);

			if (expected[0] != 0 || expected[1] != 0 || expected[2] != 0) anyNonBlackReference = true;
		}
	}

	// A second, independent check of the sample at the centre of the image: query the same
	// footprint through the public API and confirm it encodes to the same bytes.
	{
		BeamVolume beam(volume, palette);
		const BeamResult centre = beam.query(2, false,
			std::array<i64, 2>{ {width / 2, height / 2} },
			std::array<i64, 2>{ {width / 2 + 1, height / 2 + 1} }, 0, 8);

		const vec3d color = centre.radiance + centre.transmittance * background;
		const std::size_t offset = (static_cast<std::size_t>(height / 2) * width + width / 2) * 3;
		TASK_CHECK(pixels[offset + 0] == encodeLinearChannel(color.x));
		TASK_CHECK(pixels[offset + 1] == encodeLinearChannel(color.y));
		TASK_CHECK(pixels[offset + 2] == encodeLinearChannel(color.z));
	}

	TASK_CHECK(anyNonBlackReference);
	return 0;
}

// ---------------------------------------------------------------------------------------------
// sharing
// ---------------------------------------------------------------------------------------------

static int sharing_baked_reuse(void)
{
	const Palette palette = makePalette();

	Volume single;
	fillBlock(single, 0, 0, 0, 1);
	BeamVolume singleBeam(single, palette);
	const BeamPrepareStats singleStats = singleBeam.prepareStats();

	// A single-block reference, prepared on an otherwise empty volume.
	Volume emptySource;
	BeamVolume emptyBeam(emptySource, palette);
	const BeamPrepareStats emptyStats = emptyBeam.prepareStats();
	TASK_CHECK(emptyStats.sourceStates < singleStats.sourceStates);

	// Two identical, well-separated blocks. Baking shares the repeated subgraph.
	Volume repeated;
	fillBlock(repeated, 0, 0, 0, 1);
	fillBlock(repeated, 16, 16, 16, 1);
	repeated.bake();

	const u32 nodesBefore = repeated.countNodes();
	BeamVolume repeatedBeam(repeated, palette);
	const BeamPrepareStats repeatedStats = repeatedBeam.prepareStats();

	// Preparing the optical representation must not rebuild or modify the source DAG.
	TASK_CHECK(repeated.countNodes() == nodesBefore);

	// The repeated subgraph is shared: the transfer graph does not grow with the copy.
	TASK_CHECK(repeatedStats.transferNodes == singleStats.transferNodes);
	TASK_CHECK(repeatedStats.sourceStates <= singleStats.sourceStates + 64);

	// Both copies are still rendered correctly.
	const i64 footprintLo[2][2] = { { 0, 0 }, { 16, 16 } };
	const i64 footprintHi[2][2] = { { 4, 4 }, { 20, 20 } };
	for (int i = 0; i < 2; ++i)
	{
		TASK_CHECK(queryMatchesReference(repeatedBeam, repeated, palette, 2, false,
			std::array<i64, 2>{ {footprintLo[i][0], footprintLo[i][1]} },
			std::array<i64, 2>{ {footprintHi[i][0], footprintHi[i][1]} }, 0, 20));
	}

	// Setting the same palette again must not rebuild the optical graph.
	const BeamPrepareStats beforeReset = repeatedBeam.prepareStats();
	TASK_CHECK(repeatedBeam.setPalette(palette));
	Palette copy = palette;
	TASK_CHECK(repeatedBeam.setPalette(copy));
	const BeamPrepareStats afterReset = repeatedBeam.prepareStats();
	TASK_CHECK(afterReset.sourceStates == beforeReset.sourceStates);
	TASK_CHECK(afterReset.transferNodes == beforeReset.transferNodes);

	return 0;
}

static int sharing_scale_and_direction(void)
{
	// Scale: the same material used at different heights yields proportionally longer paths.
	{
		Palette palette = blackPalette();
		palette[1].extinction = 0.25;
		palette[1].emission = vec3d(1.0, 0.0, 0.0);

		Volume small;
		for (int z = 0; z < 2; ++z)
			for (int y = 0; y < 2; ++y)
				for (int x = 0; x < 2; ++x)
					small.setVoxel(x, y, z, 1);

		Volume large;
		for (int z = 0; z < 4; ++z)
			for (int y = 0; y < 4; ++y)
				for (int x = 0; x < 4; ++x)
					large.setVoxel(x, y, z, 1);

		BeamVolume smallBeam(small, palette);
		BeamVolume largeBeam(large, palette);

		const BeamResult smallResult = smallBeam.query(2, false,
			std::array<i64, 2>{ {0, 0} }, std::array<i64, 2>{ {2, 2} }, 0, 2);
		const BeamResult largeResult = largeBeam.query(2, false,
			std::array<i64, 2>{ {0, 0} }, std::array<i64, 2>{ {4, 4} } , 0, 4);

		TASK_CHECK(close(smallResult.transmittance, std::exp(-0.25 * 2.0)));
		TASK_CHECK(close(largeResult.transmittance, std::exp(-0.25 * 4.0)));
		TASK_CHECK(!close(smallResult.transmittance, largeResult.transmittance));

		// A depth window which clips part of the large block must scale by the actual length.
		for (int axis = 0; axis < 3; ++axis)
		{
			const BeamResult clipped = largeBeam.query(axis, false,
				std::array<i64, 2>{ {0, 0} }, std::array<i64, 2>{ {4, 4} }, 1, 3);
			TASK_CHECK(close(clipped.transmittance, std::exp(-0.25 * 2.0)));
		}
	}

	// Direction: forward and reverse share the same source nodes and axis, but must not share
	// a cache entry because the composition order differs.
	{
		Volume volume;
		volume.setVoxel(0, 0, 0, 1);
		volume.setVoxel(0, 0, 1, 2);

		BeamVolume beam(volume, makeRedBluePalette());

		const std::array<i64, 2> lo{ {0, 0} };
		const std::array<i64, 2> hi{ {1, 1} };
		const BeamResult forward = beam.query(2, false, lo, hi, 0, 2);
		const BeamResult reversed = beam.query(2, true, lo, hi, 0, 2);

		TASK_CHECK(!close(forward.radiance, reversed.radiance, 1e-6));

		// Repeat both orders and verify they are stable (the cache is keyed by direction).
		const BeamResult forward2 = beam.query(2, false, lo, hi, 0, 2);
		const BeamResult reversed2 = beam.query(2, true, lo, hi, 0, 2);
		TASK_CHECK(close(forward.radiance, forward2.radiance));
		TASK_CHECK(close(reversed.radiance, reversed2.radiance));
	}

	return 0;
}

static int sharing_uniform_limits(void)
{
	Palette palette = blackPalette();
	palette[1].extinction = 0.5;
	palette[1].emission = vec3d(0.3, 0.6, 0.2);

	Volume volume;
	volume.fill(1);

	const u32 nodesBefore = volume.countNodes();
	BeamVolume beam(volume, palette);
	const BeamPrepareStats prepared = beam.prepareStats();

	TASK_CHECK(prepared.sourceStates <= 512);
	TASK_CHECK(prepared.transferNodes <= 512);
	TASK_CHECK(volume.countNodes() == nodesBefore);

	const std::array<i64, 2> los[] = {
		{ {-10, -10} }, { {0, 0} }, { {-1000000, 5} }, { {-(i64(1) << 31), 0} }
	};
	const std::array<i64, 2> his[] = {
		{ {10, 10} }, { {1, 1} }, { {1000000, 6} }, { {0, (i64(1) << 31)} }
	};
	const i64 windows[][2] = {
		{ 0, 0 }, { -5, 5 }, { -1000000, 1000000 }, { -(i64(1) << 31), (i64(1) << 31) }
	};

	for (int axis = 0; axis < 3; ++axis)
	{
		for (int reverse = 0; reverse < 2; ++reverse)
		{
			for (const auto& lo : los)
			{
				for (const auto& hi : his)
				{
					for (const auto& window : windows)
					{
						// Skip pairs where hi < lo: those correctly throw, and are covered
						// by clipping_invalid_arguments.
						if (std::max(lo[0], lo[1]) >= std::min(hi[0], hi[1])) continue;

						BeamStats stats;
						const BeamResult result = beam.query(axis, reverse != 0, lo, hi, window[0], window[1], &stats);
						TASK_CHECK(stats.sourceVisits <= 256);
						TASK_CHECK(stats.transferVisits <= 256);
						TASK_CHECK(result.transmittance >= 0.0 && result.transmittance <= 1.0);
						TASK_CHECK(std::isfinite(result.radiance.x));
					}
				}
			}
		}
	}

	// Querying must not rebuild the persistent representation.
	const BeamPrepareStats after = beam.prepareStats();
	TASK_CHECK(after.sourceStates == prepared.sourceStates);
	TASK_CHECK(after.transferNodes == prepared.transferNodes);
	TASK_CHECK(volume.countNodes() == nodesBefore);

	return 0;
}

static int sharing_palette_failure_keeps_state(void)
{
	const Palette palette = makePalette();
	Volume volume;
		fillComplexScene(volume);
	BeamVolume beam(volume, palette);

	const std::array<i64, 2> lo{ {-4, -2} };
	const std::array<i64, 2> hi{ {2, 3} };

	const BeamResult before = beam.query(2, false, lo, hi, -4, 4);
	const BeamPrepareStats beforeStats = beam.prepareStats();

	// Material zero must be transparent.
	{
		Palette bad = palette;
		bad[0].extinction = 0.1;
		TASK_CHECK(!beam.setPalette(bad));
	}
	{
		Palette bad = palette;
		bad[0].emission.x = 1.0;
		TASK_CHECK(!beam.setPalette(bad));
	}

	// Non-finite and out-of-range coefficients must be rejected.
	{
		Palette bad = palette;
		bad[5].extinction = std::numeric_limits<double>::infinity();
		TASK_CHECK(!beam.setPalette(bad));
	}
	{
		Palette bad = palette;
		bad[5].emission.y = std::numeric_limits<double>::quiet_NaN();
		TASK_CHECK(!beam.setPalette(bad));
	}
	{
		Palette bad = palette;
		bad[5].extinction = 1.0e6 + 1.0;
		TASK_CHECK(!beam.setPalette(bad));
	}
	{
		Palette bad = palette;
		bad[7].emission.z = -0.001;
		TASK_CHECK(!beam.setPalette(bad));
	}

	// The previous result and statistics are retained.
	const BeamResult after = beam.query(2, false, lo, hi, -4, 4);
	TASK_CHECK(close(after.transmittance, before.transmittance));
	TASK_CHECK(close(after.radiance, before.radiance));
	const BeamPrepareStats afterStats = beam.prepareStats();
	TASK_CHECK(afterStats.sourceStates == beforeStats.sourceStates);
	TASK_CHECK(afterStats.transferNodes == beforeStats.transferNodes);

	// A valid palette is published and changes the image.
	Palette brighter = palette;
	brighter[1].emission = vec3d(5.0, 5.0, 5.0);
	TASK_CHECK(beam.setPalette(brighter));
	const BeamResult changed = beam.query(2, false, lo, hi, -4, 4);
	TASK_CHECK(!close(changed.radiance, before.radiance, 1e-6));

	// The invalid palette must also never be observable through palette().
	Palette bad = palette;
	bad[0].extinction = 0.5;
	TASK_CHECK(!beam.setPalette(bad));
	TASK_CHECK(beam.palette()[0].extinction == 0.0);

	return 0;
}

static int sharing_source_untouched(void)
{
	const Palette palette = makePalette();
	Volume volume;
		fillComplexScene(volume);

	const u32 nodesBefore = volume.countNodes();
	const MaterialId sampleBefore = volume.voxel(0, 0, 0);

	{
		BeamVolume beam(volume, palette);
		TASK_CHECK(queryMatchesReference(beam, volume, palette, 0, false,
			std::array<i64, 2>{ {0, 0} }, std::array<i64, 2>{ {4, 4} }, -3, 3));

		BeamStats stats;
		beam.query(1, true, std::array<i64, 2>{ {-6, -4} }, std::array<i64, 2>{ {1, 1} }, -4, 4, &stats);
		TASK_CHECK(stats.sourceVisits > 0 || stats.transferVisits > 0);
	}

	TASK_CHECK(volume.countNodes() == nodesBefore);
	TASK_CHECK(volume.voxel(0, 0, 0) == sampleBefore);

	// A baked source is equally unaffected by preparing an optical representation.
	Volume baked;
	fillComplexScene(baked);
	baked.bake();
	const u32 bakedBefore = baked.countNodes();
	const MaterialId bakedSample = baked.voxel(-3, -3, 1);
	{
		BeamVolume beam(baked, palette);
		(void)beam.prepareStats();
	}
	TASK_CHECK(baked.countNodes() == bakedBefore);
	TASK_CHECK(baked.voxel(-3, -3, 1) == bakedSample);

	return 0;
}

const task_case task_cases[] = {
	{"correlation", "basic_equations", correlation_basic_equations},
	{"correlation", "matches_reference", correlation_matches_reference},
	{"correlation", "empty_and_emission", correlation_empty_and_emission},
	{"correlation", "two_objects", correlation_two_objects},
	{"clipping", "depth_windows", clipping_depth_windows},
	{"clipping", "transverse_windows", clipping_transverse_windows},
	{"clipping", "invalid_arguments", clipping_invalid_arguments},
	{"clipping", "huge_extents", clipping_huge_extents},
	{"clipping", "ppm_output", clipping_ppm_output},
	{"sharing", "baked_reuse", sharing_baked_reuse},
	{"sharing", "scale_and_direction", sharing_scale_and_direction},
	{"sharing", "uniform_limits", sharing_uniform_limits},
	{"sharing", "palette_failure_keeps_state", sharing_palette_failure_keeps_state},
	{"sharing", "source_untouched", sharing_source_untouched},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
