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

#include <string>
#include <vector>

#include "../core/DragonBones.h"
#include "../geom/ColorTransform.h"

DRAGONBONES_NAMESPACE_BEGIN

class Armature;

/**
 * A single point of a CPU mask polygon, expressed in the mask slot's local
 * coordinate system.
 */
struct CpuMaskPoint
{
    float x;
    float y;
};

/**
 * A draw-order scoped clipping range.
 * The mask starts after maskSlot has been drawn and ends when endSlot has
 * been drawn. The mask slot itself never emits geometry. The polygon is convex,
 * contains at least three points and is defined in maskSlot local coordinates.
 */
struct CpuMaskRange
{
    std::string maskSlot;
    std::string endSlot;
    std::vector<CpuMaskPoint> polygon;
};

/**
 * A deformed mesh vertex in armature space together with the original
 * normalized mesh UV coordinates.
 */
struct CpuMeshVertex
{
    float x;
    float y;
    float u;
    float v;
};

/**
 * One output triangle. sourceTriangle is the index of the triangle in the
 * source mesh data. Winding order always follows the source triangle.
 */
struct CpuMeshTriangle
{
    CpuMeshVertex vertices[3];
    unsigned sourceTriangle;
};

/**
 * Self contained clipped mesh package for a single slot. Every field holds its
 * own copy, so advancing the armature, replacing displays or disposing the
 * source armature can never invalidate an existing package.
 */
struct CpuSlotMesh
{
    std::string slotName;
    std::string displayName;
    std::string materialPath;
    BlendMode blendMode;
    ColorTransform colorTransform;
    std::vector<CpuMeshTriangle> triangles;
};

/**
 * Build device independent, deformed and clipped triangle geometry for the
 * armature's current frame. The armature is expected to have been advanced
 * already; this call only reads the current pose (flushing pending slot
 * transform caches when needed), never advances animation, never changes the
 * pose and never touches any graphics adapter.
 *
 * Returns false (and clears output) on illegal slot references, non convex or
 * non finite mask polygons, invalid or crossing ranges, or active displays
 * that are not supported (Image, child Armature, Path). An empty range list
 * runs the same CPU deformation path and returns the full current meshes.
 */
bool buildClippedMesh(
    Armature& armature,
    const std::vector<CpuMaskRange>& ranges,
    std::vector<CpuSlotMesh>& output
);

DRAGONBONES_NAMESPACE_END
#endif // DRAGONBONES_CPU_CLIPPED_MESH_H
