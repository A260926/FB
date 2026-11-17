// Regression tests for Volume::combineTranslated().
//
// These tests are linked with task_support/driver.c (which provides main()) and
// register themselves through the task_case table declared in task_support/check.h.
// Three groups are expected: placement, aliasing and sparse.

#include "storage.h"
#include "raytracing.h"
#include "../task_support/check.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace Cubiquity;

namespace
{
	// A simple dense reference grid used only by the tests. 'base' is the value
	// everywhere outside [lo, hi) as well, matching a Volume which has been
	// filled with 'base' before any voxels are set.
	struct RefGrid
	{
		i32 lo = 0;
		i32 hi = 0;
		MaterialId base = 0;
		std::vector<MaterialId> cells;

		RefGrid() = default;
		RefGrid(i32 lower, i32 upper, MaterialId fill = 0)
			: lo(lower), hi(upper), base(fill)
			, cells(static_cast<size_t>(upper - lower) * (upper - lower) * (upper - lower), fill)
		{
		}

		bool contains(i64 x, i64 y, i64 z) const
		{
			return x >= lo && x < hi && y >= lo && y < hi && z >= lo && z < hi;
		}

		size_t index(i64 x, i64 y, i64 z) const
		{
			return static_cast<size_t>(((x - lo) * (hi - lo) + (y - lo)) * (hi - lo) + (z - lo));
		}

		MaterialId get(i64 x, i64 y, i64 z) const
		{
			return contains(x, y, z) ? cells[index(x, y, z)] : base;
		}

		void set(i64 x, i64 y, i64 z, MaterialId value)
		{
			if (contains(x, y, z)) {
				cells[index(x, y, z)] = value;
			}
		}
	};

	// Combiner used by most tests. Deliberately not commutative and sensitive
	// to a zero source material.
	MaterialId blend(MaterialId target, MaterialId source)
	{
		return static_cast<MaterialId>((target * 5 + source * 11 + 3) & 0xff);
	}

	// Combiner which keeps a non-zero source material but otherwise leaves the target.
	MaterialId replaceIfSource(MaterialId target, MaterialId source)
	{
		return source ? source : target;
	}

	struct TranslationSpec
	{
		std::array<i64, 3> lo;
		std::array<i64, 3> hi;
		std::array<i64, 3> delta;
	};

	// Applies the translated combine to the reference grid, reading source values
	// from 'sourceBefore' (a separate snapshot, which models the aliasing rule).
	void apply_translation_reference(RefGrid& target, const RefGrid& sourceBefore,
		const TranslationSpec& spec, MaterialId (*combiner)(MaterialId, MaterialId))
	{
		for (i64 x = spec.lo[0]; x < spec.hi[0]; x++) {
			for (i64 y = spec.lo[1]; y < spec.hi[1]; y++) {
				for (i64 z = spec.lo[2]; z < spec.hi[2]; z++) {
					const i64 px = x + spec.delta[0];
					const i64 py = y + spec.delta[1];
					const i64 pz = z + spec.delta[2];
					if (px < INT32_MIN || px > INT32_MAX ||
						py < INT32_MIN || py > INT32_MAX ||
						pz < INT32_MIN || pz > INT32_MAX) {
						continue;
					}
					target.set(px, py, pz, combiner(target.get(px, py, pz),
						sourceBefore.get(x, y, z)));
				}
			}
		}
	}

	bool grids_match(const Volume& volume, const RefGrid& reference)
	{
		for (i64 x = reference.lo; x < reference.hi; x++) {
			for (i64 y = reference.lo; y < reference.hi; y++) {
				for (i64 z = reference.lo; z < reference.hi; z++) {
					const MaterialId expected = reference.get(x, y, z);
					const MaterialId actual = volume.voxel(static_cast<i32>(x),
						static_cast<i32>(y), static_cast<i32>(z));
					if (actual != expected) {
						std::fprintf(stderr, "mismatch at (%lld,%lld,%lld): got %u expected %u\n",
							static_cast<long long>(x), static_cast<long long>(y),
							static_cast<long long>(z), actual, expected);
						return false;
					}
				}
			}
		}
		return true;
	}

	// Fills a volume and a matching reference grid with the same deterministic pattern.
	void build_pattern(Volume& volume, RefGrid& reference, i32 range, MaterialId base,
		unsigned seed, MaterialId materialLimit)
	{
		reference = RefGrid(-range, range, base);
		volume.fill(base);

		std::mt19937 rng(seed);
		for (i64 x = -range; x < range; x++) {
			for (i64 y = -range; y < range; y++) {
				for (i64 z = -range; z < range; z++) {
					const MaterialId value = static_cast<MaterialId>(rng() % materialLimit);
					volume.setVoxel(static_cast<i32>(x), static_cast<i32>(y),
						static_cast<i32>(z), value);
					reference.set(x, y, z, value);
				}
			}
		}
	}

	void set_block(Volume& volume, RefGrid& reference, i64 lower, i64 upper, MaterialId value)
	{
		for (i64 x = lower; x < upper; x++) {
			for (i64 y = lower; y < upper; y++) {
				for (i64 z = lower; z < upper; z++) {
					volume.setVoxel(static_cast<i32>(x), static_cast<i32>(y),
						static_cast<i32>(z), value);
					reference.set(x, y, z, value);
				}
			}
		}
	}

	// Descends the tree from 'root' to the node covering the given voxel at
	// 'height'. Returns a material id if a material node is reached early.
	u32 node_at_height(const Internals::NodeStore& nodes, u32 root,
		i32 x, i32 y, i32 z, int height)
	{
		const u32 ux = static_cast<u32>(x) + UINT32_C(0x80000000);
		const u32 uy = static_cast<u32>(y) + UINT32_C(0x80000000);
		const u32 uz = static_cast<u32>(z) + UINT32_C(0x80000000);

		u32 nodeIndex = root;
		for (int level = 32; level > height && !Internals::isMaterialNode(nodeIndex); level--) {
			const int bit = level - 1;
			const u32 childId = ((ux >> bit) & 0x1) |
				(((uy >> bit) & 0x1) << 1) |
				(((uz >> bit) & 0x1) << 2);
			nodeIndex = nodes[nodeIndex][childId];
		}
		return nodeIndex;
	}

	bool ray_hits_x(const Volume& volume, float originX, float y, float z,
		MaterialId expectedMaterial, float expectedDistance)
	{
		const SubDAGArray subDAGs = findSubDAGs(Internals::getNodes(volume),
			Internals::getRootNodeIndex(volume));
		const RayVolumeIntersection hit = intersectVolume(volume, subDAGs,
			originX, y, z, 1.0f, 0.0f, 0.0f, true);

		if (!hit.hit) {
			std::fprintf(stderr, "ray missed\n");
			return false;
		}
		if (hit.material != expectedMaterial) {
			std::fprintf(stderr, "ray material %u expected %u\n", hit.material, expectedMaterial);
			return false;
		}
		if (std::fabs(hit.distance - expectedDistance) > 1e-4) {
			std::fprintf(stderr, "ray distance %g expected %g\n", hit.distance, expectedDistance);
			return false;
		}
		return true;
	}

	////////////////////////////////////////////////////////////////////////////
	// placement
	////////////////////////////////////////////////////////////////////////////

	int placement_odd_positive_delta()
	{
		Volume source;
		RefGrid sourceRef;
		build_pattern(source, sourceRef, 4, 0, 17, 5);

		Volume target;
		RefGrid targetRef;
		build_pattern(target, targetRef, 6, 1, 29, 3);

		const TranslationSpec spec{ {-5, -3, -7}, {6, 2, 4}, {3, 5, -7} };

		const RefGrid sourceBefore = sourceRef;
		RefGrid expected = targetRef;
		apply_translation_reference(expected, sourceBefore, spec, blend);

		CombineStats stats;
		target.combineTranslated(source, spec.lo, spec.hi, spec.delta, blend, &stats);

		TASK_CHECK(grids_match(target, expected));
		TASK_CHECK(stats.region_visits > 0);
		TASK_CHECK(stats.allocated_nodes > 0);
		// The source must be untouched.
		TASK_CHECK(grids_match(source, sourceBefore));
		return 0;
	}

	int placement_odd_negative_delta()
	{
		Volume source;
		RefGrid sourceRef;
		build_pattern(source, sourceRef, 5, 0, 41, 6);

		Volume target;
		RefGrid targetRef;
		build_pattern(target, targetRef, 7, 2, 53, 4);

		const TranslationSpec spec{ {-6, -6, -6}, {5, 5, 5}, {-5, -3, -7} };

		const RefGrid sourceBefore = sourceRef;
		RefGrid expected = targetRef;
		apply_translation_reference(expected, sourceBefore, spec, blend);

		CombineStats stats;
		target.combineTranslated(source, spec.lo, spec.hi, spec.delta, blend, &stats);

		TASK_CHECK(grids_match(target, expected));
		TASK_CHECK(stats.region_visits > 0);
		return 0;
	}

	// With a zero delta and a box covering the full signed coordinate range,
	// combineTranslated must behave exactly like the original combine().
	int placement_zero_delta_matches_combine()
	{
		Volume sourceA;
		RefGrid sourceARef;
		build_pattern(sourceA, sourceARef, 4, 0, 61, 5);

		Volume targetOld;
		RefGrid targetOldRef;
		build_pattern(targetOld, targetOldRef, 5, 3, 67, 3);
		targetOld.combine(sourceA, blend);

		Volume targetNew;
		RefGrid targetNewRef;
		build_pattern(targetNew, targetNewRef, 5, 3, 67, 3);

		const std::array<i64, 3> lo = { INT32_MIN, INT32_MIN, INT32_MIN };
		const std::array<i64, 3> hi = { INT64_C(1) << 31, INT64_C(1) << 31, INT64_C(1) << 31 };
		const std::array<i64, 3> delta = { 0, 0, 0 };
		targetNew.combineTranslated(sourceA, lo, hi, delta, blend);

		RefGrid expected = targetOldRef;
		// The reference grid only covers [-6, 6); the source is zero everywhere
		// outside [-4, 4), so restricting the reference scan to the grid's bounds
		// (which are all still inside the requested box) gives the same answer.
		const TranslationSpec checked{ {
			std::max<i64>(lo[0], -6), std::max<i64>(lo[1], -6), std::max<i64>(lo[2], -6) }, {
			std::min<i64>(hi[0], 6), std::min<i64>(hi[1], 6), std::min<i64>(hi[2], 6) }, delta };
		apply_translation_reference(expected, sourceARef, checked, blend);

		TASK_CHECK(grids_match(targetNew, expected));
		TASK_CHECK(grids_match(targetOld, expected));
		return 0;
	}

	int placement_zero_source_changes_target()
	{
		Volume source;
		source.fill(0); // Everything is material zero.
		Volume target;
		target.fill(4);

		CombineStats stats;
		target.combineTranslated(source, { 0, 0, 0 }, { 2, 2, 2 }, { 1, 1, 1 },
			blend, &stats);

		// blend(target=4, source=0) = (20 + 0 + 3) & 0xff = 23, so the zero source
		// material really does change the target.
		TASK_CHECK(target.voxel(1, 1, 1) == 23);
		TASK_CHECK(target.voxel(2, 2, 2) == 23);
		TASK_CHECK(target.voxel(3, 3, 3) == 4); // Outside the translated box.
		TASK_CHECK(target.voxel(0, 0, 0) == 4);
		TASK_CHECK(stats.region_visits > 0);
		return 0;
	}

	int placement_clip_positive_i32_end()
	{
		Volume source;
		source.fill(6);
		Volume target;
		target.fill(1);

		// Targets would run past INT32_MAX (and must not wrap around).
		target.combineTranslated(source, { 2147483640LL, 0, 0 }, { 2147483648LL, 1, 1 },
			{ 4, 0, 0 }, replaceIfSource);

		TASK_CHECK(target.voxel(2147483644, 0, 0) == 6);
		TASK_CHECK(target.voxel(2147483647, 0, 0) == 6);
		TASK_CHECK(target.voxel(2147483643, 0, 0) == 1);
		TASK_CHECK(target.voxel(-2147483648, 0, 0) == 1); // No wrap to the negative end.
		TASK_CHECK(target.voxel(2147483647, 1, 0) == 1);  // Outside the box in y.
		return 0;
	}

	int placement_clip_negative_i32_end()
	{
		Volume source;
		source.fill(6);
		Volume target;
		target.fill(1);

		// Targets would run below INT32_MIN.
		target.combineTranslated(source, { -2147483648LL, 0, 0 }, { -2147483640LL, 1, 1 },
			{ -4, 0, 0 }, replaceIfSource);

		TASK_CHECK(target.voxel(-2147483648, 0, 0) == 6);
		TASK_CHECK(target.voxel(-2147483645, 0, 0) == 6);
		TASK_CHECK(target.voxel(-2147483644, 0, 0) == 1);
		TASK_CHECK(target.voxel(2147483647, 0, 0) == 1); // No wrap.
		return 0;
	}

	int placement_empty_box_is_noop()
	{
		Volume source;
		source.fill(7);
		Volume target;
		target.fill(2);
		target.setVoxel(0, 0, 0, 9);

		CombineStats stats;
		stats.region_visits = 123;
		stats.allocated_nodes = 456;

		target.combineTranslated(source, { 0, 0, 0 }, { 0, 5, 5 }, { 3, 3, 3 },
			blend, &stats);

		TASK_CHECK(stats.region_visits == 0);
		TASK_CHECK(stats.allocated_nodes == 0);
		TASK_CHECK(target.voxel(0, 0, 0) == 9);
		TASK_CHECK(target.voxel(3, 0, 0) == 2);
		TASK_CHECK(target.voxel(3, 3, 3) == 2);
		return 0;
	}

	int placement_ray_after_paste()
	{
		Volume source;
		source.fill(0);
		for (i32 x = 2; x < 5; x++) {
			for (i32 y = 2; y < 5; y++) {
				for (i32 z = 2; z < 5; z++) {
					source.setVoxel(x, y, z, 6);
				}
			}
		}

		Volume target;
		target.setVoxel(50, 50, 50, 3); // Unrelated geometry.

		target.combineTranslated(source, { 2, 2, 2 }, { 5, 5, 5 }, { 4, -2, 6 },
			replaceIfSource);

		TASK_CHECK(target.voxel(6, 0, 8) == 6);
		TASK_CHECK(target.voxel(8, 2, 10) == 6);
		TASK_CHECK(target.voxel(5, 0, 8) == 0);
		TASK_CHECK(target.voxel(9, 0, 8) == 0);
		TASK_CHECK(target.voxel(6, 3, 8) == 0);
		TASK_CHECK(target.voxel(6, 0, 11) == 0);

		// Real ray entry point, using the convention that voxel centres lie on
		// integer coordinates (voxel boundaries are at half integers).
		TASK_CHECK(ray_hits_x(target, 4.25f, 1.25f, 8.25f, 6, 1.25f));
		return 0;
	}

	////////////////////////////////////////////////////////////////////////////
	// aliasing
	////////////////////////////////////////////////////////////////////////////

	int aliasing_self_overlap()
	{
		Volume volume;
		RefGrid reference;
		build_pattern(volume, reference, 5, 0, 71, 4);

		// A pattern which is not symmetric, so a left-to-right drag would show up.
		for (i32 x = -2; x <= 2; x++) {
			volume.setVoxel(x, 0, 0, static_cast<MaterialId>(10 + x + 2));
			reference.set(x, 0, 0, static_cast<MaterialId>(10 + x + 2));
		}

		const TranslationSpec spec{ {-4, -4, -4}, {4, 4, 4}, {2, -1, 3} };

		const RefGrid before = reference;
		RefGrid expected = reference;
		apply_translation_reference(expected, before, spec, blend);

		CombineStats stats;
		volume.combineTranslated(volume, spec.lo, spec.hi, spec.delta, blend, &stats);

		TASK_CHECK(grids_match(volume, expected));
		TASK_CHECK(stats.region_visits > 0);
		return 0;
	}

	int aliasing_self_overlap_odd_negative()
	{
		Volume volume;
		RefGrid reference;
		build_pattern(volume, reference, 4, 0, 83, 5);

		const TranslationSpec spec{ {-3, -3, -3}, {4, 4, 4}, {-3, 1, -2} };

		const RefGrid before = reference;
		RefGrid expected = reference;
		apply_translation_reference(expected, before, spec, blend);

		volume.combineTranslated(volume, spec.lo, spec.hi, spec.delta, blend);

		TASK_CHECK(grids_match(volume, expected));
		return 0;
	}

	int aliasing_cross_store_and_bake()
	{
		Volume source;
		RefGrid sourceRef;
		build_pattern(source, sourceRef, 4, 0, 97, 6);

		Volume target;
		RefGrid targetRef;
		build_pattern(target, targetRef, 6, 2, 101, 3);
		target.bake(); // All target nodes are shared before the paste.

		const RefGrid sourceBefore = sourceRef;
		const TranslationSpec spec{ {-3, -2, -4}, {4, 3, 5}, {7, -3, 2} };

		RefGrid expected = targetRef;
		apply_translation_reference(expected, sourceBefore, spec, blend);

		target.combineTranslated(source, spec.lo, spec.hi, spec.delta, blend);

		TASK_CHECK(grids_match(target, expected));
		TASK_CHECK(grids_match(source, sourceBefore));

		// Editing the target afterwards must not disturb the source's nodes.
		target.setVoxel(4, 0, 4, 200);
		TASK_CHECK(grids_match(source, sourceBefore));
		return 0;
	}

	int aliasing_shared_subgraph_isolation()
	{
		Volume source;
		source.fill(0);
		RefGrid sourceRef(-4, 16, 0);
		for (i32 x = 0; x < 4; x++) {
			for (i32 y = 0; y < 4; y++) {
				for (i32 z = 0; z < 4; z++) {
					const MaterialId value = static_cast<MaterialId>(6 + ((x + y + z) & 1));
					source.setVoxel(x, y, z, value);
					sourceRef.set(x, y, z, value);
					source.setVoxel(x + 8, y, z, value);
					sourceRef.set(x + 8, y, z, value);
				}
			}
		}

		Volume target;
		target.fill(0);
		target.combineTranslated(source, { 0, 0, 0 }, { 12, 4, 4 }, { 100, 0, 0 },
			replaceIfSource);

		const Internals::NodeStore& nodes = Internals::getNodes(target);
		const u32 root = Internals::getRootNodeIndex(target);
		const u32 firstBlock = node_at_height(nodes, root, 100, 0, 0, 2);
		const u32 secondBlock = node_at_height(nodes, root, 108, 0, 0, 2);

		// The two identical source blocks must have been imported as one shared subgraph.
		TASK_CHECK(firstBlock >= Internals::MaterialCount);
		TASK_CHECK(firstBlock == secondBlock);

		// And a later edit of one copy must not corrupt the other. The source
		// block at (x, y, z) held 6 + ((x + y + z) & 1); the second copy starts
		// at x = 8, so target x = 101 corresponds to source x = 1.
		target.setVoxel(100, 0, 0, 42);
		TASK_CHECK(target.voxel(100, 0, 0) == 42);
		TASK_CHECK(target.voxel(108, 0, 0) == 6);
		TASK_CHECK(target.voxel(101, 0, 1) == 6);
		TASK_CHECK(target.voxel(109, 0, 1) == 6);
		TASK_CHECK(target.voxel(102, 0, 1) == 7);
		TASK_CHECK(target.voxel(110, 0, 1) == 7);
		TASK_CHECK(firstBlock != node_at_height(Internals::getNodes(target),
			Internals::getRootNodeIndex(target), 100, 0, 0, 2));
		return 0;
	}

	int aliasing_source_destroyed_then_edit()
	{
		auto source = std::make_unique<Volume>();
		source->fill(0);
		for (i32 x = 0; x < 6; x++) {
			for (i32 y = 0; y < 6; y++) {
				for (i32 z = 0; z < 6; z++) {
					source->setVoxel(x, y, z, static_cast<MaterialId>(1 + (x + y + z) % 5));
				}
			}
		}

		Volume target;
		target.fill(0);
		target.combineTranslated(*source, { 0, 0, 0 }, { 6, 6, 6 }, { -2, 3, 1 },
			blend);

		source.reset(); // External source may be destroyed immediately.

		TASK_CHECK(target.voxel(-2, 3, 1) == blend(0, 1));
		TASK_CHECK(target.voxel(3, 8, 6) == blend(0, 1 + (0) % 5)); // (0,0,0) -> (0,0,0)

		// Editing after the call still sees the pasted values.
		target.setVoxel(-2, 3, 1, 77);
		TASK_CHECK(target.voxel(-2, 3, 1) == 77);

		// The old combine still works.
		Volume other;
		other.fill(2);
		target.combine(other, replaceIfSource);
		TASK_CHECK(target.voxel(0, 0, 0) == 2);

		// Re-preparing the SubDAGs gives a working ray traversal of the pasted data.
		Volume rayTarget;
		rayTarget.setVoxel(0, 0, 0, 5);
		rayTarget.combineTranslated(rayTarget, { -1, -1, -1 }, { 1, 1, 1 }, { 9, 0, 0 },
			replaceIfSource);
		TASK_CHECK(ray_hits_x(rayTarget, 6.75f, 0.25f, 0.25f, 5, 1.75f));
		return 0;
	}

	////////////////////////////////////////////////////////////////////////////
	// sparse
	////////////////////////////////////////////////////////////////////////////

	int sparse_large_aligned_box()
	{
		Volume source;
		source.fill(6);
		Volume target;
		target.fill(1);

		CombineStats stats;
		target.combineTranslated(source,
			{ -1048576, -1048576, -1048576 }, { 0, 0, 0 }, { 2097152, 0, 0 },
			[](MaterialId a, MaterialId b) { return static_cast<MaterialId>(a + b); },
			&stats);

		TASK_CHECK(stats.region_visits <= 8192);
		TASK_CHECK(stats.allocated_nodes <= 8192);

		// Inside the translated box every voxel is 7.
		TASK_CHECK(target.voxel(1048576, -1048576, -1048576) == 7);
		TASK_CHECK(target.voxel(1048576, -1, -1) == 7);
		TASK_CHECK(target.voxel(2097151, -1, -1) == 7);
		TASK_CHECK(target.voxel(1500000, -500000, -3) == 7);

		// Outside it every voxel is still 1.
		TASK_CHECK(target.voxel(1048575, -1, -1) == 1);
		TASK_CHECK(target.voxel(2097152, -1, -1) == 1);
		TASK_CHECK(target.voxel(1500000, 0, -3) == 1);
		TASK_CHECK(target.voxel(1500000, -500000, 0) == 1);
		TASK_CHECK(target.voxel(0, 0, 0) == 1);

		// The stats counters are reset by every call.
		target.combineTranslated(source, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
			[](MaterialId a, MaterialId b) { return static_cast<MaterialId>(a + b); },
			&stats);
		TASK_CHECK(stats.region_visits == 0);
		TASK_CHECK(stats.allocated_nodes == 0);
		return 0;
	}

	int sparse_uniform_early_out_nonuniform_target()
	{
		Volume source;
		source.fill(6);

		Volume target;
		target.fill(1);
		target.setVoxel(123, 456, 789, 3);

		CombineStats stats;
		target.combineTranslated(source,
			{ 0, 0, 0 }, { 1048576, 1048576, 1048576 }, { 0, 0, 0 },
			[](MaterialId a, MaterialId b) { return static_cast<MaterialId>(a + b); },
			&stats);

		TASK_CHECK(stats.region_visits <= 8192);
		TASK_CHECK(stats.allocated_nodes <= 8192);

		TASK_CHECK(target.voxel(0, 0, 0) == 7);
		TASK_CHECK(target.voxel(123, 456, 789) == 9);
		TASK_CHECK(target.voxel(1048575, 1048575, 1048575) == 7);
		TASK_CHECK(target.voxel(1048576, 0, 0) == 1);
		TASK_CHECK(target.voxel(-1, 0, 0) == 1);
		return 0;
	}
}

const struct task_case task_cases[] = {
	// placement
	{ "placement", "odd_positive_delta", &placement_odd_positive_delta },
	{ "placement", "odd_negative_delta", &placement_odd_negative_delta },
	{ "placement", "zero_delta_matches_combine", &placement_zero_delta_matches_combine },
	{ "placement", "zero_source_changes_target", &placement_zero_source_changes_target },
	{ "placement", "clip_positive_i32_end", &placement_clip_positive_i32_end },
	{ "placement", "clip_negative_i32_end", &placement_clip_negative_i32_end },
	{ "placement", "empty_box_is_noop", &placement_empty_box_is_noop },
	{ "placement", "ray_after_paste", &placement_ray_after_paste },

	// aliasing
	{ "aliasing", "self_overlap", &aliasing_self_overlap },
	{ "aliasing", "self_overlap_odd_negative", &aliasing_self_overlap_odd_negative },
	{ "aliasing", "cross_store_and_bake", &aliasing_cross_store_and_bake },
	{ "aliasing", "shared_subgraph_isolation", &aliasing_shared_subgraph_isolation },
	{ "aliasing", "source_destroyed_then_edit", &aliasing_source_destroyed_then_edit },

	// sparse
	{ "sparse", "large_aligned_box", &sparse_large_aligned_box },
	{ "sparse", "uniform_early_out_nonuniform_target", &sparse_uniform_early_out_nonuniform_target },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
