/***************************************************************************************************
* Cubiquity - A micro-voxel engine for games and other interactive applications                    *
*                                                                                                  *
* Written in 2019 by David Williams                                                                *
*                                                                                                  *
* To the extent possible under law, the author(s) have dedicated all copyright and related and     *
* neighboring rights to this software to the public domain worldwide. This software is distributed *
* without any warranty.                                                                            *
*                                                                                                  *
* You should have received a copy of the CC0 Public Domain Dedication along with this software.    *
* If not, see http://creativecommons.org/publicdomain/zero/1.0/.                                   *
***************************************************************************************************/
#include "storage.h"
#include "utility.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <unordered_set>

#ifdef CUBIQUITY_USE_POOLSTL
#define POOLSTL_STD_SUPPLEMENT
#define POOLSTL_STD_SUPPLEMENT_FORCE
#include "poolstl.hpp"
#else
#include <execution>
#endif // CUBIQUITY_USE_POOLSTL

namespace Cubiquity
{
	using namespace Internals;

	// It is desirable for the spatial relationship between child nodes to be consistent
	// throughout the tree, as this makes it easier to e.g compute normals from children
	// and to compute intersections during ray traversal. However, the use of signed integers
	// for positions results in a different spatial relationship for children of the root
	// node due to the the sign bit being set for negative numbers.
	//
	// Therefore we map the signed ints to unsigned ints. The offset is required because 
	// otherwise negative ints get mapped to large unsigned values, inverting the spatial
	// relationship between negative and positive positions.
	void make_position_unsigned(i32 ix, i32 iy, i32 iz, u32& ux, u32& uy, u32& uz)
	{
		ux = static_cast<u32>(ix) + UINT32_C(0x80000000);
		uy = static_cast<u32>(iy) + UINT32_C(0x80000000);
		uz = static_cast<u32>(iz) + UINT32_C(0x80000000);
	}

	// Potentially optimise this by bit-interleaving these three ints into a
	// larger  96-bit int (std::bitset or 2x u64) in advance of the loop calling
	// this function. Then we would just have one shift-by-three and a mask on
	// each iteration. Shorter/simpler loop body but some setup cost. SIMD on 
	// three variables could be an alternative optimisation.
	u32 extract_next_child_id(u32& ux, u32& uy, u32& uz)
	{
		u32 childX = ux & 0x80000000;
		ux = ux << 1;
		u32 childY = uy & 0x80000000;
		uy = uy << 1;
		u32 childZ = uz & 0x80000000;
		uz = uz << 1;

		return childZ >> 29 | childY >> 30 | childX >> 31;
	}

	bool Internals::isMaterialNode(const Node& node)
	{
		// Less than MaterialCount and all matching
		return isMaterialNode(node[0]) &&
			std::all_of(node.begin(), node.end(),
				[&](int x) { return x == node[0]; });
	}

	// Not allowed to be an std::hash specialisation as Node is just a typedef.
	struct Internals::NodeHasher
	{
		u64 operator()(const Internals::Node& node) const noexcept
		{
			// Reference version baed on FNV-1a
			// return Internals::fnv1a(node.data(), sizeof(node));

			// Simple XOR-multiply hash with constants picked from MurmurHash3.
			Cubiquity::u64 hash = 0;
			hash = (hash ^ node[0]) * 0x87c37b91114253d5ULL;
			hash = (hash ^ node[1]) * 0x4cf5ad432745937fULL;
			hash = (hash ^ node[2]) * 0xff51afd7ed558ccdULL;
			hash = (hash ^ node[3]) * 0xc4ceb9fe1a85ec53ULL;
			hash = (hash ^ node[4]) * 0x87c37b91114253d5ULL;
			hash = (hash ^ node[5]) * 0x4cf5ad432745937fULL;
			hash = (hash ^ node[6]) * 0xff51afd7ed558ccdULL;
			hash = (hash ^ node[7]) * 0xc4ceb9fe1a85ec53ULL;
			hash = bit_mix(hash);
			return hash;
		}
	};

	// We populate the node vector with material nodes in which the children
	// point at themselves. This has a few uses:
	// 
	//   1. It lets us recurse to a single voxel even if the tree has been
	//      pruned (used when setting a voxel).
	//   2. New material nodes can be created by copying these (rather than
	//      needing to call 'make_node(...)') which reduces conditional
	//      logic during traversal.
	//   3. During merging they can be inserted into the map in the same way as
	//      non-material nodes (as they have a valid location in the vector).
	NodeVector Internals::make_node_vector(u32 count)
	{
		// Init with invalid nodes
		Node invalid_node = make_node(InvalidNodeIndex);
		NodeVector nodeVector(count, invalid_node);

		// Set material nodes
		assert(count >= MaterialCount);
		for (u32 i = 0; i < MaterialCount; i++) {
			nodeVector[i] = make_node(i);
		}
		return nodeVector;
	}

	NodeStore::NodeStore()
		:mNodeVector(make_node_vector())
		,mSharedNodesEnd(mNodeVector.size())
	{
		/*std::mt19937 rng;
		rng.seed(42);
		std::uniform_int_distribution<u32> u32_dist;

		NodeVector test_vec;
		int count = 100000000;
		for (int i = 0; i < count; i++)
		{
			test_vec.push_back(Node{ u32_dist(rng), u32_dist(rng),  u32_dist(rng),  u32_dist(rng),  u32_dist(rng),  u32_dist(rng),  u32_dist(rng),  u32_dist(rng) });
		}

		NodeLookupSorted node_lookup(test_vec, count);
		exit(0);*/
	}

	u32 NodeStore::getNodeChild(u32 nodeIndex, u32 childId) const
	{
		if (nodeIndex < MaterialCount) { // Can bypass memory access
			return nodeIndex;
		} else {
			return mNodeVector[nodeIndex][childId];
		}
	}

	u32 NodeStore::setNodeChild(u32 nodeIndex, u32 childId, u32 value)
	{
		// Sanity checks
		//assert(!isMaterialNode(nodeIndex) && "Can't modify material nodes");
		assert(value != nodeIndex && "New child points at parent");
		assert(value != mNodeVector[nodeIndex][childId] && "Self-assignment");

		// Copy the existing node if it is shared
		if (isShared(nodeIndex)) {
			nodeIndex = append(mNodeVector[nodeIndex]);
		}

		// Update the child
		mNodeVector[nodeIndex][childId] = value;
		return nodeIndex;
	}

	u32 NodeStore::countNodes(u32 startNodeIndex) const
	{
		std::unordered_set<u32> usedIndices;
		countNodes(startNodeIndex, usedIndices);
		return usedIndices.size();
	}

	// Counts the number of nodes at distinct locations (node indices).
	// A tree which is not fully merged will contain idetical nodes at different
	// locations in memory, and these will be counted seperately.
	void NodeStore::countNodes(u32 startNodeIndex, std::unordered_set<u32>& usedIndices) const
	{
		// It may have been more efficient to have done this test before calling
		// into this function, but the implementation is simpler this way around.
		if (isMaterialNode(startNodeIndex)) { return; }

		usedIndices.insert(startNodeIndex);
		for (const u32& childNodeIndex : mNodeVector[startNodeIndex])
		{
			countNodes(childNodeIndex, usedIndices);
		}
	}

	void NodeStore::read(std::ifstream& file)
	{
		u32 nodeCount;
		file.read(reinterpret_cast<char*>(&nodeCount), sizeof(nodeCount));
		mNodeVector.resize(MaterialCount + nodeCount);
		file.read(reinterpret_cast<char*>(&(mNodeVector[sharedNodesBegin()])), sizeof(Node) * nodeCount);
		mSharedNodesEnd = mNodeVector.size(); // One past end, no unshared nodes.
	}

	void NodeStore::write(std::ofstream& file)
	{
		u32 nodeCount = sharedNodesEnd() - sharedNodesBegin();
		file.write(reinterpret_cast<const char*>(&nodeCount), sizeof(nodeCount));
		file.write(reinterpret_cast<const char*>(&mNodeVector[sharedNodesBegin()]), sizeof(Node) * nodeCount);
	}	

	u32 NodeStore::merge(u32 index)
	{
		// Destination used as hashmap and bigger than source to reduce collisions
		NodeVector merged_node_vector = make_node_vector((mNodeVector.size() * 12) / 10);

		// Perform the recursive merge
		u32 mergedRoot = merge_node(index, merged_node_vector);

		// Compact in-place, tracking how nodes move.
		std::vector<u32> moves(merged_node_vector.size());

		size_t write = 0;
		for (size_t read = 0; read < merged_node_vector.size(); ++read) {
			if (merged_node_vector[read][0] != InvalidNodeIndex) { // Occupied
				merged_node_vector[write] = std::move(merged_node_vector[read]);
				moves[read] = write;
				write++;
			}
		}
		merged_node_vector.resize(write);

		// Update node children based on how they moved during compacting.
		for (int i = 0; i < merged_node_vector.size(); i++)	{
			for (int childId = 0; childId < 8; childId++) {
				merged_node_vector[i][childId] = moves[merged_node_vector[i][childId]];
			}
		}
		mergedRoot = moves[mergedRoot];

		mNodeVector = std::move(merged_node_vector);
		mSharedNodesEnd = mNodeVector.size(); // Everything is now potentially shared

		return mergedRoot;
	}

	u32 NodeStore::merge_node(u32 nodeIndex, NodeVector& merged_node_vector)
	{
		assert(!isMaterialNode(nodeIndex));
		Node& node = mNodeVector[nodeIndex]; // By ref, modify in-place

		// In a DAG we might hit the same node more than once due to multiple 
		// parents. Handle this case to avoid reprocessing the node.
		const int Visited = 0, NewPos = 1; // Channel identifiers, not flags.
		if (node[Visited] == InvalidNodeIndex) return node[NewPos];

		// Merge the children if needed
		for (int i = 0; i < 8; i++) {
			if (!isMaterialNode(node[i])) {
				node[i] = merge_node(node[i], merged_node_vector);
			}
		}

		u32 newNodeIndex = 0;
		if (isMaterialNode(node)) {
			newNodeIndex = node[0]; // Any element works here, they are all the same
		}
		else {
			// Determine first index (skipping material node slots)
			newNodeIndex = static_cast<u32>(NodeHasher{}(node));
			newNodeIndex %= (merged_node_vector.size() - MaterialCount);
			newNodeIndex += MaterialCount;

			while (true) {
				if (merged_node_vector[newNodeIndex] == node) break; // Already present

				if (merged_node_vector[newNodeIndex][0] == InvalidNodeIndex) { // Empty space
					merged_node_vector[newNodeIndex] = node;
					break;
				}

				if (++newNodeIndex == merged_node_vector.size()) { // Move to next slot
					newNodeIndex = MaterialCount; // Wrap around, skip material nodes.
				}
			}
		}

		// Mark node as visited and note where it went. Making these changes
		// invalidates the node but we have already copied it to the destination.
		node[Visited] = InvalidNodeIndex;
		node[NewPos] = newNodeIndex;

		return newNodeIndex;
	}

	u32 NodeStore::append(const Node& node)
	{
		mNodeVector.push_back(node);
		return mNodeVector.size() - 1; // Index of new node
	}

	u32 NodeStore::cloneRoot(u32 rootIndex)
	{
		mSharedNodesEnd = mNodeVector.size(); // Mark everything as potentially shared
		mNodeVector.push_back(mNodeVector[rootIndex]); // Append unshared copy of root
		return mNodeVector.size() - 1; // Return position of new node
	}

	void NodeStore::print(std::ostream& os)
	{
		for (u32 i = 0; i < mNodeVector.size(); i++)
		{
			if (i == mSharedNodesEnd)
			{
				os << "\n-------------------- Shared nodes end / unshared nodes begin -------------------\n\n";
			}

			os << std::setw(4) << i << ": ";
			for (int c = 0; c < 8; c++)
			{
				os << std::setw(4) << mNodeVector[i][c] << " ";
			}
			os << "\n";
		}

		os << "\nShared Nodes end = " << mSharedNodesEnd << "\n";
	}

	////////////////////////////////////////////////////////////////////////////////
	// Public member functions
	////////////////////////////////////////////////////////////////////////////////

	Volume::Volume()
	{
		mRootNodeIndices.resize(1);
		mCurrentRoot = 0;
	}

	Volume::Volume(const std::string& filename)
	{
		mRootNodeIndices.resize(1);
		mCurrentRoot = 0;

		load(filename);
	}

	void Volume::fill(MaterialId matId)
	{
		setRootNodeIndex(matId);
	}

	u32 Volume::rootNodeIndex() const
	{
		return mRootNodeIndices[mCurrentRoot];
	}

	void Volume::setRootNodeIndex(u32 newRootNodeIndex)
	{
		mRootNodeIndices[mCurrentRoot] = newRootNodeIndex;
	}

	void Volume::checkpoint()
	{
		// If we 'undo' a few times and then create a checkpoint then the 'redo'
		// checkpoints are invalidated. Discard them by cropping the vector.
		mRootNodeIndices.resize(mCurrentRoot + 1); // Clear any 'redo' items

		// Clone the current root
		u32 root = mRootNodeIndices[mCurrentRoot];
		u32 newRoot = mNodeStore.cloneRoot(root);

		// Store it
		mRootNodeIndices.push_back(newRoot);
		mCurrentRoot++;
	}

	void Volume::undo()
	{
		if (mCurrentRoot > 0) {
			mCurrentRoot--;
		}
	}

	void Volume::redo()
	{
		if (mCurrentRoot < mRootNodeIndices.size() - 1)	{
			mCurrentRoot++;
		}
	}

	void Volume::bake()
	{
		Cubiquity::Timer timer;
		u32 newRootIndex = mNodeStore.merge(rootNodeIndex());
		setRootNodeIndex(newRootIndex);
		log_debug("Baked %u nodes in %.3f seconds",
			mNodeStore.unsharedNodesEnd(), timer.elapsed_seconds());
	}

	void Volume::setVoxel(i32 x, i32 y, i32 z, MaterialId matId)
	{
		// Map to unsigned positions
		u32 ux, uy, uz;
		make_position_unsigned(x, y, z, ux, uy, uz);

		const int rootHeight = log_base_2(VolumeSideLength);
		u32 newRootNodeIndex = setVoxel(ux, uy, uz, matId, rootNodeIndex(), rootHeight);
		setRootNodeIndex(newRootNodeIndex);
	}

	// I previously had iterative versions of ths function which do perform
	// better but are a little harder to reason about. One version gives the
	// same behaviour but is faster (possibly due to easier handling of the
	// early out), and another version writes nodes on the way down the tree
	// (it assumes the voxel isn't already set) rather on the way back up - this
	// is faster still. But I don't want to maintain differnt versions at the
	// moment, and the recursive version is easier. The others can be found in
	// the commit history on the NodeStore branch (present in Jan 2026).
	u32 Volume::setVoxel(u32 ux, u32 uy, u32 uz, MaterialId matId, u32 nodeIndex, int nodeHeight)
	{
		// Get the relevant child node
		u32 childId = extract_next_child_id(ux, uy, uz); // Modifies params
		u32 childNodeIndex = mNodeStore.getNodeChild(nodeIndex, childId);

		// Check if already set to desired material
		if (childNodeIndex == matId) { return nodeIndex; }

		// Recusively process the child node until we reach a just-above-leaf node (height of 1).
		if (nodeHeight > 1)
		{
			u32 newChildNodeIndex = setVoxel(ux, uy, uz, matId, childNodeIndex, nodeHeight - 1);

			// If the child hasn't changed then we don't need to update the current node.
			if (newChildNodeIndex == childNodeIndex) { return nodeIndex; }

			return mNodeStore.setNodeChild(nodeIndex, childId, newChildNodeIndex);
		}
		else
		{
			return mNodeStore.setNodeChild(nodeIndex, childId, matId);
		}
	}

	void Volume::combine(const Volume& rhsVolume, const MaterialCombiner& combiner)
	{
		u32 newRootNodeIndex = combine(rhsVolume, rhsVolume.rootNodeIndex(), rootNodeIndex(), combiner);
		setRootNodeIndex(newRootNodeIndex);
	}

	// Fixme - This function can prbably be more efficient. Firstly through better early out when whole nodes are full/empty, and also
	// if seperate volumes shared a common memory space it would be easier to copy node directly across (maybe in compressd space?).
	u32 Volume::combine(const Volume& rhsVolume, u32 rhsNodeIndex, u32 nodeIndex, const MaterialCombiner& combiner)
	{
		for (u32 childId = 0; childId < 8; childId++)
		{
			const bool nodeIsMaterial = isMaterialNode(nodeIndex);
			const bool rhsNodeIsMaterial = isMaterialNode(rhsNodeIndex);

			// If current node is a material then just propergate it. Otherwise get the true child.
			u32 childNodeIndex = mNodeStore.getNodeChild(nodeIndex, childId);
			u32 rhsChildNodeIndex = rhsVolume.mNodeStore.getNodeChild(rhsNodeIndex, childId);

			// Process children
			u32 newChildNodeIndex = childNodeIndex;

			if (isMaterialNode(childNodeIndex) && isMaterialNode(rhsChildNodeIndex))
			{
				newChildNodeIndex = combiner(childNodeIndex, rhsChildNodeIndex);
			}
			else
			{
				newChildNodeIndex = combine(rhsVolume, rhsChildNodeIndex, childNodeIndex, combiner);
			}

			// If the child has changed then we need to update the current node.
			if (childNodeIndex != newChildNodeIndex)
			{
				nodeIndex = mNodeStore.setNodeChild(nodeIndex, childId, newChildNodeIndex);
			}
		}

		return nodeIndex;
	}

	namespace
	{
		// The compressed tree covers signed 32-bit voxel coordinates. In the
		// tree's unsigned encoding the root cell is exactly this interval, and
		// the root node has height 32 (its children are height 31, etc).
		constexpr i64 TranslatedRootLower = -(INT64_C(1) << 31);
		constexpr int TranslatedRootHeight = 32;

		// A node's height is implicit in where it is referenced, so nodes can only
		// be reused for other subtrees of the same height. Two nodes with equal
		// contents but different heights would otherwise be confused.
		struct TranslatedNodeKey
		{
			int height;
			Node node;
			bool operator==(const TranslatedNodeKey& other) const
			{
				return height == other.height && node == other.node;
			}
		};

		struct TranslatedNodeKeyHasher
		{
			size_t operator()(const TranslatedNodeKey& key) const noexcept
			{
				return static_cast<size_t>(
					Internals::hash_combine(NodeHasher{}(key.node),
						static_cast<u64>(static_cast<u32>(key.height))));
			}
		};

		struct TranslatedCombineContext
		{
			NodeStore& targetStore;
			const NodeStore& sourceStore;
			u32 sourceRootNode;
			std::array<i64, 3> delta;
			const MaterialCombiner& combiner;
			CombineStats stats;
			// Nodes created during this call, keyed by height and content so that
			// repeated subgraphs in the source are imported only once.
			std::unordered_map<TranslatedNodeKey, u32, TranslatedNodeKeyHasher> createdNodes;
		};

		// Adds a node to the destination store, collapsing uniform nodes to the
		// corresponding material node and reusing identical nodes created earlier
		// during this same call.
		u32 translated_append_node(TranslatedCombineContext& context, const Node& node, int height)
		{
			if (isMaterialNode(node)) {
				return node[0];
			}

			const TranslatedNodeKey key{ height, node };
			const auto existing = context.createdNodes.find(key);
			if (existing != context.createdNodes.end()) {
				return existing->second;
			}

			const u32 nodeIndex = context.targetStore.appendNode(node);
			context.createdNodes.emplace(key, nodeIndex);
			context.stats.allocated_nodes++;
			return nodeIndex;
		}

		// Determines whether the source subtree rooted at 'nodeIndex' has a single
		// material throughout the (axis aligned, non-empty) box [boxLower, boxUpper).
		// The box must be contained in the cell [cellLower, cellLower + 2^cellHeight).
		// Returns true and sets 'material' if so. Uniform source nodes terminate the
		// recursion immediately, which is what keeps solid regions cheap.
		bool query_source_uniform(TranslatedCombineContext& context, u32 nodeIndex,
			const std::array<i64, 3>& cellLower, int cellHeight,
			const std::array<i64, 3>& boxLower, const std::array<i64, 3>& boxUpper,
			MaterialId& material)
		{
			if (isMaterialNode(nodeIndex)) {
				material = static_cast<MaterialId>(nodeIndex);
				return true;
			}

			// Valid stores always have material nodes at height zero.
			assert(cellHeight > 0 && "Internal node at leaf height");

			const Node node = context.sourceStore[nodeIndex];

			const i64 cellSize = INT64_C(1) << cellHeight;
			bool boxIsWholeCell = true;
			for (int axis = 0; axis < 3; axis++) {
				if (boxLower[axis] != cellLower[axis] ||
					boxUpper[axis] != cellLower[axis] + cellSize) {
					boxIsWholeCell = false;
				}
			}

			// If the box covers the whole cell then the node itself has to be
			// uniform. Compressed trees normally represent such a cell by a
			// material node, but edits such as setVoxel() can also leave an
			// uncompressed-but-uniform internal node, so check for that too.
			if (boxIsWholeCell) {
				if (isMaterialNode(nodeIndex)) {
					material = static_cast<MaterialId>(nodeIndex);
					return true;
				}
				// Not collapsed by the store, but an internal node whose eight
				// children are all the same material is still uniform.
				if (isMaterialNode(node)) {
					material = static_cast<MaterialId>(node[0]);
					return true;
				}
				return false;
			}

			const i64 childSize = cellSize >> 1;
			bool foundMaterial = false;
			for (u32 childId = 0; childId < 8; childId++) {
				std::array<i64, 3> childLower;
				bool overlaps = true;
				for (int axis = 0; axis < 3; axis++) {
					childLower[axis] = cellLower[axis] +
						(((childId >> axis) & 0x1) ? childSize : 0);
					if (!(childLower[axis] < boxUpper[axis] &&
						boxLower[axis] < childLower[axis] + childSize)) {
						overlaps = false;
					}
				}
				if (!overlaps) {
					continue;
				}

				MaterialId childMaterial = 0;
				if (!query_source_uniform(context, node[childId], childLower,
					cellHeight - 1, boxLower, boxUpper, childMaterial)) {
					return false;
				}

				if (!foundMaterial) {
					material = childMaterial;
					foundMaterial = true;
				}
				else if (material != childMaterial) {
					return false;
				}
			}

			return foundMaterial;
		}

		// Builds a replacement for the destination subtree 'nodeIndex' (which covers
		// 'cellLower' at 'cellHeight') with the translated source combined in over
		// [regionLower, regionUpper), which is the intersection of the requested
		// region with this cell. Voxels outside the region keep their old value.
		//
		// 'knownSourceMaterial', when non-null, states that the source is uniform
		// over this whole cell (with the given value), letting descendants skip
		// repeated source queries.
		u32 build_translated_region(TranslatedCombineContext& context, u32 nodeIndex,
			const std::array<i64, 3>& cellLower, int cellHeight,
			const std::array<i64, 3>& regionLower, const std::array<i64, 3>& regionUpper,
			const MaterialId* knownSourceMaterial = nullptr)
		{
			context.stats.region_visits++;

			const i64 cellSize = INT64_C(1) << cellHeight;

			std::array<i64, 3> sourceLower;
			std::array<i64, 3> sourceUpper;
			bool regionIsWholeCell = true;
			for (int axis = 0; axis < 3; axis++) {
				if (regionLower[axis] != cellLower[axis] ||
					regionUpper[axis] != cellLower[axis] + cellSize) {
					regionIsWholeCell = false;
				}
				// Computed in 64-bit because the source box of a translated region
				// can lie outside the signed 32-bit coordinate range.
				sourceLower[axis] = regionLower[axis] - context.delta[axis];
				sourceUpper[axis] = regionUpper[axis] - context.delta[axis];
			}

			if (regionIsWholeCell) {
				MaterialId sourceMaterial = 0;
				bool sourceIsUniform = false;
				if (knownSourceMaterial) {
					sourceMaterial = *knownSourceMaterial;
					sourceIsUniform = true;
				}
				else {
					const std::array<i64, 3> rootLower = {
						TranslatedRootLower, TranslatedRootLower, TranslatedRootLower };
					sourceIsUniform = query_source_uniform(context,
						context.sourceRootNode, rootLower, TranslatedRootHeight,
						sourceLower, sourceUpper, sourceMaterial);
				}

				if (sourceIsUniform) {
					if (isMaterialNode(nodeIndex)) {
						return context.combiner(
							static_cast<MaterialId>(nodeIndex), sourceMaterial);
					}

					// The destination is not uniform, but the source is. Each of
					// its children still combines with the same source material.
					const Node node = context.targetStore[nodeIndex];
					const i64 childSize = cellSize >> 1;
					Node result;
					bool changed = false;
					for (u32 childId = 0; childId < 8; childId++) {
						const std::array<i64, 3> childLower = {
							cellLower[0] + (((childId >> 0) & 0x1) ? childSize : 0),
							cellLower[1] + (((childId >> 1) & 0x1) ? childSize : 0),
							cellLower[2] + (((childId >> 2) & 0x1) ? childSize : 0) };
						const std::array<i64, 3> childUpper = {
							childLower[0] + childSize,
							childLower[1] + childSize,
							childLower[2] + childSize };

						const u32 childNodeIndex = node[childId];
						u32 newChildNodeIndex = childNodeIndex;
						if (isMaterialNode(childNodeIndex)) {
							newChildNodeIndex = context.combiner(
								static_cast<MaterialId>(childNodeIndex), sourceMaterial);
						}
						else {
							newChildNodeIndex = build_translated_region(context,
								childNodeIndex, childLower, cellHeight - 1,
								childLower, childUpper, &sourceMaterial);
						}

						result[childId] = newChildNodeIndex;
						changed = changed || (newChildNodeIndex != childNodeIndex);
					}

					return changed ? translated_append_node(context, result, cellHeight) : nodeIndex;
				}

				// A single destination voxel's source region is one source voxel,
				// so it is always uniform. Valid stores cannot get here.
				assert(cellHeight > 0 && "Source region over a single voxel must be uniform");
				if (cellHeight == 0) {
					return nodeIndex;
				}
			}

			const i64 childSize = cellSize >> 1;
			const Node node = isMaterialNode(nodeIndex)
				? make_node(nodeIndex)
				: context.targetStore[nodeIndex];

			Node result;
			bool changed = false;
			for (u32 childId = 0; childId < 8; childId++) {
				const std::array<i64, 3> childLower = {
					cellLower[0] + (((childId >> 0) & 0x1) ? childSize : 0),
					cellLower[1] + (((childId >> 1) & 0x1) ? childSize : 0),
					cellLower[2] + (((childId >> 2) & 0x1) ? childSize : 0) };
				const std::array<i64, 3> childUpper = {
					childLower[0] + childSize,
					childLower[1] + childSize,
					childLower[2] + childSize };

				std::array<i64, 3> clippedLower;
				std::array<i64, 3> clippedUpper;
				bool overlaps = true;
				for (int axis = 0; axis < 3; axis++) {
					clippedLower[axis] = std::max(regionLower[axis], childLower[axis]);
					clippedUpper[axis] = std::min(regionUpper[axis], childUpper[axis]);
					if (clippedLower[axis] >= clippedUpper[axis]) {
						overlaps = false;
					}
				}

				const u32 childNodeIndex = node[childId];
				u32 newChildNodeIndex = childNodeIndex;
				if (overlaps) {
					newChildNodeIndex = build_translated_region(context,
						childNodeIndex, childLower, cellHeight - 1,
						clippedLower, clippedUpper);
				}

				result[childId] = newChildNodeIndex;
				changed = changed || (newChildNodeIndex != childNodeIndex);
			}

			return changed ? translated_append_node(context, result, cellHeight) : nodeIndex;
		}
	}

	void Volume::combineTranslated(const Volume& sourceVolume,
		const std::array<i64, 3>& lower,
		const std::array<i64, 3>& upper,
		const std::array<i64, 3>& delta,
		const MaterialCombiner& combiner,
		CombineStats* stats)
	{
		CombineStats localStats;
		CombineStats& resultStats = stats ? *stats : localStats;
		resultStats = CombineStats(); // Every call counts from zero.

		// Clip the source box to the voxels whose translated position is still a
		// valid signed 32-bit target coordinate. Everything is computed in 64-bit
		// because 'delta' alone may span [-2^32, 2^32] and lo/hi may reach 2^31.
		constexpr i64 MinTargetCoord = INT32_MIN;
		constexpr i64 MaxTargetCoord = INT32_MAX;

		std::array<i64, 3> sourceLower;
		std::array<i64, 3> sourceUpper;
		std::array<i64, 3> targetLower;
		std::array<i64, 3> targetUpper;
		bool empty = false;
		for (int axis = 0; axis < 3; axis++) {
			// Smallest/largest source coordinate whose target position is valid.
			const i64 minSourceCoord = MinTargetCoord - delta[axis];
			const i64 maxSourceCoord = MaxTargetCoord - delta[axis]; // Inclusive

			sourceLower[axis] = std::max(lower[axis], minSourceCoord);
			sourceUpper[axis] = std::min(upper[axis], maxSourceCoord + 1);
			if (sourceLower[axis] >= sourceUpper[axis]) {
				empty = true;
			}

			// These are guaranteed to lie within the signed 32-bit target range.
			targetLower[axis] = sourceLower[axis] + delta[axis];
			targetUpper[axis] = sourceUpper[axis] + delta[axis];
		}

		if (empty) {
			return; // Empty box (or entirely clipped away): nothing changes.
		}

		TranslatedCombineContext context{
			mNodeStore,
			sourceVolume.mNodeStore,
			sourceVolume.rootNodeIndex(),
			delta,
			combiner,
			CombineStats(),
			{} };

		const std::array<i64, 3> rootLower = {
			TranslatedRootLower, TranslatedRootLower, TranslatedRootLower };

		const u32 oldRootNodeIndex = rootNodeIndex();
		const u32 newRootNodeIndex = build_translated_region(context,
			oldRootNodeIndex, rootLower, TranslatedRootHeight,
			targetLower, targetUpper);

		if (newRootNodeIndex != oldRootNodeIndex) {
			setRootNodeIndex(newRootNodeIndex);
		}

		// Nodes created by this call may be referenced from several places in the
		// new tree. Mark every node as shared so later in-place edits take the
		// copy-on-write path and cannot corrupt a shared subgraph.
		if (context.stats.allocated_nodes > 0) {
			mNodeStore.markAllNodesShared();
		}

		resultStats = context.stats;
	}

	MaterialId Volume::voxel(i32 x, i32 y, i32 z) const
	{
		// TODO: Consider starting from split point instead of root
		u32 nodeIndex = rootNodeIndex();

		// Map to unsigned positions
		u32 ux, uy, uz;
		make_position_unsigned(x, y, z, ux, uy, uz);

		// Descend tree. Memory access is apparent bottleneck. Improve node
		// ordering for better cache coherency or try memory prefetch?
		while (!isMaterialNode(nodeIndex))
		{
			u32 childId = extract_next_child_id(ux, uy, uz); // Modifies params
			nodeIndex = mNodeStore[nodeIndex][childId];
		}

		return static_cast<MaterialId>(nodeIndex);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Private member functions
	////////////////////////////////////////////////////////////////////////////////

	bool Volume::load(const std::string& filename)
	{
		std::ifstream file(filename, std::ios::binary);

		// FIXME - What should we do for error handling in this function? Return codes or exceptions?
		if(!file.is_open())
		{
			return false;
		}

		u32 rootNodeIndex;
		file.read(reinterpret_cast<char*>(&rootNodeIndex), sizeof(rootNodeIndex));

		mNodeStore.read(file);

		if (rootNodeIndex >= MaterialCount)
		{
			rootNodeIndex += mNodeStore.sharedNodesBegin() - MaterialCount;
		}

		setRootNodeIndex(rootNodeIndex);

		return true;
	}

	void Volume::save(const std::string& filename)
	{
		bake(); // Possibly should be here, not part of saving?

		u32 root = rootNodeIndex();
		const Node& data = mNodeStore[mNodeStore.sharedNodesBegin()];

		std::ofstream file;
		file = std::ofstream(filename, std::ios::out | std::ios::binary);
		file.write(reinterpret_cast<const char*>(&root), sizeof(root));
		mNodeStore.write(file);
		file.close();
	}

	void Volume::print(std::ostream& os)
	{
		mNodeStore.print(os);

		os << "Root nodes: [";
		for (const auto& rootNodeIndex : mRootNodeIndices)
		{
			os << rootNodeIndex << ", ";
		}
		os << "]\n";

		os << "Current root index = " << mCurrentRoot << "\n";
		os << "Current root value = " << mRootNodeIndices[mCurrentRoot] << "\n";
	}

	namespace Internals
	{
		/// This is an advanced function which should only be used if you
		/// understand the internal memory layout of Cubiquity's volume data.
		NodeStore& getNodes(Volume& volume)
		{
			return volume.mNodeStore;
		}

		const NodeStore& getNodes(const Volume& volume)
		{
			return volume.mNodeStore;
		}

		const u32 getRootNodeIndex(const Volume& volume)
		{
			return volume.rootNodeIndex();
		}
	}
}
