/***************************************************************************************************
* Regression tests for Volume::combineTranslated().                                                *
*                                                                                                  *
* Registered through task_support/check.h (no main() here). The small scenarios keep an            *
* independent dense reference array which is computed from plain integer arithmetic, so the        *
* compressed tree is checked voxel by voxel rather than against itself.                            *
***************************************************************************************************/
#include "task_support/check.h"
#include "raytracing.h"
#include "storage.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Cubiquity;

namespace
{
	// Dense reference grid over a small coordinate window.
	struct RefGrid
	{
		std::array<i32, 3> lo;
		std::array<i32, 3> hi;
		std::vector<MaterialId> cells;

		RefGrid(i32 lox, i32 loy, i32 loz, i32 hix, i32 hiy, i32 hiz)
			: lo{ lox, loy, loz }, hi{ hix, hiy, hiz }
		{
			const i64 n = static_cast<i64>(hix - lox) * (hiy - loy) * (hiz - loz);
			cells.resize(static_cast<size_t>(n), 0);
		}

		bool inside(i32 x, i32 y, i32 z) const
		{
			return x >= lo[0] && x < hi[0] && y >= lo[1] && y < hi[1] && z >= lo[2] && z < hi[2];
		}

		size_t index(i32 x, i32 y, i32 z) const
		{
			const i32 sx = hi[0] - lo[0];
			const i32 sy = hi[1] - lo[1];
			return static_cast<size_t>((z - lo[2]) * sx * sy + (y - lo[1]) * sx + (x - lo[0]));
		}

		MaterialId at(i32 x, i32 y, i32 z) const { return cells[index(x, y, z)]; }
		void set(i32 x, i32 y, i32 z, MaterialId m) { cells[index(x, y, z)] = m; }
	};

	constexpr std::array<i32, 3> WindowLo = { -6, -6, -6 };
	constexpr std::array<i32, 3> WindowHi = { 7, 7, 7 };

	// Source pattern inside the source box [0,4)^3; deliberately produces zeros as well.
	MaterialId sourcePattern(i32 x, i32 y, i32 z)
	{
		return static_cast<MaterialId>((x + 2 * y + 3 * z) % 7);
	}

	// Initial destination pattern over the window; never zero so source-zero edits are visible.
	MaterialId destPattern(i32 x, i32 y, i32 z)
	{
		return static_cast<MaterialId>(1 + ((x + 3 * y + 5 * z) % 4 + 4) % 4);
	}

	// Deterministic, non-commutative combiner; f(a, 0) still differs from a, so a call with a
	// zero source material can be distinguished from "combiner never ran".
	MaterialId combineFn(MaterialId a, MaterialId b)
	{
		return static_cast<MaterialId>((a * 7 + b + 1) & 0xff);
	}

	bool insideSourceBox(i32 x, i32 y, i32 z)
	{
		return x >= 0 && x < 4 && y >= 0 && y < 4 && z >= 0 && z < 4;
	}

	RefGrid makeDestReference()
	{
		RefGrid ref(WindowLo[0], WindowLo[1], WindowLo[2], WindowHi[0], WindowHi[1], WindowHi[2]);
		for (i32 z = WindowLo[2]; z < WindowHi[2]; z++)
			for (i32 y = WindowLo[1]; y < WindowHi[1]; y++)
				for (i32 x = WindowLo[0]; x < WindowHi[0]; x++)
					ref.set(x, y, z, destPattern(x, y, z));
		return ref;
	}

	void buildDestVolume(Volume& vol)
	{
		vol.fill(0);
		for (i32 z = WindowLo[2]; z < WindowHi[2]; z++)
			for (i32 y = WindowLo[1]; y < WindowHi[1]; y++)
				for (i32 x = WindowLo[0]; x < WindowHi[0]; x++)
					vol.setVoxel(x, y, z, destPattern(x, y, z));
	}

	void buildSourceVolume(Volume& vol)
	{
		vol.fill(0);
		for (i32 z = 0; z < 4; z++)
			for (i32 y = 0; y < 4; y++)
				for (i32 x = 0; x < 4; x++)
				{
					const MaterialId m = sourcePattern(x, y, z);
					if (m != 0) { vol.setVoxel(x, y, z, m); }
				}
	}

	// Expected result of combineTranslated over the window, evaluated from call-start values.
	RefGrid expectedAfterTranslate(const RefGrid& initial,
		const std::array<i64, 3>& lo, const std::array<i64, 3>& hi,
		const std::array<i64, 3>& delta)
	{
		RefGrid expected = initial;
		for (i32 z = WindowLo[2]; z < WindowHi[2]; z++)
			for (i32 y = WindowLo[1]; y < WindowHi[1]; y++)
				for (i32 x = WindowLo[0]; x < WindowHi[0]; x++)
				{
					const i64 qx = static_cast<i64>(x) - delta[0];
					const i64 qy = static_cast<i64>(y) - delta[1];
					const i64 qz = static_cast<i64>(z) - delta[2];
					if (qx >= lo[0] && qx < hi[0] && qy >= lo[1] && qy < hi[1] &&
						qz >= lo[2] && qz < hi[2])
					{
						const MaterialId src = sourcePattern(
							static_cast<i32>(qx), static_cast<i32>(qy), static_cast<i32>(qz));
						expected.set(x, y, z, combineFn(initial.at(x, y, z), src));
					}
				}
		return expected;
	}

	bool volumeMatchesGrid(const Volume& vol, const RefGrid& ref)
	{
		for (i32 z = WindowLo[2]; z < WindowHi[2]; z++)
			for (i32 y = WindowLo[1]; y < WindowHi[1]; y++)
				for (i32 x = WindowLo[0]; x < WindowHi[0]; x++)
				{
					if (vol.voxel(x, y, z) != ref.at(x, y, z)) { return false; }
				}
		return true;
	}

	const std::array<i64, 3> SourceLo = { 0, 0, 0 };
	const std::array<i64, 3> SourceHi = { 4, 4, 4 };
}

// ------------------------------------------------------------------------------------------------
// placement group: cross-store pastes with odd positive/negative translations, dense verification
// ------------------------------------------------------------------------------------------------

static int placement_odd_translations()
{
	const std::array<i64, 3> deltas[] = {
		{ 3, 5, -7 }, { -3, -5, 7 }, { 1, 1, 1 }, { -1, -1, -1 },
		{ 7, -9, 11 }, { -11, 9, -7 }
	};

	for (const auto& delta : deltas)
	{
		Volume dst, src;
		buildDestVolume(dst);
		buildSourceVolume(src);
		const RefGrid initial = makeDestReference();
		const RefGrid expected = expectedAfterTranslate(initial, SourceLo, SourceHi, delta);

		CombineStats stats;
		dst.combineTranslated(src, SourceLo, SourceHi, delta, combineFn, &stats);

		TASK_CHECK(volumeMatchesGrid(dst, expected));
		TASK_CHECK(stats.region_visits > 0);
		// The source must be untouched by the operation.
		TASK_CHECK(src.voxel(1, 2, 3) == sourcePattern(1, 2, 3));
	}
	return 0;
}

static int placement_zero_source_material_edits_target()
{
	// A zero source material still invokes the combiner and must change the destination.
	Volume dst, src;
	dst.fill(5);
	src.fill(0);
	const auto plusOne = [](MaterialId a, MaterialId b) { return static_cast<MaterialId>(a + b + 1); };

	const std::array<i64, 3> lo = { -2, -2, -2 };
	const std::array<i64, 3> hi = { 3, 3, 3 };
	const std::array<i64, 3> delta = { 1, -1, 3 }; // Odd, mixed-sign translation.
	dst.combineTranslated(src, lo, hi, delta, plusOne);

	for (i32 z = 1; z < 6; z++)
		for (i32 y = -3; y < 2; y++)
			for (i32 x = -1; x < 4; x++)
				TASK_CHECK(dst.voxel(x, y, z) == 6);
	TASK_CHECK(dst.voxel(5, 5, 5) == 5);     // Outside the mapped box.
	TASK_CHECK(dst.voxel(-2, -3, -2) == 5);   // Just outside.
	TASK_CHECK(dst.voxel(-1, -3, 0) == 5);    // One below the mapped z range.
	TASK_CHECK(dst.voxel(-1, 2, 1) == 5);     // One above the mapped y range.
	TASK_CHECK(dst.voxel(4, -3, 1) == 5);     // One above the mapped x range.
	return 0;
}

static int placement_non_commutative_order()
{
	// The first combiner argument is the old destination value, the second the old source value.
	TASK_CHECK(combineFn(3, 5) != combineFn(5, 3));

	Volume dst, src;
	dst.fill(0);
	src.fill(0);
	dst.setVoxel(0, 0, 0, 3);
	src.setVoxel(0, 0, 0, 5);
	const std::array<i64, 3> lo = { 0, 0, 0 };
	const std::array<i64, 3> hi = { 1, 1, 1 };
	const std::array<i64, 3> delta = { 0, 0, 0 };
	dst.combineTranslated(src, lo, hi, delta, combineFn);
	TASK_CHECK(dst.voxel(0, 0, 0) == combineFn(3, 5));
	TASK_CHECK(dst.voxel(0, 0, 0) != combineFn(5, 3));
	return 0;
}

static int placement_empty_box_and_stats_reset()
{
	Volume dst, src;
	buildDestVolume(dst);
	buildSourceVolume(src);
	const u32 rootBefore = dst.rootNodeIndex();

	CombineStats stats;
	const std::array<i64, 3> empty = { 2, 2, 2 };
	const std::array<i64, 3> delta = { 1, 1, 1 };
	dst.combineTranslated(src, empty, empty, delta, combineFn, &stats);
	TASK_CHECK(stats.region_visits == 0);
	TASK_CHECK(stats.allocated_nodes == 0);
	TASK_CHECK(dst.rootNodeIndex() == rootBefore);

	const RefGrid initial = makeDestReference();
	dst.combineTranslated(src, SourceLo, SourceHi, delta, combineFn, &stats);
	TASK_CHECK(stats.region_visits > 0);
	TASK_CHECK(stats.allocated_nodes > 0);

	// Counters start from zero again on every call.
	dst.combineTranslated(src, empty, empty, delta, combineFn, &stats);
	TASK_CHECK(stats.region_visits == 0);
	TASK_CHECK(stats.allocated_nodes == 0);

	const RefGrid expected = expectedAfterTranslate(initial, SourceLo, SourceHi, delta);
	TASK_CHECK(volumeMatchesGrid(dst, expected));
	return 0;
}

static int placement_i32_edge_clipping()
{
	const auto add = [](MaterialId a, MaterialId b) { return static_cast<MaterialId>(a + b); };
	constexpr i32 kMin = INT32_MIN;
	constexpr i32 kMax = INT32_MAX;

	// Low end: partial clip leaves exactly the INT_MIN cell participating, no wrap to INT_MAX.
	{
		Volume dst, src;
		dst.fill(0);
		src.fill(0);
		for (i32 dz = 0; dz < 2; dz++)
			src.setVoxel(kMin + dz, kMin + dz, kMin + dz, 9);
		const std::array<i64, 3> lo = { kMin, kMin, kMin };
		const std::array<i64, 3> hi = { static_cast<i64>(kMin) + 2,
		                                static_cast<i64>(kMin) + 2,
		                                static_cast<i64>(kMin) + 2 };
		const std::array<i64, 3> delta = { -1, -1, -1 };
		dst.combineTranslated(src, lo, hi, delta, add);
		TASK_CHECK(dst.voxel(kMin, kMin, kMin) == 9);
		TASK_CHECK(dst.voxel(kMin + 1, kMin + 1, kMin + 1) == 0);
		TASK_CHECK(dst.voxel(kMax, kMax, kMax) == 0); // No wraparound.
	}

	// Low end: fully clipped, nothing changes.
	{
		Volume dst, src;
		dst.fill(2);
		src.fill(9);
		const std::array<i64, 3> lo = { kMin, kMin, kMin };
		const std::array<i64, 3> hi = { static_cast<i64>(kMin) + 2,
		                                static_cast<i64>(kMin) + 2,
		                                static_cast<i64>(kMin) + 2 };
		const std::array<i64, 3> delta = { -4, -4, -4 };
		CombineStats stats;
		dst.combineTranslated(src, lo, hi, delta, add, &stats);
		TASK_CHECK(dst.voxel(kMin, kMin, kMin) == 2);
		TASK_CHECK(stats.allocated_nodes == 0);
	}

	// High end: hi == 2^31 is a legal half-open bound; partial clip keeps only INT_MAX.
	{
		Volume dst, src;
		dst.fill(0);
		src.fill(0);
		src.setVoxel(kMax - 1, kMax - 1, kMax - 1, 9);
		src.setVoxel(kMax, kMax, kMax, 9);
		const std::array<i64, 3> lo = { static_cast<i64>(kMax) - 1,
		                                static_cast<i64>(kMax) - 1,
		                                static_cast<i64>(kMax) - 1 };
		const std::array<i64, 3> hi = { static_cast<i64>(kMax) + 1,
		                                static_cast<i64>(kMax) + 1,
		                                static_cast<i64>(kMax) + 1 };
		const std::array<i64, 3> delta = { 1, 1, 1 };
		dst.combineTranslated(src, lo, hi, delta, add);
		TASK_CHECK(dst.voxel(kMax, kMax, kMax) == 9);
		TASK_CHECK(dst.voxel(kMax - 1, kMax - 1, kMax - 1) == 0);
		TASK_CHECK(dst.voxel(kMin, kMin, kMin) == 0); // No wraparound.
	}

	// High end: fully clipped.
	{
		Volume dst, src;
		dst.fill(2);
		src.fill(9);
		const std::array<i64, 3> lo = { kMax, kMax, kMax };
		const std::array<i64, 3> hi = { static_cast<i64>(kMax) + 1,
		                                static_cast<i64>(kMax) + 1,
		                                static_cast<i64>(kMax) + 1 };
		const std::array<i64, 3> delta = { 4, 4, 4 };
		dst.combineTranslated(src, lo, hi, delta, add);
		TASK_CHECK(dst.voxel(kMax, kMax, kMax) == 2);
	}
	return 0;
}

static int placement_large_aligned_box_stats()
{
	// The reference scenario from the task: solid source (6) over solid destination (1),
	// box [-2^20, 0)^3 translated by (2^21, 0, 0), combiner sums the materials.
	Volume dst, src;
	dst.fill(1);
	src.fill(6);
	const i64 edge = INT64_C(1) << 20;
	const std::array<i64, 3> lo = { -edge, -edge, -edge };
	const std::array<i64, 3> hi = { 0, 0, 0 };
	const std::array<i64, 3> delta = { INT64_C(1) << 21, 0, 0 };
	const auto sum = [](MaterialId a, MaterialId b) { return static_cast<MaterialId>(a + b); };

	CombineStats stats;
	dst.combineTranslated(src, lo, hi, delta, sum, &stats);
	TASK_CHECK(stats.region_visits <= 8192);
	TASK_CHECK(stats.allocated_nodes <= 8192);

	// Inside the mapped box: 1 + 6 = 7. Corners and interior points.
	TASK_CHECK(dst.voxel(static_cast<i32>(edge), static_cast<i32>(-edge), static_cast<i32>(-edge)) == 7);
	TASK_CHECK(dst.voxel(static_cast<i32>((INT64_C(1) << 21) - 1), -1, -1) == 7);
	TASK_CHECK(dst.voxel(0, 0, 0) == 1); // Box starts at x = 2^20, so the origin is outside.

	// Outside the mapped box: unchanged material 1.
	TASK_CHECK(dst.voxel(static_cast<i32>(edge) - 1, -1, -1) == 1);
	TASK_CHECK(dst.voxel(static_cast<i32>(INT64_C(1) << 21), 0, 0) == 1);
	TASK_CHECK(dst.voxel(static_cast<i32>(edge), static_cast<i32>(-edge) - 1, 0) == 1);
	TASK_CHECK(dst.voxel(static_cast<i32>(edge), 0, 0) == 1);
	return 0;
}

// ------------------------------------------------------------------------------------------------
// aliasing group: source and destination are the same volume, regions may overlap
// ------------------------------------------------------------------------------------------------

static int aliasing_self_overlap_odd_delta()
{
	Volume vol;
	buildDestVolume(vol);
	for (i32 z = 0; z < 4; z++)
		for (i32 y = 0; y < 4; y++)
			for (i32 x = 0; x < 4; x++)
				vol.setVoxel(x, y, z, sourcePattern(x, y, z));

	// Snapshot of every value at the start of the call (small window, plain voxel reads).
	RefGrid initial(WindowLo[0], WindowLo[1], WindowLo[2], WindowHi[0], WindowHi[1], WindowHi[2]);
	for (i32 z = WindowLo[2]; z < WindowHi[2]; z++)
		for (i32 y = WindowLo[1]; y < WindowHi[1]; y++)
			for (i32 x = WindowLo[0]; x < WindowHi[0]; x++)
				initial.set(x, y, z, vol.voxel(x, y, z));

	const std::array<i64, 3> delta = { 3, -5, 1 }; // Overlaps the source box, odd components.
	vol.combineTranslated(vol, SourceLo, SourceHi, delta, combineFn);

	// Reference is computed purely from start-of-call values: any left-to-right smear fails.
	for (i32 z = WindowLo[2]; z < WindowHi[2]; z++)
		for (i32 y = WindowLo[1]; y < WindowHi[1]; y++)
			for (i32 x = WindowLo[0]; x < WindowHi[0]; x++)
			{
				MaterialId expected = initial.at(x, y, z);
				const i64 qx = static_cast<i64>(x) - delta[0];
				const i64 qy = static_cast<i64>(y) - delta[1];
				const i64 qz = static_cast<i64>(z) - delta[2];
				if (insideSourceBox(static_cast<i32>(qx), static_cast<i32>(qy), static_cast<i32>(qz)))
				{
					expected = combineFn(expected,
						initial.at(static_cast<i32>(qx), static_cast<i32>(qy), static_cast<i32>(qz)));
				}
				TASK_CHECK(vol.voxel(x, y, z) == expected);
			}
	return 0;
}

static int aliasing_zero_delta_matches_aligned_combine()
{
	// A zero-displacement combineTranslated over a box must agree with the legacy combine()
	// inside that box, while leaving everything outside the box untouched.
	Volume a, b, sourceA, sourceB;
	buildDestVolume(a);
	buildDestVolume(b);
	buildSourceVolume(sourceA);
	buildSourceVolume(sourceB);
	const RefGrid initial = makeDestReference();

	a.combineTranslated(sourceA, SourceLo, SourceHi,
		std::array<i64, 3>{ 0, 0, 0 }, combineFn);
	b.combine(sourceB, combineFn);

	for (i32 z = WindowLo[2]; z < WindowHi[2]; z++)
		for (i32 y = WindowLo[1]; y < WindowHi[1]; y++)
			for (i32 x = WindowLo[0]; x < WindowHi[0]; x++)
			{
				if (insideSourceBox(x, y, z))
				{
					TASK_CHECK(a.voxel(x, y, z) == b.voxel(x, y, z));
				}
				else
				{
					TASK_CHECK(a.voxel(x, y, z) == initial.at(x, y, z));
				}
			}
	return 0;
}

static int aliasing_self_shift_no_smear()
{
	// Classic smear hazard: shift a strip one voxel along +x inside the same volume. A correct
	// implementation reads all source values from the start state, so the pattern is copied
	// rather than extended.
	Volume vol;
	vol.fill(0);
	for (i32 x = 0; x < 4; x++)
		vol.setVoxel(x, 0, 0, static_cast<MaterialId>(x + 1)); // 1,2,3,4

	const std::array<i64, 3> lo = { 0, 0, 0 };
	const std::array<i64, 3> hi = { 4, 1, 1 };
	const std::array<i64, 3> delta = { 1, 0, 0 };
	const auto overwrite = [](MaterialId, MaterialId b) { return b; };
	vol.combineTranslated(vol, lo, hi, delta, overwrite);

	// Start values were [1,2,3,4] at x=0..3; destination x=1..4 gets those same start values.
	TASK_CHECK(vol.voxel(0, 0, 0) == 1); // Unmapped.
	TASK_CHECK(vol.voxel(1, 0, 0) == 1);
	TASK_CHECK(vol.voxel(2, 0, 0) == 2);
	TASK_CHECK(vol.voxel(3, 0, 0) == 3);
	TASK_CHECK(vol.voxel(4, 0, 0) == 4);
	TASK_CHECK(vol.voxel(5, 0, 0) == 0);
	return 0;
}

// ------------------------------------------------------------------------------------------------
// sparse group: distinct stores, source lifetime, sharing, rays through the real traversal
// ------------------------------------------------------------------------------------------------

static int sparse_cross_store_source_destroyed()
{
	Volume dst;
	buildDestVolume(dst);
	const RefGrid initial = makeDestReference();
	const std::array<i64, 3> delta = { -5, 3, -1 };

	{
		Volume src;
		buildSourceVolume(src);
		dst.combineTranslated(src, SourceLo, SourceHi, delta, combineFn);
	} // src destroyed here; dst must keep working with no dangling references.

	RefGrid expected = expectedAfterTranslate(initial, SourceLo, SourceHi, delta);
	TASK_CHECK(volumeMatchesGrid(dst, expected));

	// Continue editing with the ordinary API.
	dst.setVoxel(6, 6, 6, 42);
	TASK_CHECK(dst.voxel(6, 6, 6) == 42);
	dst.setVoxel(6, 6, 6, 0);
	TASK_CHECK(dst.voxel(6, 6, 6) == 0);
	expected.set(6, 6, 6, 0); // Keep the dense reference in step with the manual edit.

	// Legacy combine still sees the pasted data (adding an empty volume changes nothing).
	Volume empty;
	empty.fill(0);
	const auto add = [](MaterialId a, MaterialId b) { return static_cast<MaterialId>(a + b); };
	dst.combine(empty, add);
	TASK_CHECK(volumeMatchesGrid(dst, expected));
	return 0;
}

static int sparse_shared_subgraphs_preserved_and_reused()
{
	// Build a source with a repeated structured pattern, then bake so identical subtrees merge.
	Volume src;
	src.fill(0);
	const i32 anchors[] = { -16, 16 };
	for (i32 ax : anchors)
		for (i32 ay : anchors)
			for (i32 az : anchors)
				for (i32 dz = 0; dz < 2; dz++)
					for (i32 dy = 0; dy < 2; dy++)
						for (i32 dx = 0; dx < 2; dx++)
							src.setVoxel(ax + dx, ay + dy, az + dz,
								static_cast<MaterialId>(1 + dx + 2 * dy + 4 * dz));
	src.bake();
	const u32 distinctSourceNodes = src.countNodes();
	TASK_CHECK(distinctSourceNodes > 0);

	// Full-domain paste into a uniform destination: every distinct source node is imported once.
	Volume dst;
	dst.fill(1);
	const std::array<i64, 3> lo = { INT64_C(-2147483648), INT64_C(-2147483648), INT64_C(-2147483648) };
	const std::array<i64, 3> hi = { INT64_C(2147483648), INT64_C(2147483648), INT64_C(2147483648) };
	const std::array<i64, 3> delta = { 0, 0, 0 };
	CombineStats stats;
	dst.combineTranslated(src, lo, hi, delta, combineFn, &stats);

	// One appended target node per distinct internal source node (repeated subgraphs reused).
	TASK_CHECK(stats.allocated_nodes == distinctSourceNodes);
	for (i32 ax : anchors)
		for (i32 ay : anchors)
			for (i32 az : anchors)
				for (i32 dz = 0; dz < 2; dz++)
					for (i32 dy = 0; dy < 2; dy++)
						for (i32 dx = 0; dx < 2; dx++)
						{
							const MaterialId sm = static_cast<MaterialId>(1 + dx + 2 * dy + 4 * dz);
							TASK_CHECK(dst.voxel(ax + dx, ay + dy, az + dz) == combineFn(1, sm));
						}

	// Paste the full source again, then re-paste just one block with a different combiner. The
	// other blocks share imported subgraphs with the first paste and must survive untouched.
	Volume dst2;
	dst2.fill(1);
	dst2.combineTranslated(src, lo, hi, delta, combineFn);
	const auto multiply = [](MaterialId a, MaterialId b) { return static_cast<MaterialId>(a * b); };
	const std::array<i64, 3> partLo = { 16, 16, 16 };
	const std::array<i64, 3> partHi = { 18, 18, 18 };
	dst2.combineTranslated(src, partLo, partHi, std::array<i64, 3>{ 0, 0, 0 }, multiply);

	auto blockMaterial = [&](i32 ax, i32 ay, i32 az, i32 dx, i32 dy, i32 dz) {
		return static_cast<MaterialId>(1 + dx + 2 * dy + 4 * dz);
	};
	for (i32 dz = 0; dz < 2; dz++)
		for (i32 dy = 0; dy < 2; dy++)
			for (i32 dx = 0; dx < 2; dx++)
			{
				const MaterialId sm = blockMaterial(16, 16, 16, dx, dy, dz);
				// Re-pasted block: multiply applied on top of the first combine.
				TASK_CHECK(dst2.voxel(16 + dx, 16 + dy, 16 + dz) ==
					multiply(combineFn(1, sm), sm));
				// Untouched blocks still carry only the first combine (shared subgraphs intact).
				TASK_CHECK(dst2.voxel(-16 + dx, -16 + dy, -16 + dz) == combineFn(1, sm));
				TASK_CHECK(dst2.voxel(-16 + dx, 16 + dy, 16 + dz) == combineFn(1, sm));
			}
	// The independently pasted volume is unaffected by the second operation.
	TASK_CHECK(dst.voxel(16, 16, 16) == combineFn(1, blockMaterial(16, 16, 16, 0, 0, 0)));
	return 0;
}

static int sparse_ray_cross_check()
{
	// Paste a 3^3 block of material 9 with an odd translation, then cross-check with a freshly
	// prepared SubDAG array and the real ESVO ray traversal. Voxel with integer coordinate k
	// spans [k-0.5, k+0.5); the ray starts at z = 0.25.
	Volume dst, src;
	dst.fill(0);
	src.fill(0);
	for (i32 z = 10; z < 13; z++)
		for (i32 y = 10; y < 13; y++)
			for (i32 x = 10; x < 13; x++)
				src.setVoxel(x, y, z, 9);
	const std::array<i64, 3> lo = { 10, 10, 10 };
	const std::array<i64, 3> hi = { 13, 13, 13 };
	const std::array<i64, 3> delta = { 1, 2, 3 }; // Block lands at x[11,14), y[12,15), z[13,16).
	const auto overwrite = [](MaterialId, MaterialId b) { return b; };
	dst.combineTranslated(src, lo, hi, delta, overwrite);

	auto subDAGs = findSubDAGs(dst.mNodeStore, dst.rootNodeIndex());
	RayVolumeIntersection hit = intersectVolume(dst, subDAGs, 12.25f, 13.25f, 0.25f, 0, 0, 1, true);
	TASK_CHECK(hit.hit);
	TASK_CHECK(hit.material == 9);
	TASK_CHECK(std::abs(hit.distance - 12.25) < 1e-4); // Entry face of voxel z=13 minus origin.

	// A ray through an empty region misses, rather than hitting a query-overlay artefact.
	RayVolumeIntersection miss = intersectVolume(dst, subDAGs, 0.25f, 0.25f, 0.25f, 0, 0, 1, true);
	TASK_CHECK(!miss.hit);

	// Edit again and re-prepare the SubDAGs: the new ray result must reflect both operations.
	dst.setVoxel(12, 13, 4, 7);
	subDAGs = findSubDAGs(dst.mNodeStore, dst.rootNodeIndex());
	RayVolumeIntersection hit2 = intersectVolume(dst, subDAGs, 12.25f, 13.25f, 0.25f, 0, 0, 1, true);
	TASK_CHECK(hit2.hit);
	TASK_CHECK(hit2.material == 7);
	TASK_CHECK(std::abs(hit2.distance - 3.25) < 1e-4); // Entry face of voxel z=4 minus origin.
	return 0;
}

const task_case task_cases[] = {
	{ "placement", "odd_translations", placement_odd_translations },
	{ "placement", "zero_source_material", placement_zero_source_material_edits_target },
	{ "placement", "non_commutative_order", placement_non_commutative_order },
	{ "placement", "empty_box_and_stats", placement_empty_box_and_stats_reset },
	{ "placement", "i32_edge_clipping", placement_i32_edge_clipping },
	{ "placement", "large_aligned_box", placement_large_aligned_box_stats },
	{ "aliasing", "self_overlap_odd_delta", aliasing_self_overlap_odd_delta },
	{ "aliasing", "zero_delta_matches_combine", aliasing_zero_delta_matches_aligned_combine },
	{ "aliasing", "self_shift_no_smear", aliasing_self_shift_no_smear },
	{ "sparse", "cross_store_source_destroyed", sparse_cross_store_source_destroyed },
	{ "sparse", "shared_subgraphs", sparse_shared_subgraphs_preserved_and_reused },
	{ "sparse", "ray_cross_check", sparse_ray_cross_check },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
