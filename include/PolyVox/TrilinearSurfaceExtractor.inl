/*******************************************************************************
* The MIT License (MIT)
*
* Copyright (c) 2015 David Williams and Matthew Williams
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*******************************************************************************/

#include "Impl/ErrorHandling.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace PolyVox
{
	inline trilinear_degenerate_field::trilinear_degenerate_field(const Vector3DInt32& v3dCellLowerCorner, const std::string& strMessage)
		: std::runtime_error(strMessage)
		, m_v3dCellLowerCorner(v3dCellLowerCorner)
	{
	}

	inline Vector3DInt32 trilinear_degenerate_field::getCellPosition(void) const
	{
		return m_v3dCellLowerCorner;
	}

	namespace Impl
	{
		struct TrilinearNodeKey
		{
			int32_t x;
			int32_t y;
			int32_t z;

			bool operator==(const TrilinearNodeKey& rhs) const
			{
				return x == rhs.x && y == rhs.y && z == rhs.z;
			}
		};

		struct TrilinearEdgeKey
		{
			int32_t x;
			int32_t y;
			int32_t z;
			uint8_t axis; // 0 = x, 1 = y, 2 = z; (x,y,z) is the lower endpoint of the edge

			bool operator==(const TrilinearEdgeKey& rhs) const
			{
				return x == rhs.x && y == rhs.y && z == rhs.z && axis == rhs.axis;
			}
		};

		struct TrilinearNodeKeyHash
		{
			std::size_t operator()(const TrilinearNodeKey& key) const
			{
				std::size_t seed = std::hash<int32_t>()(key.x);
				seed ^= std::hash<int32_t>()(key.y) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
				seed ^= std::hash<int32_t>()(key.z) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
				return seed;
			}
		};

		struct TrilinearEdgeKeyHash
		{
			std::size_t operator()(const TrilinearEdgeKey& key) const
			{
				std::size_t seed = std::hash<int32_t>()(key.x);
				seed ^= std::hash<int32_t>()(key.y) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
				seed ^= std::hash<int32_t>()(key.z) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
				seed ^= std::hash<int32_t>()(key.axis) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
				return seed;
			}
		};

		// Corner numbering inside a cell matches the classic Marching Cubes convention:
		// bit 0 = x, bit 1 = y, bit 2 = z.
		static const int32_t TrilinearCornerOffset[8][3] =
		{
			{ 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 },
			{ 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }
		};

		// The six faces as cyclic lists of four corner indices.
		static const int32_t TrilinearFaceCorners[6][4] =
		{
			{ 0, 2, 6, 4 }, // x = 0
			{ 1, 3, 7, 5 }, // x = 1
			{ 0, 1, 5, 4 }, // y = 0
			{ 2, 3, 7, 6 }, // y = 1
			{ 0, 1, 3, 2 }, // z = 0
			{ 4, 5, 7, 6 }  // z = 1
		};

		// Canonical edge identifiers. Index = axis * 4 + (fixed bits packed in the
		// order of the two axes other than 'axis', lowest other axis first).
		// Each entry stores the lower and upper corner index of that edge.
		struct TrilinearEdgeInfo
		{
			int32_t lowCorner;
			int32_t highCorner;
		};

		inline int32_t trilinearEdgeId(int32_t lowCorner, int32_t highCorner)
		{
			int32_t axis = 0;
			while (axis < 3 && ((lowCorner >> axis) & 1) == ((highCorner >> axis) & 1))
			{
				++axis;
			}
			int32_t packed = 0;
			int32_t bit = 0;
			for (int32_t candidate = 0; candidate < 3; ++candidate)
			{
				if (candidate == axis) continue;
				packed |= ((lowCorner >> candidate) & 1) << bit;
				++bit;
			}
			return axis * 4 + packed;
		}

		inline TrilinearEdgeInfo trilinearEdgeInfo(int32_t edgeId)
		{
			const int32_t axis = edgeId / 4;
			const int32_t packed = edgeId % 4;
			int32_t lowCorner = 0;
			int32_t bit = 0;
			for (int32_t candidate = 0; candidate < 3; ++candidate)
			{
				if (candidate == axis) continue;
				lowCorner |= ((packed >> bit) & 1) << candidate;
				++bit;
			}
			return TrilinearEdgeInfo{ lowCorner, lowCorner | (1 << axis) };
		}

		inline float trilinearDeterminant3(const float m[3][3])
		{
			return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
				- m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
				+ m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		}

		// Eigenvalues and orthonormal eigenvectors of a symmetric 3x3 matrix,
		// obtained by cyclic Jacobi rotations. Eigenvalues come out unordered.
		inline void trilinearSymmetricEigen3(float m[3][3], float eigenvalues[3], float eigenvectors[3][3])
		{
			float a[3][3];
			for (int32_t i = 0; i < 3; ++i)
				for (int32_t j = 0; j < 3; ++j)
					a[i][j] = m[i][j];

			float v[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };

			for (int32_t sweep = 0; sweep < 40; ++sweep)
			{
				float offDiagonal = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
				if (offDiagonal < 1.0e-20f) break;

				for (int32_t p = 0; p < 2; ++p)
				{
					for (int32_t q = p + 1; q < 3; ++q)
					{
						if (std::fabs(a[p][q]) < 1.0e-20f) continue;
						const float tau = (a[q][q] - a[p][p]) / (2.0f * a[p][q]);
						const float t = (tau >= 0.0f ? 1.0f : -1.0f) /
							(std::fabs(tau) + std::sqrt(1.0f + tau * tau));
						const float c = 1.0f / std::sqrt(1.0f + t * t);
						const float s = t * c;

						for (int32_t k = 0; k < 3; ++k)
						{
							const float apk = a[p][k];
							const float aqk = a[q][k];
							a[p][k] = c * apk - s * aqk;
							a[q][k] = s * apk + c * aqk;
						}
						for (int32_t k = 0; k < 3; ++k)
						{
							const float akp = a[k][p];
							const float akq = a[k][q];
							a[k][p] = c * akp - s * akq;
							a[k][q] = s * akp + c * akq;
						}
						for (int32_t k = 0; k < 3; ++k)
						{
							const float vkp = v[k][p];
							const float vkq = v[k][q];
							v[k][p] = c * vkp - s * vkq;
							v[k][q] = s * vkp + c * vkq;
						}
					}
				}
			}

			for (int32_t i = 0; i < 3; ++i)
			{
				eigenvalues[i] = a[i][i];
				for (int32_t j = 0; j < 3; ++j)
				{
					eigenvectors[j][i] = v[j][i];
				}
			}
		}
	}

	template< typename VolumeType, typename ControllerType >
	Mesh< Vertex<typename VolumeType::VoxelType> > extractTrilinearMesh(VolumeType* volData, Region region, ControllerType controller, float degeneracy_tolerance)
	{
		typedef typename VolumeType::VoxelType VoxelType;

		POLYVOX_THROW_IF(volData == nullptr, std::invalid_argument, "Provided volume cannot be null");
		POLYVOX_THROW_IF(!(degeneracy_tolerance > 0.0f), std::invalid_argument, "degeneracy_tolerance must be a positive number");

		Mesh< Vertex<VoxelType> > result;

		const int32_t lx = region.getLowerX();
		const int32_t ly = region.getLowerY();
		const int32_t lz = region.getLowerZ();
		const int32_t ux = region.getUpperX();
		const int32_t uy = region.getUpperY();
		const int32_t uz = region.getUpperZ();

		const float threshold = static_cast<float>(controller.getThreshold());

		// One shared vertex per global lattice edge, and one cached central-difference
		// normal per lattice node, for the whole extraction.
		std::unordered_map<Impl::TrilinearEdgeKey, uint32_t, Impl::TrilinearEdgeKeyHash> edgeVertexIndices;
		std::unordered_map<Impl::TrilinearNodeKey, Vector3DFloat, Impl::TrilinearNodeKeyHash> nodeNormals;

		auto sampleDensity = [&](int32_t x, int32_t y, int32_t z) -> float
		{
			return static_cast<float>(controller.convertToDensity(volData->getVoxel(x, y, z)));
		};

		// Central difference normal, pointing from the high density side towards the
		// low density side (the convention used by the Marching Cubes extractor).
		auto nodeNormal = [&](int32_t x, int32_t y, int32_t z) -> Vector3DFloat
		{
			const Impl::TrilinearNodeKey key{ x, y, z };
			auto found = nodeNormals.find(key);
			if (found != nodeNormals.end())
			{
				return found->second;
			}
			const Vector3DFloat normal(
				sampleDensity(x - 1, y, z) - sampleDensity(x + 1, y, z),
				sampleDensity(x, y - 1, z) - sampleDensity(x, y + 1, z),
				sampleDensity(x, y, z - 1) - sampleDensity(x, y, z + 1));
			nodeNormals[key] = normal;
			return normal;
		};

		auto failDegenerate = [&](const Vector3DInt32& cell, const char* reason) -> void
		{
			std::stringstream ss;
			ss << "Trilinear surface extraction cannot reliably classify cell at ("
				<< cell.getX() << ", " << cell.getY() << ", " << cell.getZ()
				<< "): " << reason;
			// Thrown directly (rather than through POLYVOX_THROW) so the exception can
			// carry the global cell coordinate alongside the message.
			throw trilinear_degenerate_field(cell, ss.str());
		};

		// Creates (or returns the existing index of) the surface vertex on a
		// sign-changing lattice edge.
		auto edgeVertex = [&](const Impl::TrilinearEdgeKey& edgeKey, float lowDensity, float highDensity,
			const VoxelType& lowVoxel, const VoxelType& highVoxel,
			const Vector3DFloat& lowNormal, const Vector3DFloat& highNormal) -> uint32_t
		{
			auto found = edgeVertexIndices.find(edgeKey);
			if (found != edgeVertexIndices.end())
			{
				return found->second;
			}

			// Linear root finding along the edge.
			const float t = (threshold - lowDensity) / (highDensity - lowDensity);

			Vector3DFloat position(static_cast<float>(edgeKey.x - lx),
				static_cast<float>(edgeKey.y - ly),
				static_cast<float>(edgeKey.z - lz));
			position.setElement(edgeKey.axis, position.getElement(edgeKey.axis) + t);

			Vector3DFloat normal = lowNormal * (1.0f - t) + highNormal * t;
			if (normal.lengthSquared() <= 1.0e-12f)
			{
				// The neighbourhood can be flat at an isolated node (e.g. alternating
				// solid/empty voxels); fall back to the edge direction so the normal
				// stays finite and points towards the low density endpoint.
				normal = Vector3DFloat(0.0f, 0.0f, 0.0f);
				normal.setElement(edgeKey.axis, lowDensity - highDensity);
			}
			normal.normalise();

			Vertex<VoxelType> vertex;
			vertex.position = position;
			vertex.normal = normal;
			vertex.data = controller.blendMaterials(lowVoxel, highVoxel, t);

			const uint32_t index = result.addVertex(vertex);
			edgeVertexIndices[edgeKey] = index;
			return index;
		};

		for (int32_t cz = lz; cz < uz; ++cz)
		{
			for (int32_t cy = ly; cy < uy; ++cy)
			{
				for (int32_t cx = lx; cx < ux; ++cx)
				{
					const Vector3DInt32 cellPosition(cx, cy, cz);

					// Gather the eight corner voxels and their density deviation from
					// the threshold.
					std::array<float, 8> deviation;
					std::array<float, 8> density;
					std::array<VoxelType, 8> voxel;
					float largestDeviation = 0.0f;
					bool hasNegative = false;
					bool hasPositive = false;
					bool finiteValues = true;

					for (int32_t corner = 0; corner < 8; ++corner)
					{
						const int32_t x = cx + Impl::TrilinearCornerOffset[corner][0];
						const int32_t y = cy + Impl::TrilinearCornerOffset[corner][1];
						const int32_t z = cz + Impl::TrilinearCornerOffset[corner][2];
						voxel[corner] = volData->getVoxel(x, y, z);
						density[corner] = static_cast<float>(controller.convertToDensity(voxel[corner]));
						deviation[corner] = density[corner] - threshold;
						if (!std::isfinite(density[corner]) || !std::isfinite(deviation[corner]))
						{
							finiteValues = false;
						}
						largestDeviation = (std::max)(largestDeviation, std::fabs(deviation[corner]));
						hasNegative = hasNegative || deviation[corner] < 0.0f;
						hasPositive = hasPositive || deviation[corner] > 0.0f;
					}

					if (!finiteValues)
					{
						failDegenerate(cellPosition, "a corner density is not a finite real number");
					}

					// A field constant at the iso level is degenerate at every corner.
					if (largestDeviation <= 0.0f)
					{
						failDegenerate(cellPosition, "every corner lies on the iso level");
					}

					// Decisions use deviations normalised by the largest corner
					// deviation inside the cell, making the tolerance scale independent.
					// The corner proximity test runs before the one-side skip so that an
					// iso level passing through (or grazing) a corner is always reported,
					// even when the remaining corners happen to share one sign.
					const float scale = largestDeviation;
					std::array<float, 8> q;
					for (int32_t corner = 0; corner < 8; ++corner)
					{
						q[corner] = deviation[corner] / scale;
						if (std::fabs(q[corner]) < degeneracy_tolerance)
						{
							failDegenerate(cellPosition, "the iso level passes too close to a cell corner");
						}
					}

					// A cell entirely on one side of the iso level is skipped normally;
					// its corners have already been shown to be well away from the level.
					if (!hasNegative || !hasPositive)
					{
						continue;
					}

					// Trilinear expansion in U = 2x-1, V = 2y-1, W = 2z-1:
					// F = c0 + aU + bV + cW + dUV + eUW + fVW (no UVW term).
					float c0 = 0.0f, a = 0.0f, b = 0.0f, c = 0.0f;
					float d = 0.0f, e = 0.0f, f = 0.0f;
					for (int32_t corner = 0; corner < 8; ++corner)
					{
						const float U = (corner & 1) ? 1.0f : -1.0f;
						const float V = (corner & 2) ? 1.0f : -1.0f;
						const float W = (corner & 4) ? 1.0f : -1.0f;
						const float value = q[corner];
						c0 += value;
						a += value * U;
						b += value * V;
						c += value * W;
						d += value * U * V;
						e += value * U * W;
						f += value * V * W;
					}
					c0 *= 0.125f; a *= 0.125f; b *= 0.125f; c *= 0.125f;
					d *= 0.125f; e *= 0.125f; f *= 0.125f;

					auto fieldAt = [&](float px, float py, float pz) -> float
					{
						const float U = 2.0f * px - 1.0f;
						const float V = 2.0f * py - 1.0f;
						const float W = 2.0f * pz - 1.0f;
						return c0 + a * U + b * V + c * W + d * U * V + e * U * W + f * V * W;
					};

					// Low-density oriented gradient of the trilinear field at a cell
					// local point.
					auto fieldGradientTowardsLow = [&](float px, float py, float pz) -> Vector3DFloat
					{
						const float U = 2.0f * px - 1.0f;
						const float V = 2.0f * py - 1.0f;
						const float W = 2.0f * pz - 1.0f;
						const float dFdU = a + d * V + e * W;
						const float dFdV = b + d * U + f * W;
						const float dFdW = c + e * U + f * V;
						return Vector3DFloat(-2.0f * dFdU, -2.0f * dFdV, -2.0f * dFdW);
					};

					// ---- Face examination ------------------------------------------------
					bool faceIsAmbiguous[6] = { false };
					bool faceSaddleNegative[6] = { false };

					for (int32_t face = 0; face < 6; ++face)
					{
						const int32_t i0 = Impl::TrilinearFaceCorners[face][0];
						const int32_t i1 = Impl::TrilinearFaceCorners[face][1];
						const int32_t i2 = Impl::TrilinearFaceCorners[face][2];
						const int32_t i3 = Impl::TrilinearFaceCorners[face][3];
						const float q0 = q[i0];
						const float q1 = q[i1];
						const float q2 = q[i2];
						const float q3 = q[i3];

						const bool alternating =
							(q0 < 0.0f) != (q1 < 0.0f) &&
							(q1 < 0.0f) != (q2 < 0.0f) &&
							(q2 < 0.0f) != (q3 < 0.0f) &&
							(q3 < 0.0f) != (q0 < 0.0f);

						// Bilinear saddle of q0,q1,q2 (u=v=1), q3 (u=0,v=1).
						const float A = q3 - q0; // d/dv along u = 0
						const float B = q2 - q1; // d/dv along u = 1
						const float C = q1 - q0; // d/du along v = 0
						const float D = q2 - q3; // d/du along v = 1
						const bool solvable = std::fabs(A - B) > 1.0e-12f && std::fabs(C - D) > 1.0e-12f;
						float uStar = 0.5f;
						float vStar = 0.5f;
						bool saddleInside = false;
						if (solvable)
						{
							uStar = A / (A - B);
							vStar = C / (C - D);
							saddleInside = uStar > 0.0f && uStar < 1.0f && vStar > 0.0f && vStar < 1.0f;
						}

						if (saddleInside)
						{
							const float saddleValue =
								(1.0f - vStar) * ((1.0f - uStar) * q0 + uStar * q1) +
								vStar * ((1.0f - uStar) * q3 + uStar * q2);
							if (std::fabs(saddleValue) < degeneracy_tolerance)
							{
								failDegenerate(cellPosition, "the iso level passes too close to a face saddle");
							}
							if (alternating)
							{
								faceIsAmbiguous[face] = true;
								faceSaddleNegative[face] = saddleValue < 0.0f;
							}
						}
						else if (alternating)
						{
							failDegenerate(cellPosition, "could not locate the saddle of an ambiguous face");
						}
					}

					// ---- Interior critical point ----------------------------------------
					// The gradient is linear, so there is at most one interior critical
					// point. Its Hessian has zero trace, so it is always a saddle (never a
					// local extremum), hence every surface component meets the boundary.
					bool hasInteriorCritical = false;
					Vector3DFloat criticalLocal(0.5f, 0.5f, 0.5f);
					float criticalValue = 0.0f;
					// When the saddle merges two corner basins, these are the two
					// corners reached by gradient lines along the responsible eigen
					// direction.
					int32_t saddleMergeCorners[2] = { -1, -1 };
					bool saddleMergeActive = false;
					{
						// dF/dU = a + dV + eW = 0
						// dF/dV = b + dU + fW = 0
						// dF/dW = c + eU + fV = 0
						float matrix[3][3] =
						{
							{ 0.0f, d, e },
							{ d, 0.0f, f },
							{ e, f, 0.0f }
						};
						const float determinant = Impl::trilinearDeterminant3(matrix);
						if (std::fabs(determinant) > 1.0e-12f)
						{
							const float rhs[3] = { -a, -b, -c };
							float solution[3] = { 0.0f, 0.0f, 0.0f };
							for (int32_t column = 0; column < 3; ++column)
							{
								float replaced[3][3];
								for (int32_t row = 0; row < 3; ++row)
								{
									for (int32_t col = 0; col < 3; ++col)
									{
										replaced[row][col] = (col == column) ? rhs[row] : matrix[row][col];
									}
								}
								solution[column] = Impl::trilinearDeterminant3(replaced) / determinant;
							}
							const float U = solution[0];
							const float V = solution[1];
							const float W = solution[2];

							if (std::isfinite(U) && std::isfinite(V) && std::isfinite(W) &&
								U > -1.0f && U < 1.0f && V > -1.0f && V < 1.0f && W > -1.0f && W < 1.0f)
							{
								hasInteriorCritical = true;
								criticalLocal = Vector3DFloat((U + 1.0f) * 0.5f, (V + 1.0f) * 0.5f, (W + 1.0f) * 0.5f);
								criticalValue = c0 + a * U + b * V + c * W + d * U * V + e * U * W + f * V * W;
								if (std::fabs(criticalValue) < degeneracy_tolerance)
								{
									failDegenerate(cellPosition, "the iso level passes too close to the interior critical point");
								}

								// Classify the saddle. The Hessian has zero trace, so an
								// interior critical point has index 1 (one negative
								// eigenvalue) or index 2 (one positive eigenvalue).
								float eigenvalues[3];
								float eigenvectors[3][3];
								Impl::trilinearSymmetricEigen3(matrix, eigenvalues, eigenvectors);

								// Trace a gradient line from a small offset along the
								// chosen eigenvector until it reaches a corner basin.
								// descent == true follows -grad(F) (low density direction),
								// false follows +grad(F).
								auto traceToCorner = [&](const Vector3DFloat& eigenDirection, bool descent) -> int32_t
								{
									Vector3DFloat position = criticalLocal + eigenDirection * 1.0e-4f;
									const float stepLength = 0.005f;
									for (int32_t iteration = 0; iteration < 40000; ++iteration)
									{
										int32_t nearestCorner = 0;
										float nearestDistance = std::numeric_limits<float>::infinity();
										for (int32_t corner = 0; corner < 8; ++corner)
										{
											const float dx = position.getX() - static_cast<float>(Impl::TrilinearCornerOffset[corner][0]);
											const float dy = position.getY() - static_cast<float>(Impl::TrilinearCornerOffset[corner][1]);
											const float dz = position.getZ() - static_cast<float>(Impl::TrilinearCornerOffset[corner][2]);
											const float distanceSquared = dx * dx + dy * dy + dz * dz;
											if (distanceSquared < nearestDistance)
											{
												nearestDistance = distanceSquared;
												nearestCorner = corner;
											}
										}
										if (nearestDistance < 1.0e-6f)
										{
											return nearestCorner;
										}

										Vector3DFloat direction = fieldGradientTowardsLow(position.getX(), position.getY(), position.getZ());
										if (!descent) direction = direction * -1.0f;
										const float gradientLength = std::sqrt(direction.lengthSquared());
										if (gradientLength < 1.0e-12f)
										{
											return nearestCorner; // converged onto another critical point
										}
										direction = direction * (1.0f / gradientLength);
										position += direction * stepLength;
										position.setX((std::min)(1.0f - 1.0e-6f, (std::max)(1.0e-6f, position.getX())));
										position.setY((std::min)(1.0f - 1.0e-6f, (std::max)(1.0e-6f, position.getY())));
										position.setZ((std::min)(1.0f - 1.0e-6f, (std::max)(1.0e-6f, position.getZ())));
									}

									int32_t fallbackCorner = 0;
									float fallbackDistance = std::numeric_limits<float>::infinity();
									for (int32_t corner = 0; corner < 8; ++corner)
									{
										const float dx = position.getX() - static_cast<float>(Impl::TrilinearCornerOffset[corner][0]);
										const float dy = position.getY() - static_cast<float>(Impl::TrilinearCornerOffset[corner][1]);
										const float dz = position.getZ() - static_cast<float>(Impl::TrilinearCornerOffset[corner][2]);
										const float distanceSquared = dx * dx + dy * dy + dz * dz;
										if (distanceSquared < fallbackDistance)
										{
											fallbackDistance = distanceSquared;
											fallbackCorner = corner;
										}
									}
									return fallbackCorner;
								};

								// An interior critical point is always a saddle. Its
								// Hessian has zero trace, so it is either index 1 (one
								// negative eigenvalue) or index 2 (two). Empirically, the
								// only event that joins two boundary loops of a trilinear
								// cell is an index 2 saddle whose value is above the iso
								// level: the two positive (high density) pockets become one
								// connected region, so their bounding loops share an interior
								// vertex. Index 1 saddles and saddles on the far side of the
								// level leave every boundary loop as a separate sheet.
								int32_t positiveEigenIndex = -1;
								int32_t negativeCount = 0;
								for (int32_t eigenIndex = 0; eigenIndex < 3; ++eigenIndex)
								{
									if (eigenvalues[eigenIndex] < 0.0f)
									{
										++negativeCount;
									}
									else
									{
										positiveEigenIndex = eigenIndex;
									}
								}

								const bool indexTwo = negativeCount == 2;
								if (indexTwo && criticalValue > 0.0f && positiveEigenIndex >= 0)
								{
									// The single positive eigenvector spans the ridge that
									// connects the two maxima; following the gradient uphill
									// from either side reaches the two positive corners.
									const Vector3DFloat direction(eigenvectors[0][positiveEigenIndex],
										eigenvectors[1][positiveEigenIndex], eigenvectors[2][positiveEigenIndex]);
									const int32_t cornerA = traceToCorner(direction, false);
									const int32_t cornerB = traceToCorner(direction * -1.0f, false);
									if (cornerA >= 0 && cornerB >= 0 && cornerA != cornerB &&
										q[cornerA] > 0.0f && q[cornerB] > 0.0f)
									{
										saddleMergeCorners[0] = cornerA;
										saddleMergeCorners[1] = cornerB;
										saddleMergeActive = true;
									}
								}
							}
						}
					}

					// ---- Edge vertices ---------------------------------------------------
					std::array<int32_t, 12> edgeMeshIndex;
					edgeMeshIndex.fill(-1);

					for (int32_t edgeId = 0; edgeId < 12; ++edgeId)
					{
						const Impl::TrilinearEdgeInfo info = Impl::trilinearEdgeInfo(edgeId);
						const int32_t lowCorner = info.lowCorner;
						const int32_t highCorner = info.highCorner;
						if ((q[lowCorner] < 0.0f) == (q[highCorner] < 0.0f))
						{
							continue;
						}
						const int32_t axis = edgeId / 4;
						int32_t anchor[3] = { cx, cy, cz };
						anchor[0] += Impl::TrilinearCornerOffset[lowCorner][0];
						anchor[1] += Impl::TrilinearCornerOffset[lowCorner][1];
						anchor[2] += Impl::TrilinearCornerOffset[lowCorner][2];
						const Impl::TrilinearEdgeKey edgeKey{ anchor[0], anchor[1], anchor[2], static_cast<uint8_t>(axis) };

						const Vector3DFloat lowNormal = nodeNormal(
							cx + Impl::TrilinearCornerOffset[lowCorner][0],
							cy + Impl::TrilinearCornerOffset[lowCorner][1],
							cz + Impl::TrilinearCornerOffset[lowCorner][2]);
						const Vector3DFloat highNormal = nodeNormal(
							cx + Impl::TrilinearCornerOffset[highCorner][0],
							cy + Impl::TrilinearCornerOffset[highCorner][1],
							cz + Impl::TrilinearCornerOffset[highCorner][2]);

						edgeMeshIndex[edgeId] = edgeVertex(edgeKey,
							density[lowCorner], density[highCorner],
							voxel[lowCorner], voxel[highCorner],
							lowNormal, highNormal);
					}

					// ---- Face arcs and boundary loops ------------------------------------
					// Each arc separates one negative boundary region from one positive
					// boundary region; those regions are recorded alongside the arc so the
					// loops built from the arcs can be labelled without any sampling.
					struct Arc
					{
						uint32_t first;
						uint32_t second;
						int32_t negativeBasin; // boundary-region root on the low density side
						int32_t positiveBasin; // boundary-region root on the high density side
					};
					std::vector<Arc> arcs;

					// Boundary-region connectivity: same-sign corners join along lattice
					// edges and across ambiguous faces (the diagonal matching the bilinear
					// saddle). This correctly describes regions restricted to the cube
					// surface, which is what the face arcs separate.
					std::array<int32_t, 8> boundaryParent;
					for (int32_t i = 0; i < 8; ++i)
					{
						boundaryParent[i] = i;
					}
					std::function<int32_t(int32_t)> boundaryRoot = [&](int32_t node) -> int32_t
					{
						while (boundaryParent[node] != node)
						{
							boundaryParent[node] = boundaryParent[boundaryParent[node]];
							node = boundaryParent[node];
						}
						return node;
					};
					auto boundaryUnion = [&](int32_t p, int32_t q)
					{
						const int32_t rp = boundaryRoot(p);
						const int32_t rq = boundaryRoot(q);
						if (rp != rq) boundaryParent[rp] = rq;
					};
					for (int32_t i = 0; i < 8; ++i)
					{
						for (int32_t axis = 0; axis < 3; ++axis)
						{
							if ((i >> axis) & 1) continue;
							const int32_t j = i | (1 << axis);
							if ((q[i] < 0.0f) == (q[j] < 0.0f))
							{
								boundaryUnion(i, j);
							}
						}
					}
					for (int32_t face = 0; face < 6; ++face)
					{
						if (!faceIsAmbiguous[face]) continue;
						const int32_t c0n = Impl::TrilinearFaceCorners[face][0];
						const int32_t c1n = Impl::TrilinearFaceCorners[face][1];
						const int32_t c2n = Impl::TrilinearFaceCorners[face][2];
						const int32_t c3n = Impl::TrilinearFaceCorners[face][3];
						if (faceSaddleNegative[face])
						{
							if (q[c0n] < 0.0f) boundaryUnion(c0n, c2n);
							if (q[c1n] < 0.0f) boundaryUnion(c1n, c3n);
						}
						else
						{
							if (q[c0n] > 0.0f) boundaryUnion(c0n, c2n);
							if (q[c1n] > 0.0f) boundaryUnion(c1n, c3n);
						}
					}

					for (int32_t face = 0; face < 6; ++face)
					{
						std::array<uint32_t, 4> sideVertex;
						std::array<int32_t, 4> sideStartCorner;
						int32_t crossingCount = 0;
						for (int32_t side = 0; side < 4; ++side)
						{
							const int32_t cornerA = Impl::TrilinearFaceCorners[face][side];
							const int32_t cornerB = Impl::TrilinearFaceCorners[face][(side + 1) % 4];
							const int32_t edgeId = Impl::trilinearEdgeId(cornerA, cornerB);
							if (edgeMeshIndex[edgeId] >= 0)
							{
								sideVertex[crossingCount] = static_cast<uint32_t>(edgeMeshIndex[edgeId]);
								sideStartCorner[crossingCount] = cornerA;
								++crossingCount;
							}
						}

						if (crossingCount == 0)
						{
							continue;
						}
						POLYVOX_THROW_IF(crossingCount != 2 && crossingCount != 4, std::logic_error,
							"Trilinear extractor produced an unexpected number of face crossings");

						// Representative negative and positive boundary regions on this face.
						int32_t faceNegativeCorner = -1;
						int32_t facePositiveCorner = -1;
						for (int32_t side = 0; side < 4; ++side)
						{
							const int32_t corner = Impl::TrilinearFaceCorners[face][side];
							if (q[corner] < 0.0f) faceNegativeCorner = corner;
							else facePositiveCorner = corner;
						}
						const int32_t faceNegativeBasin = boundaryRoot(faceNegativeCorner);
						const int32_t facePositiveBasin = boundaryRoot(facePositiveCorner);

						if (crossingCount == 2)
						{
							arcs.push_back(Arc{ sideVertex[0], sideVertex[1], faceNegativeBasin, facePositiveBasin });
						}
						else
						{
							// Ambiguous face: two arcs, each wrapping one isolated corner.
							// The bilinear saddle carries the sign of the region connected
							// through the face centre, so the isolated islands are the two
							// corners of the opposite sign. A side leaves the corner at its
							// start, so the two sides meeting at an isolated corner bound
							// that corner's arc.
							const bool isolatedCornersNegative = !faceSaddleNegative[face];
							for (int32_t side = 0; side < 4; ++side)
							{
								const int32_t cornerA = Impl::TrilinearFaceCorners[face][side];
								if ((q[cornerA] < 0.0f) != isolatedCornersNegative) continue;

								const int32_t previousSide = (side + 3) % 4;
								int32_t entryHere = -1;
								int32_t entryPrevious = -1;
								for (int32_t k = 0; k < 4; ++k)
								{
									if (sideStartCorner[k] == Impl::TrilinearFaceCorners[face][side]) entryHere = k;
									if (sideStartCorner[k] == Impl::TrilinearFaceCorners[face][previousSide]) entryPrevious = k;
								}
								const int32_t arcNegativeBasin = isolatedCornersNegative ?
									boundaryRoot(cornerA) : faceNegativeBasin;
								const int32_t arcPositiveBasin = isolatedCornersNegative ?
									facePositiveBasin : boundaryRoot(cornerA);
								arcs.push_back(Arc{ sideVertex[entryPrevious], sideVertex[entryHere],
									arcNegativeBasin, arcPositiveBasin });
							}
						}
					}

					// Arcs meet at shared edge vertices; walk them into closed loops,
					// remembering which arcs bound each loop.
					std::unordered_map<uint32_t, std::array<uint32_t, 2> > loopAdjacency;
					std::unordered_map<uint32_t, std::array<size_t, 2> > arcAtVertex;
					for (size_t arcIndex = 0; arcIndex < arcs.size(); ++arcIndex)
					{
						auto registerEnd = [&](uint32_t node, uint32_t neighbour, size_t index)
						{
							// Stored as index + 1 so that vertex 0 does not collide with
							// the default-constructed empty slot.
							std::array<uint32_t, 2>& slots = loopAdjacency[node];
							std::array<size_t, 2>& arcSlots = arcAtVertex[node];
							if (slots[0] == 0)
							{
								slots[0] = neighbour + 1;
								arcSlots[0] = index + 1;
							}
							else
							{
								slots[1] = neighbour + 1;
								arcSlots[1] = index + 1;
							}
						};
						registerEnd(arcs[arcIndex].first, arcs[arcIndex].second, arcIndex);
						registerEnd(arcs[arcIndex].second, arcs[arcIndex].first, arcIndex);
					}

					struct LabelledLoop
					{
						std::vector<uint32_t> vertices;
						std::vector<size_t> arcIndices;
						std::set<int32_t> negativeBasins;
						std::set<int32_t> positiveBasins;
					};
					std::vector<LabelledLoop> labelledLoops;
					std::unordered_map<uint32_t, bool> loopVisited;
					for (const Arc& arcSeed : arcs)
					{
						if (loopVisited.find(arcSeed.first) != loopVisited.end()) continue;
						std::vector<uint32_t> loop;
						std::vector<size_t> loopArcIndices;
						const uint32_t start = arcSeed.first;
						uint32_t current = start;
						uint32_t previous = 0xFFFFFFFFu;
						do
						{
							loop.push_back(current);
							loopVisited[current] = true;
							const std::array<uint32_t, 2>& slots = loopAdjacency[current];
							const std::array<size_t, 2>& arcSlots = arcAtVertex[current];
							const uint32_t option0 = slots[0] - 1;
							const uint32_t option1 = slots[1] - 1;
							const size_t chosenArc = (option0 != previous) ? arcSlots[0] - 1 : arcSlots[1] - 1;
							loopArcIndices.push_back(chosenArc);
							const uint32_t next = (option0 != previous) ? option0 : option1;
							previous = current;
							current = next;
						} while (current != start);

						// A loop is adjacent to a boundary basin on a given sign side if
						// any of its arcs has that basin on that side. A loop can border
						// several basins on one side (e.g. one connected negative region
						// surrounding several positive pockets), so the full set is kept.
						LabelledLoop labelled{ loop, loopArcIndices, std::set<int32_t>(), std::set<int32_t>() };
						for (size_t arcIndex : loopArcIndices)
						{
							labelled.negativeBasins.insert(arcs[arcIndex].negativeBasin);
							labelled.positiveBasins.insert(arcs[arcIndex].positiveBasin);
						}
						labelledLoops.push_back(labelled);
					}

					// The loop graph has degree two at every node, so it decomposes into
					// disjoint cycles. A cell has twelve edges and the shortest loop uses
					// three, so there are between one and four loops.
					POLYVOX_THROW_IF(labelledLoops.empty() || labelledLoops.size() > 4, std::logic_error,
						"Trilinear extractor produced an unexpected number of boundary loops");

					// Two boundary loops belong to one connected sheet in two cases.
					//  1. An index 2 positive saddle joins the two positive regions
					//     reached uphill along its positive eigenvector.
					//  2. A tube: the loops border the same region on both sign sides,
					//     i.e. one sheet enters and leaves the cell through different
					//     faces without an interior critical point.
					// Loop indices are unioned; every resulting group of size >= 2 gets
					// a shared interior vertex, singleton groups are capped directly.
					std::vector<size_t> loopUnion(labelledLoops.size());
					for (size_t i = 0; i < loopUnion.size(); ++i) loopUnion[i] = i;
					std::function<size_t(size_t)> loopRoot = [&](size_t node) -> size_t
					{
						while (loopUnion[node] != node)
						{
							loopUnion[node] = loopUnion[loopUnion[node]];
							node = loopUnion[node];
						}
						return node;
					};
					auto joinLoops = [&](size_t a, size_t b)
					{
						const size_t ra = loopRoot(a);
						const size_t rb = loopRoot(b);
						if (ra != rb) loopUnion[ra] = rb;
					};

					auto sharesBasin = [](const std::set<int32_t>& a, const std::set<int32_t>& b) -> bool
					{
						for (int32_t basin : a)
						{
							if (b.count(basin) > 0) return true;
						}
						return false;
					};

					// Tube merges. When the same boundary region borders two loops on
					// both the negative and the positive side, one sheet enters and leaves
					// the cell through different faces without an interior critical point,
					// so the loops share an interior vertex.
					for (size_t i = 0; i < labelledLoops.size(); ++i)
					{
						for (size_t j = i + 1; j < labelledLoops.size(); ++j)
						{
							const bool sharePositive = sharesBasin(
								labelledLoops[i].positiveBasins, labelledLoops[j].positiveBasins);
							const bool shareNegative = sharesBasin(
								labelledLoops[i].negativeBasins, labelledLoops[j].negativeBasins);
							if (sharePositive && shareNegative)
							{
								joinLoops(i, j);
							}
						}
					}

					// Saddle merge: loops adjacent to either of the two joined positive
					// basins on the positive side.
					if (saddleMergeActive &&
						saddleMergeCorners[0] >= 0 && saddleMergeCorners[1] >= 0)
					{
						const int32_t saddleRootA = boundaryRoot(saddleMergeCorners[0]);
						const int32_t saddleRootB = boundaryRoot(saddleMergeCorners[1]);
						if (saddleRootA != saddleRootB)
						{
							std::vector<size_t> saddleLoops;
							for (size_t loopIndex = 0; loopIndex < labelledLoops.size(); ++loopIndex)
							{
								if (labelledLoops[loopIndex].positiveBasins.count(saddleRootA) > 0 ||
									labelledLoops[loopIndex].positiveBasins.count(saddleRootB) > 0)
								{
									saddleLoops.push_back(loopIndex);
								}
							}
							for (size_t k = 1; k < saddleLoops.size(); ++k)
							{
								joinLoops(saddleLoops[0], saddleLoops[k]);
							}
						}
					}

					std::vector<bool> loopHandled(labelledLoops.size(), false);

					struct RawTriangle
					{
						uint32_t indices[3];
					};
					std::vector<RawTriangle> rawTriangles;

					// Flip any triangle whose geometric normal points into the high
					// density side, so the winding always faces low density.
					auto orientTriangle = [&](RawTriangle triangle) -> RawTriangle
					{
						const Vertex<VoxelType>& v0 = result.getVertex(triangle.indices[0]);
						const Vertex<VoxelType>& v1 = result.getVertex(triangle.indices[1]);
						const Vertex<VoxelType>& v2 = result.getVertex(triangle.indices[2]);
						const Vector3DFloat geometricNormal = (v1.position - v0.position).cross(v2.position - v0.position);
						const float length = std::sqrt(geometricNormal.lengthSquared());
						const Vector3DFloat direction = geometricNormal * (1.0f / length);
						const Vector3DFloat centroid(
							(v0.position.getX() + v1.position.getX() + v2.position.getX()) / 3.0f,
							(v0.position.getY() + v1.position.getY() + v2.position.getY()) / 3.0f,
							(v0.position.getZ() + v1.position.getZ() + v2.position.getZ()) / 3.0f);
						const float probe = 1.0e-3f;
						const float baseX = centroid.getX() - static_cast<float>(cx - lx);
						const float baseY = centroid.getY() - static_cast<float>(cy - ly);
						const float baseZ = centroid.getZ() - static_cast<float>(cz - lz);
						const float fieldPlus = fieldAt(baseX + probe * direction.getX(),
							baseY + probe * direction.getY(), baseZ + probe * direction.getZ());
						const float fieldMinus = fieldAt(baseX - probe * direction.getX(),
							baseY - probe * direction.getY(), baseZ - probe * direction.getZ());
						if (fieldPlus > fieldMinus)
						{
							std::swap(triangle.indices[1], triangle.indices[2]);
						}
						return triangle;
					};

					auto nonDegenerate = [&](uint32_t i0, uint32_t i1, uint32_t i2) -> bool
					{
						const Vector3DFloat& p0 = result.getVertex(i0).position;
						const Vector3DFloat& p1 = result.getVertex(i1).position;
						const Vector3DFloat& p2 = result.getVertex(i2).position;
						return (p1 - p0).cross(p2 - p0).lengthSquared() > 1.0e-14f;
					};

					auto fanLoop = [&](const std::vector<uint32_t>& loop, int32_t apex) -> void
					{
						if (apex < 0)
						{
							for (size_t k = 1; k + 1 < loop.size(); ++k)
							{
								if (!nonDegenerate(loop[0], loop[k], loop[k + 1])) continue;
								rawTriangles.push_back(orientTriangle(RawTriangle{ { loop[0], loop[k], loop[k + 1] } }));
							}
						}
						else
						{
							const uint32_t apexIndex = static_cast<uint32_t>(apex);
							for (size_t k = 0; k < loop.size(); ++k)
							{
								const uint32_t next = loop[(k + 1) % loop.size()];
								if (!nonDegenerate(apexIndex, loop[k], next)) continue;
								rawTriangles.push_back(orientTriangle(RawTriangle{ { apexIndex, loop[k], next } }));
							}
						}
					};

					// Build the list of loop indices in each union group.
					std::map<size_t, std::vector<size_t> > loopGroups;
					for (size_t loopIndex = 0; loopIndex < labelledLoops.size(); ++loopIndex)
					{
						loopGroups[loopRoot(loopIndex)].push_back(loopIndex);
					}

					for (const auto& groupEntry : loopGroups)
					{
						const std::vector<size_t>& group = groupEntry.second;
						if (group.size() == 1)
						{
							// A single boundary loop caps its own surface sheet.
							fanLoop(labelledLoops[group[0]].vertices, -1);
							loopHandled[group[0]] = true;
							continue;
						}

						// Several loops bound one connected sheet. Place a vertex on the
						// iso surface inside the enclosed region and fan every loop to it.
						Vector3DFloat nodeSum(0.0f, 0.0f, 0.0f);
						size_t totalNodes = 0;
						for (size_t loopIndex : group)
						{
							for (uint32_t node : labelledLoops[loopIndex].vertices)
							{
								const Vector3DFloat& position = result.getVertex(node).position;
								nodeSum += Vector3DFloat(
									position.getX() - static_cast<float>(cx - lx),
									position.getY() - static_cast<float>(cy - ly),
									position.getZ() - static_cast<float>(cz - lz));
								++totalNodes;
							}
						}
						Vector3DFloat seedLocal = nodeSum * (1.0f / static_cast<float>(totalNodes));
						float seedValue = fieldAt(seedLocal.getX(), seedLocal.getY(), seedLocal.getZ());
						const bool groupNegative = seedValue < 0.0f;

						// Prefer the interior critical point when it lies in this region.
						if (hasInteriorCritical && (criticalValue < 0.0f) == groupNegative)
						{
							seedLocal = criticalLocal;
							seedValue = criticalValue;
						}

						bool placed = false;
						Vector3DFloat placedLocal(0.5f, 0.5f, 0.5f);
						Vector3DFloat interiorNormal(0.0f, 0.0f, 0.0f);

						for (int32_t targetCorner = 0; targetCorner < 8 && !placed; ++targetCorner)
						{
							if ((q[targetCorner] < 0.0f) == groupNegative) continue; // need opposite sign
							const Vector3DFloat targetLocal(
								static_cast<float>(Impl::TrilinearCornerOffset[targetCorner][0]),
								static_cast<float>(Impl::TrilinearCornerOffset[targetCorner][1]),
								static_cast<float>(Impl::TrilinearCornerOffset[targetCorner][2]));

							Vector3DFloat low = seedLocal;
							Vector3DFloat high = targetLocal;
							float lowValue = seedValue;
							const float highValue = fieldAt(high.getX(), high.getY(), high.getZ());
							if ((lowValue < 0.0f) == (highValue < 0.0f) || std::fabs(highValue) < degeneracy_tolerance)
							{
								continue;
							}

							Vector3DFloat candidate = low;
							for (int32_t iteration = 0; iteration < 40; ++iteration)
							{
								candidate = (low + high) * 0.5f;
								const float candidateValue = fieldAt(candidate.getX(), candidate.getY(), candidate.getZ());
								if ((candidateValue < 0.0f) == (lowValue < 0.0f))
								{
									low = candidate;
									lowValue = candidateValue;
								}
								else
								{
									high = candidate;
								}
							}

							Vector3DFloat normal = fieldGradientTowardsLow(candidate.getX(), candidate.getY(), candidate.getZ());
							if (!std::isfinite(normal.getX()) || !std::isfinite(normal.getY()) || !std::isfinite(normal.getZ()) ||
								normal.lengthSquared() <= 1.0e-10f)
							{
								continue;
							}

							const Vector3DFloat apexPosition(
								static_cast<float>(cx - lx) + candidate.getX(),
								static_cast<float>(cy - ly) + candidate.getY(),
								static_cast<float>(cz - lz) + candidate.getZ());
							bool geometryOk = true;
							for (size_t geometryLoopIdx : group)
							{
								const std::vector<uint32_t>& geometryLoop = labelledLoops[geometryLoopIdx].vertices;
								for (size_t k = 0; k < geometryLoop.size(); ++k)
								{
									const uint32_t idx0 = geometryLoop[k];
									const uint32_t idx1 = geometryLoop[(k + 1) % geometryLoop.size()];
									const Vector3DFloat& p0 = result.getVertex(idx0).position;
									const Vector3DFloat& p1 = result.getVertex(idx1).position;
									if ((p1 - apexPosition).cross(p0 - apexPosition).lengthSquared() <= 1.0e-14f)
									{
										geometryOk = false;
										break;
									}
								}
								if (!geometryOk) break;
							}
							if (!geometryOk) continue;

							placedLocal = candidate;
							interiorNormal = normal;
							placed = true;
						}

						if (!placed)
						{
							failDegenerate(cellPosition, "could not place a non degenerate interior vertex on the iso surface");
						}

						interiorNormal.normalise();

						Vertex<VoxelType> interiorVertex;
						interiorVertex.position = Vector3DFloat(
							static_cast<float>(cx - lx) + placedLocal.getX(),
							static_cast<float>(cy - ly) + placedLocal.getY(),
							static_cast<float>(cz - lz) + placedLocal.getZ());
						interiorVertex.normal = interiorNormal;
						// Reuse the material already computed for the sheet's boundary
						// vertices. This keeps material blending consistent for both plain
						// density volumes and material/density voxel types.
						const uint32_t materialSourceVertex = labelledLoops[group[0]].vertices[0];
						interiorVertex.data = result.getVertex(materialSourceVertex).data;
						const int32_t interiorIndex = static_cast<int32_t>(result.addVertex(interiorVertex));

						for (size_t loopIndex : group)
						{
							fanLoop(labelledLoops[loopIndex].vertices, interiorIndex);
							loopHandled[loopIndex] = true;
						}
					}

					// Safety net: cap any loop not consumed above.
					for (size_t loopIndex = 0; loopIndex < labelledLoops.size(); ++loopIndex)
					{
						if (loopHandled[loopIndex]) continue;
						fanLoop(labelledLoops[loopIndex].vertices, -1);
					}

					POLYVOX_THROW_IF(rawTriangles.empty(), std::logic_error,
						"Trilinear extractor generated no triangles for an occupied cell");

					for (const RawTriangle& triangle : rawTriangles)
					{
						result.addTriangle(triangle.indices[0], triangle.indices[1], triangle.indices[2]);
					}
				}
			}
		}

		result.setOffset(region.getLowerCorner());
		return result;
	}
}
