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
#include "xatlas_multipose.h"
#include <cmath>
#include <cstddef>
#include <vector>

namespace xatlas {
namespace multipose {

namespace {

struct Row
{
	uint32_t count;
	uint32_t vars[6];
	double coef[6];
	double lockedSum;
};

inline bool isFiniteNumber(double v)
{
	return v == v && v <= 1.7976931348623157e308 && v >= -1.7976931348623157e308;
}

inline const float *PoseVertex(uint32_t pose, uint32_t vertex, uint32_t extraPoseCount, const float *basePositions, const float *extraPositions)
{
	if (pose == 0)
		return &basePositions[vertex * 3];
	return &extraPositions[(vertex * extraPoseCount + (pose - 1)) * 3];
}

} // namespace

bool SolveChart(uint32_t vertexCount, const uint32_t *indices, uint32_t indexCount,
	const float *basePositions, const float *extraPositions, uint32_t extraPoseCount,
	const float *extraWeights,
	uint32_t pinVertex0, uint32_t pinVertex1,
	const float pinUv0[2], const float pinUv1[2],
	float *uvOut)
{
	const uint32_t poseCount = extraPoseCount + 1;
	if (vertexCount == 0 || indices == nullptr || indexCount == 0 || (indexCount % 3) != 0 ||
		basePositions == nullptr || (extraPoseCount > 0 && (extraPositions == nullptr || extraWeights == nullptr)) ||
		uvOut == nullptr || pinVertex0 >= vertexCount || pinVertex1 >= vertexCount)
		return false;
	const uint32_t faceCount = indexCount / 3;
	const uint32_t variableCount = vertexCount * 2;

	// Validate the whole input before assembling anything. This guarantees a
	// failed solve never publishes a partial parameterization.
	double weights[64];
	double *dynamicWeights = nullptr;
	double *weightPtr = weights;
	if (poseCount > 64) {
		dynamicWeights = new double[poseCount];
		weightPtr = dynamicWeights;
	}
	weightPtr[0] = 1.0;
	for (uint32_t p = 1; p < poseCount; p++) {
		weightPtr[p] = double(extraWeights[p - 1]);
		if (!isFiniteNumber(weightPtr[p]) || weightPtr[p] <= 0.0) {
			delete[] dynamicWeights;
			return false;
		}
	}
	for (uint32_t i = 0; i < indexCount; i++) {
		if (indices[i] >= vertexCount) {
			delete[] dynamicWeights;
			return false;
		}
	}
	for (uint32_t p = 0; p < poseCount; p++) {
		for (uint32_t v = 0; v < vertexCount; v++) {
			const float *pos = PoseVertex(p, v, extraPoseCount, basePositions, extraPositions);
			if (!isFiniteNumber(pos[0]) || !isFiniteNumber(pos[1]) || !isFiniteNumber(pos[2])) {
				delete[] dynamicWeights;
				return false;
			}
		}
		for (uint32_t f = 0; f < faceCount; f++) {
			const float *p0 = PoseVertex(p, indices[f * 3 + 0], extraPoseCount, basePositions, extraPositions);
			const float *p1 = PoseVertex(p, indices[f * 3 + 1], extraPoseCount, basePositions, extraPositions);
			const float *p2 = PoseVertex(p, indices[f * 3 + 2], extraPoseCount, basePositions, extraPositions);
			const double e0 = double(p1[0]) - p0[0];
			const double e1 = double(p1[1]) - p0[1];
			const double e2 = double(p1[2]) - p0[2];
			const double f0 = double(p2[0]) - p0[0];
			const double f1 = double(p2[1]) - p0[1];
			const double f2 = double(p2[2]) - p0[2];
			const double cx = e1 * f2 - e2 * f1;
			const double cy = e2 * f0 - e0 * f2;
			const double cz = e0 * f1 - e1 * f0;
			const double twiceArea = std::sqrt(cx * cx + cy * cy + cz * cz);
			if (!isFiniteNumber(twiceArea) || twiceArea <= 0.0) {
				delete[] dynamicWeights;
				return false;
			}
		}
	}

	// The two pinned vertices lock UV translation, rotation and scale. Their
	// four real coordinates are the only locked variables.
	bool locked[4] = { true, true, true, true };
	double lockedValue[4] = { pinUv0[0], pinUv0[1], pinUv1[0], pinUv1[1] };
	if (pinVertex0 == pinVertex1) {
		locked[2] = locked[3] = false;
		lockedValue[2] = lockedValue[3] = 0.0;
	}

	std::vector<uint32_t> variableToFree(variableCount);
	uint32_t freeCount = 0;
	for (uint32_t v = 0; v < vertexCount; v++) {
		for (uint32_t c = 0; c < 2; c++) {
			const uint32_t variable = v * 2 + c;
			bool isLocked = false;
			if (v == pinVertex0)
				isLocked = locked[c];
			else if (v == pinVertex1)
				isLocked = locked[2 + c];
			variableToFree[variable] = isLocked ? UINT32_MAX : freeCount++;
		}
	}

	std::vector<Row> rows;
	rows.reserve(size_t(faceCount) * poseCount * 2);
	for (uint32_t p = 0; p < poseCount; p++) {
		const double weight = weightPtr[p];
		for (uint32_t f = 0; f < faceCount; f++) {
			const uint32_t i0 = indices[f * 3 + 0];
			const uint32_t i1 = indices[f * 3 + 1];
			const uint32_t i2 = indices[f * 3 + 2];
			const float *p0 = PoseVertex(p, i0, extraPoseCount, basePositions, extraPositions);
			const float *p1 = PoseVertex(p, i1, extraPoseCount, basePositions, extraPositions);
			const float *p2 = PoseVertex(p, i2, extraPoseCount, basePositions, extraPositions);
			double e[3] = { double(p1[0]) - p0[0], double(p1[1]) - p0[1], double(p1[2]) - p0[2] };
			const double edgeLength = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
			const double invEdgeLength = 1.0 / edgeLength;
			e[0] *= invEdgeLength;
			e[1] *= invEdgeLength;
			e[2] *= invEdgeLength;
			double rel[3] = { double(p2[0]) - p0[0], double(p2[1]) - p0[1], double(p2[2]) - p0[2] };
			const double b = rel[0] * e[0] + rel[1] * e[1] + rel[2] * e[2];
			double perp[3] = { rel[0] - b * e[0], rel[1] - b * e[1], rel[2] - b * e[2] };
			const double c = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
			const double cx = double(p1[1] - p0[1]) * double(p2[2] - p0[2]) - double(p1[2] - p0[2]) * double(p2[1] - p0[1]);
			const double cy = double(p1[2] - p0[2]) * double(p2[0] - p0[0]) - double(p1[0] - p0[0]) * double(p2[2] - p0[2]);
			const double cz = double(p1[0] - p0[0]) * double(p2[1] - p0[1]) - double(p1[1] - p0[1]) * double(p2[0] - p0[0]);
			const double twiceArea = std::sqrt(cx * cx + cy * cy + cz * cz);
			// Row scaling so that the assembled normal equations reproduce the
			// objective term weight * |R|^2 / (2 * area).
			const double rowScale = std::sqrt(weight / twiceArea);
			const double a = edgeLength;
			const uint32_t faceVerts[3] = { i0, i1, i2 };
			// Real and imaginary coefficients of
			// z1 * (U2 - U0) - z2 * (U1 - U0), with z1 = a and z2 = b + ic.
			const double realCoef[6] = { b - a, -c, -b, c, a, 0.0 };
			const double imagCoef[6] = { c, b - a, -c, -b, 0.0, a };
			for (uint32_t part = 0; part < 2; part++) {
				const double *coef = part == 0 ? realCoef : imagCoef;
				Row row;
				row.count = 0;
				row.lockedSum = 0.0;
				for (uint32_t k = 0; k < 3; k++) {
					for (uint32_t comp = 0; comp < 2; comp++) {
						const uint32_t vertex = faceVerts[k];
						const double value = coef[k * 2 + comp] * rowScale;
						if (value == 0.0)
							continue;
						const uint32_t free = variableToFree[vertex * 2 + comp];
						if (free == UINT32_MAX) {
							const uint32_t lockIndex = vertex == pinVertex0 ? comp : 2 + comp;
							row.lockedSum += value * lockedValue[lockIndex];
						} else {
							row.vars[row.count] = free;
							row.coef[row.count] = value;
							row.count++;
						}
					}
				}
				if (row.count > 0)
					rows.push_back(row);
			}
		}
	}
	delete[] dynamicWeights;

	std::vector<double> solution(freeCount, 0.0);
	std::vector<double> initial(freeCount, 0.0);
	for (uint32_t v = 0; v < vertexCount; v++) {
		for (uint32_t c = 0; c < 2; c++) {
			const uint32_t free = variableToFree[v * 2 + c];
			if (free != UINT32_MAX)
				initial[free] = uvOut[v * 2 + c];
		}
	}

	// Assemble the normal equations A_f^T A_f x = -A_f^T b, where b is the
	// contribution of the locked variables.
	std::vector<double> rhs(freeCount, 0.0);
	std::vector<double> diag(freeCount, 0.0);
	for (size_t r = 0; r < rows.size(); r++) {
		const Row &row = rows[r];
		for (uint32_t i = 0; i < row.count; i++) {
			rhs[row.vars[i]] -= row.coef[i] * row.lockedSum;
			diag[row.vars[i]] += row.coef[i] * row.coef[i];
		}
	}

	bool solved = freeCount == 0;

	// Direct dense solve when the system is small enough. This keeps the
	// result very close to a dense QR least-squares solution. Large systems use
	// a sparse conjugate gradient solve.
	const uint32_t kDenseLimit = 512;
	if (!solved && freeCount <= kDenseLimit) {
		std::vector<double> matrix(size_t(freeCount) * freeCount, 0.0);
		double maxDiag = 0.0;
		for (uint32_t i = 0; i < freeCount; i++)
			maxDiag = diag[i] > maxDiag ? diag[i] : maxDiag;
		for (size_t r = 0; r < rows.size(); r++) {
			const Row &row = rows[r];
			for (uint32_t i = 0; i < row.count; i++) {
				const size_t rowBase = size_t(row.vars[i]) * freeCount;
				for (uint32_t j = 0; j < row.count; j++)
					matrix[rowBase + row.vars[j]] += row.coef[i] * row.coef[j];
			}
		}
		// Cholesky factorization. The pinned vertices make the system positive
		// definite for connected charts.
		std::vector<double> L(size_t(freeCount) * freeCount, 0.0);
		bool positiveDefinite = maxDiag > 0.0;
		const double pivotTolerance = maxDiag * 1e-14;
		for (uint32_t i = 0; positiveDefinite && i < freeCount; i++) {
			for (uint32_t j = 0; j <= i; j++) {
				double sum = matrix[size_t(i) * freeCount + j];
				for (uint32_t k = 0; k < j; k++)
					sum -= L[size_t(i) * freeCount + k] * L[size_t(j) * freeCount + k];
				if (i == j) {
					if (!(sum > pivotTolerance)) {
						positiveDefinite = false;
						break;
					}
					L[size_t(i) * freeCount + j] = std::sqrt(sum);
				} else {
					L[size_t(i) * freeCount + j] = sum / L[size_t(j) * freeCount + j];
				}
			}
		}
		if (positiveDefinite) {
			std::vector<double> y(freeCount, 0.0);
			for (uint32_t i = 0; i < freeCount; i++) {
				double sum = rhs[i];
				for (uint32_t k = 0; k < i; k++)
					sum -= L[size_t(i) * freeCount + k] * y[k];
				y[i] = sum / L[size_t(i) * freeCount + i];
			}
			for (int i = int(freeCount) - 1; i >= 0; i--) {
				double sum = y[i];
				for (uint32_t k = uint32_t(i) + 1; k < freeCount; k++)
					sum -= L[size_t(k) * freeCount + i] * solution[k];
				solution[i] = sum / L[size_t(i) * freeCount + i];
			}
			solved = true;
		}
	}

	if (!solved && freeCount > 0) {
		// Jacobi-preconditioned conjugate gradient on the normal equations.
		std::vector<double> x = initial;
		std::vector<double> residual(freeCount, 0.0);
		std::vector<double> preconditioned(freeCount, 0.0);
		std::vector<double> direction(freeCount, 0.0);
		std::vector<double> Ap(freeCount, 0.0);
		std::vector<double> Ax(freeCount, 0.0);
		double rhsNorm2 = 0.0;
		for (uint32_t i = 0; i < freeCount; i++)
			rhsNorm2 += rhs[i] * rhs[i];
		const double target = rhsNorm2 * 1e-24; // Relative residual 1e-12.
		double residualNorm2 = 0.0;
		for (size_t r = 0; r < rows.size(); r++) {
			const Row &row = rows[r];
			double s = row.lockedSum;
			for (uint32_t i = 0; i < row.count; i++)
				s += row.coef[i] * x[row.vars[i]];
			for (uint32_t i = 0; i < row.count; i++)
				Ax[row.vars[i]] += row.coef[i] * s;
		}
		for (uint32_t i = 0; i < freeCount; i++) {
			residual[i] = rhs[i] - Ax[i];
			residualNorm2 += residual[i] * residual[i];
		}
		bool converged = residualNorm2 <= target;
		double rz = 0.0;
		if (!converged) {
			for (uint32_t i = 0; i < freeCount; i++) {
				const double inv = diag[i] > 0.0 ? 1.0 / diag[i] : 0.0;
				preconditioned[i] = residual[i] * inv;
				direction[i] = preconditioned[i];
				rz += residual[i] * preconditioned[i];
			}
		}
		const uint32_t maxIterations = 2000 + freeCount * 10;
		uint32_t iteration = 0;
		while (!converged && iteration < maxIterations) {
			for (uint32_t i = 0; i < freeCount; i++)
				Ap[i] = 0.0;
			for (size_t r = 0; r < rows.size(); r++) {
				const Row &row = rows[r];
				double s = 0.0;
				for (uint32_t i = 0; i < row.count; i++)
					s += row.coef[i] * direction[row.vars[i]];
				for (uint32_t i = 0; i < row.count; i++)
					Ap[row.vars[i]] += row.coef[i] * s;
			}
			double dAp = 0.0;
			for (uint32_t i = 0; i < freeCount; i++)
				dAp += direction[i] * Ap[i];
			if (!(dAp > 0.0)) {
				converged = residualNorm2 <= target;
				break;
			}
			const double alpha = rz / dAp;
			for (uint32_t i = 0; i < freeCount; i++) {
				x[i] += alpha * direction[i];
				residual[i] -= alpha * Ap[i];
			}
			residualNorm2 = 0.0;
			for (uint32_t i = 0; i < freeCount; i++)
				residualNorm2 += residual[i] * residual[i];
			if (residualNorm2 <= target) {
				converged = true;
				break;
			}
			double rzNew = 0.0;
			for (uint32_t i = 0; i < freeCount; i++) {
				const double inv = diag[i] > 0.0 ? 1.0 / diag[i] : 0.0;
				preconditioned[i] = residual[i] * inv;
				rzNew += residual[i] * preconditioned[i];
			}
			const double beta = rzNew / rz;
			rz = rzNew;
			for (uint32_t i = 0; i < freeCount; i++)
				direction[i] = preconditioned[i] + beta * direction[i];
			iteration++;
		}
		if (converged) {
			solution = x;
			solved = true;
		}
	}

	if (!solved)
		return false;

	// Publish the solution only after a complete, successful solve.
	for (uint32_t v = 0; v < vertexCount; v++) {
		for (uint32_t c = 0; c < 2; c++) {
			const uint32_t variable = v * 2 + c;
			const uint32_t free = variableToFree[variable];
			if (free != UINT32_MAX) {
				uvOut[variable] = float(solution[free]);
			} else if (v == pinVertex0) {
				uvOut[variable] = pinUv0[c];
			} else if (v == pinVertex1) {
				uvOut[variable] = pinUv1[c];
			}
		}
	}
	return true;
}

} // namespace multipose
} // namespace xatlas

