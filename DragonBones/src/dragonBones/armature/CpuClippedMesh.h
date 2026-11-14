/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2012-2018 DragonBones team and other contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef DRAGONBONES_CPU_CLIPPED_MESH_H
#define DRAGONBONES_CPU_CLIPPED_MESH_H

#include "../core/DragonBones.h"
#include "../geom/ColorTransform.h"

DRAGONBONES_NAMESPACE_BEGIN

class Armature;

/**
 * - A convex clipping polygon described in a mask slot's local coordinate system.
 * The polygon is transformed to armature space with the mask slot's current
 * globalTransformMatrix, so the mask follows the bone it is attached to.
 * @version DragonBones 5.5
 * @language en_US
 */
struct CpuMaskPolygon
{
    std::vector<float> points; ///< Convex polygon vertices, x/y pairs, at least three points.

    CpuMaskPolygon() {}
    CpuMaskPolygon(std::vector<float> value) : points(std::move(value)) {}
};

/**
 * - One clipping range, interpreted against the current frame's slot draw order.
 * The range opens after maskSlot is drawn and closes after endSlot is drawn.
 * The mask slot itself never emits geometry. Nested ranges intersect; crossing
 * ranges are rejected.
 * @version DragonBones 5.5
 * @language en_US
 */
struct CpuMaskRange
{
    std::string maskSlot;
    std::string endSlot;
    CpuMaskPolygon polygon;

    CpuMaskRange() {}
    CpuMaskRange(const std::string& maskSlotName, const std::string& endSlotName, CpuMaskPolygon maskPolygon) :
        maskSlot(maskSlotName),
        endSlot(endSlotName),
        polygon(std::move(maskPolygon))
    {}
};

/**
 * - One corner of a clipped output triangle.
 * @version DragonBones 5.5
 * @language en_US
 */
struct CpuMeshVertex
{
    float x;        ///< Armature space position.
    float y;
    float u;        ///< Original normalized mesh UV.
    float v;

    CpuMeshVertex() : x(0.0f), y(0.0f), u(0.0f), v(0.0f) {}
};

/**
 * - One output triangle. The winding order of the source triangle is preserved.
 * @version DragonBones 5.5
 * @language en_US
 */
struct CpuMeshTriangle
{
    CpuMeshVertex vertices[3];
    unsigned sourceTriangle; ///< Index of the triangle in the source mesh.

    CpuMeshTriangle() : sourceTriangle(0) {}
};

/**
 * - Self contained clipped mesh data of one slot.
 * Every value is copied: keeping the package after the armature advances,
 * swaps its display or is released does not change the package.
 * @version DragonBones 5.5
 * @language en_US
 */
struct CpuSlotMesh
{
    std::string slotName;
    std::string displayName;
    std::string materialPath;
    BlendMode blendMode;
    ColorTransform colorTransform;
    std::vector<CpuMeshTriangle> triangles;

    CpuSlotMesh() :
        blendMode(BlendMode::Normal)
    {}
};

/**
 * - Build the current frame's mesh geometry on the CPU, clipped by convex mask
 * ranges. The armature must already have been advanceTime()d for this frame;
 * this function only reads the current pose and synchronizes slot transform
 * caches, it never advances animation, changes the pose, reorders or writes the
 * armature data, deform buffers or display lists, and never calls any engine
 * adapter (SFML, Cocos, OpenGL, ...).
 *
 * Supported current displays: normal Mesh and weighted Mesh. Slots marked as
 * masks may be empty slots or BoundingBox displays. Active Image, child
 * Armature and Path displays are explicitly rejected. With no mask range the
 * same CPU pipeline returns the full, fully deformed current meshes.
 *
 * @param armature - The armature whose current frame is read.
 * @param ranges - Convex mask ranges, interpreted with this frame's draw order.
 * @param out - Output packages, ordered by the current slot draw order; slots
 * with no area left after clipping are omitted, slots are never merged.
 * @returns false on invalid slot references, non convex or non finite
 * polygons, invalid current ranges (crossing intervals), unsupported active
 * displays, or malformed mesh data.
 * @version DragonBones 5.5
 * @language en_US
 */
bool buildClippedMesh(
    Armature& armature,
    const std::vector<CpuMaskRange>& ranges,
    std::vector<CpuSlotMesh>& out
);

DRAGONBONES_NAMESPACE_END
#endif // DRAGONBONES_CPU_CLIPPED_MESH_H
