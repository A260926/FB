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

#include "VoxelOctree.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

/*
   Direct box editing of the sparse voxel octree.

   The on-disk descriptor format (see VoxelOctree.cpp) packs internal nodes as
   one descriptor word followed by a group of child records; leaf children store
   a raw 32 bit material, internal children store another descriptor (with an
   extra address extension word when the group uses extended addressing). It is
   compact but awkward to mutate in place, so every brush stroke parses the
   reachable descriptors into a plain pointer tree, applies the edit and
   re-serializes the whole reachable structure. No overlay is kept between calls
   and the old array is replaced, so a painted tree never retains stale content.

   Geometry convention: the root occupies the normalized scene cube [1, 2)^3.
   A node at level d spans 2^-d per axis, i.e. 2^(gridDepth - d) cells of a
   brush grid of side N = 2^gridDepth. Child indices use the same octant ordering
   as the original builder (see the posX/posY/posZ tables in VoxelOctree.cpp).
*/
namespace {

struct EditNode {
    bool isLeaf;
    uint32 material;          // valid when isLeaf
    EditNode *children[8];    // valid when !isLeaf, null == absent

    // Transient layout state used only while serializing.
    int64_t regionWords;      // child-group words plus every descendant region
    bool wideGroup;           // child group uses extended (double-wide) slots

    EditNode() : isLeaf(false), material(0), regionWords(0), wideGroup(false) {
        for (int i = 0; i < 8; ++i)
            children[i] = 0;
    }
};

struct Editor {
    std::vector<std::unique_ptr<EditNode>> pool;

    EditNode *makeNode() {
        pool.emplace_back(new EditNode());
        return pool.back().get();
    }

    EditNode *makeLeaf(uint32 material) {
        EditNode *n = makeNode();
        n->isLeaf = true;
        n->material = material;
        return n;
    }

    /*
       Parse a descriptor word array into editable nodes. Child groups are
       located through the stored relative offsets exactly the way the ray
       marcher addresses them, so every legal descriptor layout (short or
       extended addresses, uniform or mixed leaf/internal groups) is accepted.
       descIdx points at an internal descriptor; roots pass isRoot because their
       offset bits carry a marker instead of a usable offset.
    */
    EditNode *parse(const uint32 *words, uint64 descIdx, bool isRoot) {
        uint32 w = words[descIdx];
        uint32 childMask = (w >> 8) & 0xFF;
        // The low mask bit is SET for internal children and CLEAR for leaf children
        // (see buildOctree: leaf voxels leave it zero, subdivided children copy the
        // child mask). The ray marcher distinguishes the two through the same bit.
        uint32 internalMask = w & 0xFF;
        bool doubleWide = (w & 0x10000) != 0;

        uint64 groupBase;
        if (isRoot) {
            groupBase = descIdx + 1;
        } else {
            uint64 childOffset = w >> 18;
            if (w & 0x20000)
                childOffset = (childOffset << 32) | uint64(words[descIdx + 1]);
            groupBase = descIdx + childOffset;
        }

        EditNode *node = makeNode();
        uint64 wordPos = 0;
        // Children are stored in descending index order.
        for (int i = 7; i >= 0; --i) {
            uint32 bit = 128u >> i;
            if (!(childMask & bit))
                continue;

            uint64 childWord = groupBase + wordPos;
            if (!(internalMask & bit)) {
                uint32 mat = words[childWord];
                node->children[i] = mat ? makeLeaf(mat) : 0;
                wordPos += 1;
            } else {
                node->children[i] = parse(words, childWord, false);
                wordPos += doubleWide ? 2 : 1;
            }
        }
        return node;
    }

    /*
       Apply one half-open box [lo, hi) of a grid with N = 2^gridDepth cells.
       origin* are grid coordinates of the node's cube corner, span is the cube
       side length in grid cells.
    */
    EditNode *edit(EditNode *node, int level, int gridDepth,
                   uint32 ox, uint32 oy, uint32 oz, uint32 span,
                   const std::array<uint32, 3> &lo,
                   const std::array<uint32, 3> &hi, uint32 material) {
        uint32 ex = ox + span, ey = oy + span, ez = oz + span;

        // No overlap with the brush box.
        if (ex <= lo[0] || ox >= hi[0] ||
            ey <= lo[1] || oy >= hi[1] ||
            ez <= lo[2] || oz >= hi[2])
            return node;

        // Cube fully inside the brush box: overwrite the whole subtree.
        bool fullX = ox >= lo[0] && ex <= hi[0];
        bool fullY = oy >= lo[1] && ey <= hi[1];
        bool fullZ = oz >= lo[2] && ez <= hi[2];
        if (fullX && fullY && fullZ) {
            if (material == 0)
                return 0;
            if (node && node->isLeaf && node->material == material)
                return node;
            return makeLeaf(material);
        }

        // Partial overlap only happens above the finest brush level.
        if (level == gridDepth)
            return node;

        if (node == 0) {
            // Empty space only needs structure when filling it in.
            if (material == 0)
                return 0;
            node = makeNode();
        } else if (node->isLeaf) {
            // Split a coarse solid leaf; the untouched children keep its material.
            uint32 kept = node->material;
            node = makeNode();
            for (int i = 0; i < 8; ++i)
                node->children[i] = makeLeaf(kept);
        }

        uint32 half = span >> 1;
        for (int i = 0; i < 8; ++i) {
            // Builder octant order: high half when the matching bit is clear.
            uint32 cx = ox + ((i & 1) ? 0 : half);
            uint32 cy = oy + ((i & 2) ? 0 : half);
            uint32 cz = oz + ((i & 4) ? 0 : half);
            node->children[i] = edit(node->children[i], level + 1, gridDepth,
                                     cx, cy, cz, half, lo, hi, material);
        }

        bool anyChild = false;
        bool allSolidLeaf = true;
        uint32 common = 0;
        for (int i = 0; i < 8; ++i) {
            EditNode *c = node->children[i];
            if (!c) {
                allSolidLeaf = false;
                continue;
            }
            anyChild = true;
            if (!c->isLeaf || c->material == 0) {
                allSolidLeaf = false;
            } else if (common == 0) {
                common = c->material;
            } else if (common != c->material) {
                allSolidLeaf = false;
            }
        }

        if (!anyChild)
            return 0;                 // Whole branch went empty: drop it.
        if (allSolidLeaf && common)
            return makeLeaf(common);  // Eight identical solid leaves: merge.
        return node;
    }

    int presentCount(const EditNode *node) const {
        int k = 0;
        for (int i = 0; i < 8; ++i)
            if (node->children[i])
                ++k;
        return k;
    }

    int internalCount(const EditNode *node) const {
        int k = 0;
        for (int i = 0; i < 8; ++i)
            if (node->children[i] && !node->children[i]->isLeaf)
                ++k;
        return k;
    }

    /*
       Bottom-up layout pass. A child's group offset depends only on its parent's
       slot width and the sizes of higher-index sibling subtrees, so the need for an
       extended (35 bit) offset can be decided here without absolute positions:
       a node goes double wide whenever one of its internal children's group offsets
       exceeds the 14 bit short range. Leaf slots always stay one word wide, which is
       the legal mixed-width layout understood by the parser and the ray marcher.
       Returns the number of words in the child group plus every descendant group.
    */
    int64_t layout(EditNode *node) {
        for (int i = 0; i < 8; ++i) {
            EditNode *c = node->children[i];
            if (c && !c->isLeaf)
                layout(c);
        }

        const int present = presentCount(node);
        const int internal = internalCount(node);
        const int64_t wideGroupLength = present + int64_t(internal);

        // Decide promotion against the WIDE layout directly: walk the group exactly as
        // emit() will place it (two words per internal slot, one per leaf) and measure
        // each internal child's descriptor-to-group offset. Wide offsets are never
        // smaller than narrow ones, so if they all fit the short range a narrow group
        // is safe too; otherwise the group must be extended.
        bool needsWide = false;
        int64_t slotCursor = 0;
        int64_t regionCursor = wideGroupLength;
        for (int i = 7; i >= 0; --i) {
            EditNode *c = node->children[i];
            if (!c)
                continue;
            if (!c->isLeaf) {
                int64_t childOffset = regionCursor - slotCursor;
                if (childOffset > 0x3FFF)
                    needsWide = true;
                regionCursor += c->regionWords;
                slotCursor += 2;
            } else {
                slotCursor += 1;
            }
        }

        node->wideGroup = needsWide;
        int64_t groupWords = needsWide ? wideGroupLength : present;
        int64_t region = groupWords;
        for (int i = 0; i < 8; ++i) {
            EditNode *c = node->children[i];
            if (c && !c->isLeaf)
                region += c->regionWords;
        }
        node->regionWords = region;
        return region;
    }

    static void writeDescriptor(std::vector<uint32> &out, int64_t pos,
                                uint32 childMask, uint32 internalMask,
                                bool wideGroup, bool extended, int64_t offset) {
        uint32 w = (childMask << 8) | internalMask;
        if (wideGroup)
            w |= 0x10000;
        if (extended) {
            w |= 0x20000 | uint32((offset >> 32) << 18);
            out[size_t(pos)] = w;
            out[size_t(pos + 1)] = uint32(offset);
        } else {
            w |= uint32(offset << 18);
            out[size_t(pos)] = w;
        }
    }

    /*
       Top-down placement once every region size and wide flag is known. Writes the
       descriptor at descPos and the contiguous child group at groupPos, appending
       internal subtrees in descending child-index order like the original builder.
    */
    void emit(EditNode *node, int64_t descPos, int64_t groupPos,
              bool extended, std::vector<uint32> &out) {
        int64_t groupOffset = groupPos - descPos;

        uint32 childMask = 0, internalMask = 0;
        for (int i = 0; i < 8; ++i) {
            if (node->children[i]) {
                uint32 bit = 128u >> i;
                childMask |= bit;
                if (!node->children[i]->isLeaf)
                    internalMask |= bit;
            }
        }

        const int present = presentCount(node);
        int64_t groupWords = present + (node->wideGroup ? internalCount(node) : 0);

        // First pass: assign each internal child a descriptor slot and a group region.
        int64_t childDescPos[8], childGroupPos[8];
        for (int i = 0; i < 8; ++i) {
            childDescPos[i] = -1;
            childGroupPos[i] = -1;
        }
        int64_t wordPos = 0;
        int64_t appendedCursor = groupPos + groupWords;
        for (int i = 7; i >= 0; --i) {
            EditNode *c = node->children[i];
            if (!c)
                continue;
            if (!c->isLeaf) {
                childDescPos[i] = groupPos + wordPos;
                childGroupPos[i] = appendedCursor;
                appendedCursor += c->regionWords;
            }
            wordPos += (node->wideGroup && c && !c->isLeaf) ? 2 : 1;
        }

        // Leaf materials occupy their single-word slots.
        wordPos = 0;
        for (int i = 7; i >= 0; --i) {
            EditNode *c = node->children[i];
            if (!c)
                continue;
            if (c->isLeaf)
                out[size_t(groupPos + wordPos)] = c->material;
            wordPos += (node->wideGroup && !c->isLeaf) ? 2 : 1;
        }

        writeDescriptor(out, descPos, childMask, internalMask,
                        node->wideGroup, extended, groupOffset);

        // Recurse; an internal child is extended exactly when this group is wide.
        for (int i = 7; i >= 0; --i) {
            EditNode *c = node->children[i];
            if (c && !c->isLeaf)
                emit(c, childDescPos[i], childGroupPos[i], node->wideGroup, out);
        }
    }
};

} // namespace

void VoxelOctree::paintBox(unsigned gridDepth,
                           const std::array<uint32, 3> &lo,
                           const std::array<uint32, 3> &hi,
                           uint32 material) {
    // Empty brush: nothing to do.
    if (lo[0] >= hi[0] || lo[1] >= hi[1] || lo[2] >= hi[2])
        return;

    // Be robust against a failed file load: start from an empty root.
    if (!_octree) {
        _octreeSize = 1;
        _octree.reset(new uint32[1]);
        _octree[0] = 0;
    }

    Editor editor;
    EditNode *root = editor.parse(_octree.get(), 0, true);

    uint32 span = 1u << gridDepth;
    root = editor.edit(root, 0, int(gridDepth), 0, 0, 0, span, lo, hi, material);

    // The root always stays an internal descriptor (word 0 is parsed as one).
    if (!root) {
        root = editor.makeNode();                  // Fully empty: single-word root.
    } else if (root->isLeaf) {
        // Whole-scene uniform material: root plus eight identical leaves (9 words).
        uint32 uniform = root->material;
        EditNode *expanded = editor.makeNode();
        for (int i = 0; i < 8; ++i)
            expanded->children[i] = editor.makeLeaf(uniform);
        root = expanded;
    }

    // Bottom-up width decisions and region sizes, then top-down placement. The root
    // descriptor is word 0 and its group starts at word 1, so it is never extended.
    int64_t totalWords = 1 + editor.layout(root);

    std::vector<uint32> words(size_t(totalWords), 0);
    editor.emit(root, 0, 1, false, words);

    std::unique_ptr<uint32[]> packed(new uint32[words.size()]);
    for (size_t i = 0; i < words.size(); ++i)
        packed[i] = words[i];

    _octree.reset(packed.release());
    _octreeSize = words.size();
    // Editing never touches the source voxel data; drop the stale reference.
    _voxels = 0;
}
