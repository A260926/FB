/*
MIT License

Copyright (c) 2018-2020 Jonathan Young

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#pragma once
#ifndef XATLAS_MULTIPOSE_H
#define XATLAS_MULTIPOSE_H

#include <stdint.h>

// Shared-UV parameterization for a mesh with several discrete vertex poses.
//
// The base pose (weight 1) is the mesh's binding pose. Each additional pose is
// a set of vertex positions using the same vertex numbering and face
// connectivity. All poses share one set of UV chart unknowns: the solve does
// not parameterize each pose separately and does not average the results.
namespace xatlas {
namespace multipose {

// Solve the shared UV objective for one chart.
//
// For every face and every pose, the face is expressed in local complex
// coordinates z0 = 0, z1 = |p1 - p0| and
// z2 = dot(p2 - p0, e1) + i * |p2 - p0 - dot(p2 - p0, e1) * e1|, where e1 is the
// unit direction of the first edge. The objective minimized is
//
//   sum over poses, faces of
//       weight * |(z1 - z0) * (U2 - U0) - (z2 - z0) * (U1 - U0)|^2
//               / (2 * 3D face area)
//
// The UV translation, rotation and scale degrees of freedom are fixed by
// locking pinVertex0 and pinVertex1 to pinUv0 and pinUv1.
//
// basePositions points to 3 * vertexCount floats. extraPositions points to
// 3 * vertexCount * extraPoseCount floats, interleaved per vertex (the extra
// poses of vertex v are at ((v * extraPoseCount) + p) * 3). extraWeights has
// extraPoseCount entries; the implicit base pose has weight 1.
//
// uvOut must contain 2 * vertexCount floats holding the initial UV coordinates
// for every vertex (the starting point of the iterative solve). On success it
// is overwritten with the solution. On failure it is left untouched, so
// callers never observe a partially solved parameterization.
bool SolveChart(uint32_t vertexCount, const uint32_t *indices, uint32_t indexCount,
	const float *basePositions, const float *extraPositions, uint32_t extraPoseCount,
	const float *extraWeights,
	uint32_t pinVertex0, uint32_t pinVertex1,
	const float pinUv0[2], const float pinUv1[2],
	float *uvOut);

} // namespace multipose
} // namespace xatlas

#endif // XATLAS_MULTIPOSE_H
