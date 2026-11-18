/*
Copyright (c) 2013 Benedikt Bitterli

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

/* Box painting for the sparse voxel octree.
 *
 * The descriptor encoding written by VoxelOctree::buildOctree is preserved:
 * bits 8..15 hold the child occupancy mask, bits 0..7 mark which occupied
 * children are internal descriptors (the rest are leaves), bits 18..31 hold
 * the child group offset, bit 16 widens every child slot of the group to two
 * words and bit 17 stores the upper offset bits in the following word. The
 * editor decodes the reachable descriptors into a temporary node graph,
 * applies the brush exactly in scene space and re-emits a canonical tree.
 * Eight solid children with a common non-zero material collapse into one
 * coarse leaf and empty branches disappear. No edit overlay survives the
 * call, and the brush grid only locates the edit: existing cells keep their
 * scene-space positions no matter which grid depth recorded them. */

#include "VoxelOctree.hpp"

#include "IntTypes.hpp"

#include <array>
#include <memory>
#include <vector>

namespace {

const int32 MaxDecodeLevel = 64;
const uint64 ShortOffsetLimit = 0x3FFF;
const int32 MaxLayoutPasses = 64;

struct Node {
    bool leaf;
    uint32 material;
    std::unique_ptr<Node> child[8];

    /* Scratch state for the canonical layout pass. */
    bool wide;
    bool extended;
    uint64 descriptor;
    uint64 groupStart;
    uint32 childCount;

    Node() : leaf(false), material(0), wide(false), extended(false),
             descriptor(0), groupStart(0), childCount(0) {}

    bool empty() const {
        if (leaf)
            return material == 0;
        for (int i = 0; i < 8; i++)
            if (child[i]) return false;
        return true;
    }

    bool hasInternalChildren() const {
        for (int i = 0; i < 8; i++)
            if (child[i] && !child[i]->leaf) return true;
        return false;
    }

    void clearChildren() {
        for (int i = 0; i < 8; i++)
            child[i].reset();
    }

    void countChildren() {
        childCount = 0;
        for (int i = 0; i < 8; i++)
            if (child[i]) childCount++;
    }
};

std::unique_ptr<Node> makeLeaf(uint32 material) {
    std::unique_ptr<Node> node(new Node());
    node->leaf = true;
    node->material = material;
    return node;
}

/* Decode every descriptor reachable from index. Empty branches are omitted. */
bool decodeNode(const uint32 *words, uint64 count, uint64 index, int level, Node &node) {
    if (index >= count || level > MaxDecodeLevel)
        return false;

    uint32 word = words[index];
    uint32 childMask = (word >> 8) & 0xFF;
    uint32 internalMask = word & 0xFF;

    uint64 offset = word >> 18;
    if (word & (1u << 17)) {
        if (index + 1 >= count)
            return false;
        offset = (offset << 32) | uint64(words[index + 1]);
    }

    node.leaf = false;
    node.material = 0;

    if (childMask == 0)
        return true;

    uint64 groupStart = index + offset;
    uint64 stride = (word & (1u << 16)) ? 2 : 1;

    int rank = 0;
    for (int b = 0; b < 8; b++) {
        if (!(childMask & (1u << b)))
            continue;

        uint64 slot = groupStart + uint64(rank)*stride;
        rank++;
        if (slot >= count)
            continue;

        if (internalMask & (1u << b)) {
            std::unique_ptr<Node> child(new Node());
            if (decodeNode(words, count, slot, level + 1, *child) && !child->empty())
                node.child[b] = std::move(child);
        } else {
            uint32 material = words[slot];
            if (material)
                node.child[b] = makeLeaf(material);
        }
    }

    return true;
}

/* Assign descriptor and group positions. Each descriptor occupies one word
 * inside its parent group slot (two words when that group is wide, leaving
 * room for the extended offset word), and its own group is placed at the
 * current end of the array, followed by its children's subtrees - the same
 * ordering the original builder produces. */
void layoutNode(Node &node, uint64 descriptor, uint64 &end, bool forceWideGroups) {
    node.descriptor = descriptor;
    node.countChildren();
    if (forceWideGroups)
        node.wide = node.hasInternalChildren();

    uint64 stride = node.wide ? 2 : 1;
    node.groupStart = end;
    end += uint64(node.childCount)*stride;

    uint64 offset = node.groupStart - descriptor;
    node.extended = descriptor != 0 && offset > ShortOffsetLimit;

    int rank = 0;
    for (int b = 0; b < 8; b++) {
        if (!node.child[b])
            continue;
        if (!node.child[b]->leaf)
            layoutNode(*node.child[b], node.groupStart + uint64(rank)*stride,
                       end, forceWideGroups);
        rank++;
    }
}

/* A group must be wide whenever one of its internal children needs an
 * extended offset word, because that word lives next to the child descriptor
 * inside the group slot. Widening groups grows offsets, so iterate until the
 * flags settle. */
bool widenGroups(Node &node) {
    bool changed = false;
    for (int b = 0; b < 8; b++) {
        if (!node.child[b])
            continue;
        if (!node.child[b]->leaf) {
            if (widenGroups(*node.child[b]))
                changed = true;
            if (node.child[b]->extended && !node.wide) {
                node.wide = true;
                changed = true;
            }
        }
    }
    return changed;
}

void emitNode(Node &node, std::vector<uint32> &words);

std::vector<uint32> encodeTree(Node &root) {
    bool changed = true;
    int pass = 0;
    while (changed && pass < MaxLayoutPasses) {
        changed = false;
        uint64 end = 1;
        layoutNode(root, 0, end, false);
        changed = widenGroups(root);
        pass++;
    }

    uint64 end = 1;
    if (pass >= MaxLayoutPasses) {
        /* Widening every group that holds internal children is always a
         * consistent layout, so it can serve as a safe fallback. */
        layoutNode(root, 0, end, true);
    } else {
        layoutNode(root, 0, end, false);
    }

    std::vector<uint32> words(size_t(end), 0);
    root.countChildren();
    emitNode(root, words);
    return words;
}

/* Write descriptors, leaf materials and extended offset words. Node positions
 * must already have been established by layoutNode. */
void emitNode(Node &node, std::vector<uint32> &words) {
    uint64 stride = node.wide ? 2 : 1;

    uint32 childMask = 0;
    uint32 internalMask = 0;
    int rank = 0;
    for (int b = 0; b < 8; b++) {
        if (!node.child[b])
            continue;
        uint64 slot = node.groupStart + uint64(rank)*stride;
        rank++;

        childMask |= 1u << b;
        if (node.child[b]->leaf)
            words[size_t(slot)] = node.child[b]->material;
        else
            internalMask |= 1u << b;
    }

    for (int b = 0; b < 8; b++)
        if (node.child[b] && !node.child[b]->leaf)
            emitNode(*node.child[b], words);

    uint64 offset = node.groupStart - node.descriptor;
    uint32 descriptorWord = (childMask << 8) | internalMask;

    if (node.wide && node.childCount)
        descriptorWord |= 1u << 16;

    if (node.extended) {
        /* Extended offsets keep the upper 14 bits in the descriptor and the
         * lower 32 bits in the extra word, matching the builder's encoding. */
        descriptorWord |= 1u << 17;
        descriptorWord |= uint32((offset >> 32) & ShortOffsetLimit) << 18;
        words[size_t(node.descriptor) + 1] = uint32(offset);
    } else {
        descriptorWord |= uint32(offset & ShortOffsetLimit) << 18;
    }

    words[size_t(node.descriptor)] = descriptorWord;
}

struct Box {
    int depth;
    uint64 lo[3];
    uint64 hi[3];
    uint32 material;
};

enum Cover {
    Disjoint,
    Inside,
    Partial
};

/* Classify a node cube against the brush box. Node cubes are expressed on
 * the brush grid: a node at depth d covers [index, index+1) * 2^(box.depth-d)
 * when it is coarser than the brush, and is a single brush cell (or finer,
 * when the tree is deeper than the brush) otherwise. */
Cover classify(const Box &box, int depth, const uint64 index[3]) {
    if (depth >= box.depth) {
        int shift = depth - box.depth;
        for (int i = 0; i < 3; i++) {
            uint64 unit = index[i] >> shift;
            if (unit < box.lo[i] || unit >= box.hi[i])
                return Disjoint;
        }
        return Inside;
    }

    int shift = box.depth - depth;
    bool inside = true;
    for (int i = 0; i < 3; i++) {
        uint64 begin = index[i] << shift;
        uint64 end = (index[i] + 1) << shift;
        if (end <= box.lo[i] || begin >= box.hi[i])
            return Disjoint;
        if (begin < box.lo[i] || end > box.hi[i])
            inside = false;
    }
    return inside ? Inside : Partial;
}

void expandLeaf(Node &node) {
    uint32 material = node.material;
    node.leaf = false;
    node.material = 0;
    for (int b = 0; b < 8; b++)
        node.child[b] = makeLeaf(material);
}

void assignNode(std::unique_ptr<Node> &node, uint32 material) {
    if (material == 0) {
        node.reset();
        return;
    }
    node = makeLeaf(material);
}

void mergeChildren(Node &node) {
    uint32 material = 0;
    for (int b = 0; b < 8; b++) {
        if (!node.child[b] || !node.child[b]->leaf)
            return;
        if (b == 0)
            material = node.child[b]->material;
        else if (node.child[b]->material != material)
            return;
    }
    if (material == 0)
        return;

    node.leaf = true;
    node.material = material;
    node.clearChildren();
}

void paintNode(std::unique_ptr<Node> &node, int depth, const uint64 index[3], const Box &box) {
    switch (classify(box, depth, index)) {
        case Disjoint:
            return;
        case Inside:
            assignNode(node, box.material);
            return;
        case Partial:
            break;
    }

    if (!node)
        node.reset(new Node());
    if (node->leaf)
        expandLeaf(*node);

    for (int b = 0; b < 8; b++) {
        uint64 childIndex[3] = {
            index[0]*2 + uint64(b & 1),
            index[1]*2 + uint64((b >> 1) & 1),
            index[2]*2 + uint64((b >> 2) & 1)
        };
        paintNode(node->child[b], depth + 1, childIndex, box);
    }

    mergeChildren(*node);
    if (node->empty())
        node.reset();
}

} /* namespace */

void VoxelOctree::paintBox(unsigned gridDepth, const std::array<uint32, 3> &lo,
                           const std::array<uint32, 3> &hi, uint32 material) {
    if (gridDepth < 1 || gridDepth > 20)
        return;

    Box box;
    box.depth = int(gridDepth);
    for (int i = 0; i < 3; i++) {
        box.lo[i] = lo[i];
        box.hi[i] = hi[i];
        if (box.lo[i] >= box.hi[i])
            return;
    }
    box.material = material;

    uint64 side = uint64(1) << gridDepth;
    for (int i = 0; i < 3; i++)
        if (box.hi[i] > side)
            return;

    Node root;
    if (_octree && _octreeSize)
        decodeNode(_octree.get(), _octreeSize, 0, 0, root);

    bool coversAll = true;
    for (int i = 0; i < 3; i++)
        if (box.lo[i] != 0 || box.hi[i] != side)
            coversAll = false;

    if (coversAll) {
        /* A full fill collapses to the root plus eight coarse leaves; an
         * erase leaves just the root descriptor. */
        root.leaf = false;
        root.material = 0;
        root.clearChildren();
        if (material)
            for (int b = 0; b < 8; b++)
                root.child[b] = makeLeaf(material);
    } else {
        if (root.leaf)
            expandLeaf(root);

        /* Occupancy bit b of a descriptor is octant b (x = bit 0,
         * y = bit 1, z = bit 2), and group slots follow ascending bit order. */
        for (int b = 0; b < 8; b++) {
            uint64 childIndex[3] = {
                uint64(b & 1),
                uint64((b >> 1) & 1),
                uint64((b >> 2) & 1)
            };
            paintNode(root.child[b], 1, childIndex, box);
        }
    }

    std::vector<uint32> words = encodeTree(root);

    _octreeSize = uint64(words.size());
    _octree.reset(new uint32[size_t(_octreeSize)]);
    for (size_t i = 0; i < words.size(); i++)
        _octree[i] = words[i];
}
