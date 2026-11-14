/*******************************************************************************
 * Copyright 2011 See AUTHORS file.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/

package com.badlogic.gdx.graphics.g3d.utils;

import com.badlogic.gdx.graphics.g3d.ModelInstance;
import com.badlogic.gdx.graphics.g3d.model.Node;
import com.badlogic.gdx.graphics.g3d.model.NodePart;
import com.badlogic.gdx.math.Matrix4;
import com.badlogic.gdx.math.Quaternion;

/** Pose-space corrective deformation for skinned {@link NodePart}s.
 * <p>
 * A set of artist authored poses (sparse per-vertex displacements for a given set of driver bone rotations) is interpolated with
 * a Gaussian kernel over a quaternion pose distance. The interpolation solves the full kernel system, so extrapolation and
 * negative coefficients are preserved; it is not a normalized weighted average.
 * <p>
 * Usage: call {@link #prepare(int, String[], Quaternion[], double[], double, Sample[])} once with the corrective poses, then call
 * {@link Prepared#evaluate(ModelInstance, NodePart, float[], int[], float[], float[])} after the animation has been applied and
 * {@link ModelInstance#calculateTransforms()} has been called. All corrections are added in bind-mesh space, before the normal
 * four-weight linear blend skinning with the {@link NodePart#bones} palette and before the instance world transform.
 * <p>
 * This class is pure Java and does not depend on OpenGL or any native library. It does not modify animations, nodes, palettes or
 * meshes. A single {@link Prepared} instance may be evaluated for multiple model instances, including interleaved evaluations. */
public final class PoseSpaceDeformer {
	/** Maximum acceptable absolute pivot during the LU decomposition of the kernel matrix. */
	private static final double PIVOT_EPSILON = 1e-10;
	/** Maximum deviation from unit length accepted for input rotations. */
	private static final double UNIT_EPSILON = 1e-4;
	/** Two poses are considered equivalent when every driver rotation agrees up to sign within this tolerance. A sample pose is
	 * separated by at most about 0.16 degrees, which is far below any authored distinction. */
	private static final double EQUIVALENT_EPSILON = 1e-6;
	/** Relative tolerance used while validating driver local matrices (uniform scale, orthogonality, determinant). */
	private static final double MATRIX_TOLERANCE = 1e-3;

	/** A single corrective pose.
	 * <p>
	 * {@link #rotations} contains the local rotations of each driver bone in the same order as the driver ids passed to
	 * {@link #prepare(int, String[], Quaternion[], double[], double, Sample[])}. {@link #vertexIndices} lists the vertices that
	 * this pose moves, and {@link #deltas} contains the corresponding displacements in bind-mesh space, three floats per vertex in
	 * the same order as {@link #vertexIndices}. Vertices that are not listed get a zero displacement. */
	public static final class Sample {
		/** Local rotation per driver bone. */
		public Quaternion[] rotations;
		/** Vertex indices receiving a non-zero displacement, in ascending order is not required. */
		public int[] vertexIndices;
		/** Displacements in bind-mesh space: three values per entry of {@link #vertexIndices}. */
		public float[] deltas;

		public Sample () {
		}

		public Sample (final Quaternion[] rotations, final int[] vertexIndices, final float[] deltas) {
			this.rotations = rotations;
			this.vertexIndices = vertexIndices;
			this.deltas = deltas;
		}
	}

	/** Immutable prepared corrective deformation. Build it with
	 * {@link PoseSpaceDeformer#prepare(int, String[], Quaternion[], double[], double, Sample[])}. */
	public static final class Prepared {
		private final int vertexCount;
		private final int driverCount;
		private final int sampleCount;
		private final String[] driverIds;
		private final double[] driverWeights;
		private final double beta;
		/** Unit bind rotation per driver, stored as x, y, z, w in double precision. */
		private final double[][] bindRotations;
		/** Sample rotations relative to the bind rotations: [sample][driver][x, y, z, w]. */
		private final double[][][] sampleRotations;
		/** Sparse vertex indices per sample, copied from the input. */
		private final int[][] sampleVertexIndices;
		/** Sparse displacements per sample, copied and converted to double. */
		private final double[][] sampleDeltas;
		/** LU decomposition of the kernel matrix, row-major, without the implicit unit diagonal. */
		private final double[] lu;
		/** Row permutation produced by partial pivoting. */
		private final int[] pivots;

		private Prepared (int vertexCount, int driverCount, int sampleCount, String[] driverIds, double[] driverWeights,
			double beta, double[][] bindRotations, double[][][] sampleRotations, int[][] sampleVertexIndices, double[][] sampleDeltas,
			double[] lu, int[] pivots) {
			this.vertexCount = vertexCount;
			this.driverCount = driverCount;
			this.sampleCount = sampleCount;
			this.driverIds = driverIds;
			this.driverWeights = driverWeights;
			this.beta = beta;
			this.bindRotations = bindRotations;
			this.sampleRotations = sampleRotations;
			this.sampleVertexIndices = sampleVertexIndices;
			this.sampleDeltas = sampleDeltas;
			this.lu = lu;
			this.pivots = pivots;
		}

		/** Evaluates the corrective deformation and writes world-space positions to {@code outWorld}.
		 * <p>
		 * On entry the animation has already been applied and {@link ModelInstance#calculateTransforms()} has been called, so
		 * {@link NodePart#bones} contains the current skinning palette. Driver rotations are read from each driver node's
		 * {@link Node#localTransform}, never from {@link Node#rotation}.
		 * @param instance the model instance that owns {@code part}
		 * @param part the skinned node part; its {@link NodePart#bones} palette must be up to date
		 * @param bindPositions bind-mesh positions, three floats per vertex
		 * @param boneIndices four indices into {@code part.bones} per vertex, length {@code vertexCount * 4}
		 * @param boneWeights four skinning weights per vertex, length {@code vertexCount * 4}, non-negative and summing to one
		 * @param outWorld output world-space positions, three floats per vertex; may be the same array as {@code bindPositions}
		 * @throws IllegalArgumentException if any argument is invalid or the current pose or palette cannot be used */
		public void evaluate (final ModelInstance instance, final NodePart part, final float[] bindPositions,
			final int[] boneIndices, final float[] boneWeights, final float[] outWorld) {
			if (instance == null) throw new IllegalArgumentException("instance cannot be null");
			if (part == null) throw new IllegalArgumentException("part cannot be null");
			if (bindPositions == null) throw new IllegalArgumentException("bindPositions cannot be null");
			if (boneIndices == null) throw new IllegalArgumentException("boneIndices cannot be null");
			if (boneWeights == null) throw new IllegalArgumentException("boneWeights cannot be null");
			if (outWorld == null) throw new IllegalArgumentException("outWorld cannot be null");
			if (bindPositions.length != vertexCount * 3)
				throw new IllegalArgumentException("bindPositions length must be vertexCount * 3");
			if (outWorld.length != bindPositions.length)
				throw new IllegalArgumentException("outWorld length must equal bindPositions length");
			if (boneIndices.length != vertexCount * 4)
				throw new IllegalArgumentException("boneIndices length must be vertexCount * 4");
			if (boneWeights.length != vertexCount * 4)
				throw new IllegalArgumentException("boneWeights length must be vertexCount * 4");
			if (!ownsPart(instance, part)) throw new IllegalArgumentException("part does not belong to instance");

			final Matrix4[] palette = part.bones;
			if (palette == null || palette.length == 0) throw new IllegalArgumentException("part has no bone palette");
			for (int i = 0; i < palette.length; i++) {
				if (palette[i] == null) throw new IllegalArgumentException("part bone palette contains a null matrix");
				if (!isFinite(palette[i])) throw new IllegalArgumentException("part bone palette contains a non-finite matrix");
			}

			for (int i = 0; i < vertexCount; i++) {
				double sum = 0;
				for (int j = 0; j < 4; j++) {
					final int bone = boneIndices[i * 4 + j];
					if (bone < 0 || bone >= palette.length)
						throw new IllegalArgumentException("bone index out of range at vertex " + i);
					final float weight = boneWeights[i * 4 + j];
					if (!(weight >= 0f) || Float.isInfinite(weight))
						throw new IllegalArgumentException("invalid bone weight at vertex " + i);
					sum += weight;
				}
				if (Math.abs(sum - 1.0) > 1e-4) throw new IllegalArgumentException("bone weights must sum to one at vertex " + i);
			}

			if (!isFinite(instance.transform)) throw new IllegalArgumentException("instance transform is not finite");
			if (!isAffine(instance.transform)) throw new IllegalArgumentException("instance transform is not affine");
			final double worldDet = determinant3x3(instance.transform);
			if (Double.isInfinite(worldDet) || Double.isNaN(worldDet) || worldDet == 0.0)
				throw new IllegalArgumentException("instance transform is singular");

			// Read the driver rotations from the actual animated local matrices and build the pose descriptor.
			final double[][] currentRotations = new double[driverCount][];
			for (int d = 0; d < driverCount; d++) {
				final Node node = instance.getNode(driverIds[d], true);
				if (node == null) throw new IllegalArgumentException("missing driver node: " + driverIds[d]);
				final double[] rotation = rotationFromLocalMatrix(node.localTransform, driverIds[d]);
				currentRotations[d] = multiply(conjugate(bindRotations[d]), rotation);
			}

			// k(q) and the interpolation coefficients c = K^-1 k. The kernel matrix was factorized during preparation.
			final double[] k = new double[sampleCount];
			for (int s = 0; s < sampleCount; s++)
				k[s] = kernel(poseDistance(currentRotations, sampleRotations[s]));
			final double[] coefficients = solve(k);

			// Accumulate the sparse corrective displacement for every vertex in bind-mesh space.
			final double[] displacement = new double[vertexCount * 3];
			for (int s = 0; s < sampleCount; s++) {
				final double c = coefficients[s];
				final int[] indices = sampleVertexIndices[s];
				final double[] deltas = sampleDeltas[s];
				for (int e = 0, n = indices.length; e < n; e++) {
					final int base = indices[e] * 3;
					displacement[base] += c * deltas[e * 3];
					displacement[base + 1] += c * deltas[e * 3 + 1];
					displacement[base + 2] += c * deltas[e * 3 + 2];
				}
			}

			// Skin in bind-mesh space, then apply the instance world transform. Everything is computed into a temporary buffer so
			// that a failure never leaves partially updated output.
			final float[] result = new float[vertexCount * 3];
			final float[] wv = instance.transform.val;
			final double[] pd = new double[3];
			for (int v = 0; v < vertexCount; v++) {
				final int b = v * 3;
				final double x = (double)bindPositions[b] + displacement[b];
				final double y = (double)bindPositions[b + 1] + displacement[b + 1];
				final double z = (double)bindPositions[b + 2] + displacement[b + 2];

				double mx = 0, my = 0, mz = 0;
				for (int j = 0; j < 4; j++) {
					final float weight = boneWeights[v * 4 + j];
					if (weight == 0f) continue;
					final float[] m = palette[boneIndices[v * 4 + j]].val;
					final double w = weight;
					mx += w * (m[Matrix4.M00] * x + m[Matrix4.M01] * y + m[Matrix4.M02] * z + m[Matrix4.M03]);
					my += w * (m[Matrix4.M10] * x + m[Matrix4.M11] * y + m[Matrix4.M12] * z + m[Matrix4.M13]);
					mz += w * (m[Matrix4.M20] * x + m[Matrix4.M21] * y + m[Matrix4.M22] * z + m[Matrix4.M23]);
				}

				pd[0] = wv[Matrix4.M00] * mx + wv[Matrix4.M01] * my + wv[Matrix4.M02] * mz + wv[Matrix4.M03];
				pd[1] = wv[Matrix4.M10] * mx + wv[Matrix4.M11] * my + wv[Matrix4.M12] * mz + wv[Matrix4.M13];
				pd[2] = wv[Matrix4.M20] * mx + wv[Matrix4.M21] * my + wv[Matrix4.M22] * mz + wv[Matrix4.M23];
				result[b] = (float)pd[0];
				result[b + 1] = (float)pd[1];
				result[b + 2] = (float)pd[2];
			}
			System.arraycopy(result, 0, outWorld, 0, result.length);
		}

		private double kernel (double distance) {
			return Math.exp(-beta * distance);
		}

		private double poseDistance (double[][] a, double[][] b) {
			double distance = 0;
			for (int d = 0; d < driverCount; d++) {
				double dot = a[d][0] * b[d][0] + a[d][1] * b[d][1] + a[d][2] * b[d][2] + a[d][3] * b[d][3];
				if (dot > 1.0) dot = 1.0;
				else if (dot < -1.0) dot = -1.0;
				distance += driverWeights[d] * (1.0 - dot * dot);
			}
			return distance;
		}

		/** Solves K * x = b using the LU decomposition computed by {@link #prepare}. */
		private double[] solve (double[] b) {
			final double[] x = new double[sampleCount];
			for (int i = 0; i < sampleCount; i++) x[i] = b[pivots[i]];
			for (int i = 0; i < sampleCount; i++) {
				double sum = x[i];
				final int row = i * sampleCount;
				for (int j = 0; j < i; j++) sum -= lu[row + j] * x[j];
				x[i] = sum;
			}
			for (int i = sampleCount - 1; i >= 0; i--) {
				double sum = x[i];
				final int row = i * sampleCount;
				for (int j = i + 1; j < sampleCount; j++) sum -= lu[row + j] * x[j];
				x[i] = sum / lu[row + i];
			}
			return x;
		}
	}

	/** Prepares a corrective deformation from artist authored samples.
	 * <p>
	 * The inputs are copied; later changes to the arrays or quaternions supplied here do not affect the returned object.
	 * @param vertexCount number of vertices in the bind mesh, must be positive
	 * @param driverIds node ids of the driver bones, in the order used by {@link Sample#rotations}
	 * @param bindRotations unit local rotation of each driver in the bind pose
	 * @param driverWeights positive weight of each driver in the pose distance
	 * @param beta positive kernel width parameter
	 * @param samples corrective poses; at least one neutral pose (equivalent to the bind rotations, all deltas zero) is required
	 * @return a prepared deformation that can be evaluated repeatedly
	 * @throws IllegalArgumentException if the inputs are inconsistent, degenerate or contain equivalent duplicate poses */
	public static Prepared prepare (int vertexCount, String[] driverIds, Quaternion[] bindRotations, double[] driverWeights,
		double beta, Sample[] samples) {
		if (vertexCount <= 0) throw new IllegalArgumentException("vertexCount must be positive");
		if (driverIds == null || driverIds.length == 0) throw new IllegalArgumentException("driverIds must not be empty");
		final int driverCount = driverIds.length;
		if (bindRotations == null || bindRotations.length != driverCount)
			throw new IllegalArgumentException("bindRotations length must match driverIds");
		if (driverWeights == null || driverWeights.length != driverCount)
			throw new IllegalArgumentException("driverWeights length must match driverIds");
		if (samples == null || samples.length == 0) throw new IllegalArgumentException("samples must not be empty");
		if (Double.isNaN(beta) || Double.isInfinite(beta) || !(beta > 0))
			throw new IllegalArgumentException("beta must be positive and finite");

		final String[] ids = new String[driverCount];
		for (int d = 0; d < driverCount; d++) {
			final String id = driverIds[d];
			if (id == null || id.length() == 0) throw new IllegalArgumentException("driver id must not be null or empty");
			for (int j = 0; j < d; j++)
				if (ids[j].equals(id)) throw new IllegalArgumentException("duplicate driver id: " + id);
			ids[d] = id;
		}

		final double[] weights = new double[driverCount];
		for (int d = 0; d < driverCount; d++) {
			final double weight = driverWeights[d];
			if (Double.isNaN(weight) || Double.isInfinite(weight) || !(weight > 0))
				throw new IllegalArgumentException("driver weight must be positive and finite");
			weights[d] = weight;
		}

		final double[][] binds = new double[driverCount][];
		for (int d = 0; d < driverCount; d++) {
			binds[d] = unitQuaternion(bindRotations[d], "bind rotation " + d);
		}

		final int sampleCount = samples.length;
		final double[][][] rotations = new double[sampleCount][][];
		final int[][] indices = new int[sampleCount][];
		final double[][] deltas = new double[sampleCount][];
		boolean hasNeutral = false;

		for (int s = 0; s < sampleCount; s++) {
			final Sample sample = samples[s];
			if (sample == null) throw new IllegalArgumentException("sample " + s + " is null");
			if (sample.rotations == null || sample.rotations.length != driverCount)
				throw new IllegalArgumentException("sample " + s + " must provide one rotation per driver");

			final double[][] relative = new double[driverCount][];
			for (int d = 0; d < driverCount; d++) {
				final double[] local = unitQuaternion(sample.rotations[d], "sample " + s + " rotation " + d);
				relative[d] = multiply(conjugate(binds[d]), local);
			}
			rotations[s] = relative;

			final int[] sampleIndices = sample.vertexIndices == null ? new int[0] : sample.vertexIndices;
			final float[] sampleDeltas = sample.deltas == null ? new float[0] : sample.deltas;
			if (sampleDeltas.length != sampleIndices.length * 3)
				throw new IllegalArgumentException("sample " + s + " deltas length must be vertexIndices length * 3");
			final int[] copiedIndices = new int[sampleIndices.length];
			final double[] copiedDeltas = new double[sampleDeltas.length];
			boolean allZero = true;
			for (int e = 0; e < sampleIndices.length; e++) {
				final int vertex = sampleIndices[e];
				if (vertex < 0 || vertex >= vertexCount)
					throw new IllegalArgumentException("sample " + s + " vertex index out of range: " + vertex);
				for (int j = 0; j < e; j++)
					if (copiedIndices[j] == vertex)
						throw new IllegalArgumentException("sample " + s + " contains duplicate vertex index " + vertex);
				copiedIndices[e] = vertex;
				for (int k = 0; k < 3; k++) {
					final float value = sampleDeltas[e * 3 + k];
					if (Float.isNaN(value) || Float.isInfinite(value))
						throw new IllegalArgumentException("sample " + s + " contains a non-finite delta");
					copiedDeltas[e * 3 + k] = value;
					if (value != 0f) allZero = false;
				}
			}
			indices[s] = copiedIndices;
			deltas[s] = copiedDeltas;

			if (allZero && isNeutral(relative)) hasNeutral = true;
		}

		if (!hasNeutral)
			throw new IllegalArgumentException("a neutral sample (bind rotations, zero deltas) is required");

		for (int i = 0; i < sampleCount; i++)
			for (int j = i + 1; j < sampleCount; j++)
				if (equivalent(rotations[i], rotations[j]))
					throw new IllegalArgumentException("samples " + i + " and " + j + " describe equivalent poses");

		// Build and factor the shared kernel matrix. Partial pivoting is used and a tiny pivot is an explicit error.
		final double[] lu = new double[sampleCount * sampleCount];
		for (int i = 0; i < sampleCount; i++) {
			for (int j = 0; j < sampleCount; j++)
				lu[i * sampleCount + j] = Math.exp(-beta * poseDistance(rotations[i], rotations[j], weights, driverCount));
		}
		final int[] pivots = new int[sampleCount];
		for (int i = 0; i < sampleCount; i++) pivots[i] = i;
		for (int k = 0; k < sampleCount; k++) {
			int pivot = k;
			double best = Math.abs(lu[k * sampleCount + k]);
			for (int i = k + 1; i < sampleCount; i++) {
				final double candidate = Math.abs(lu[i * sampleCount + k]);
				if (candidate > best) {
					best = candidate;
					pivot = i;
				}
			}
			if (best < PIVOT_EPSILON)
				throw new IllegalArgumentException("kernel matrix is singular or ill-conditioned (pivot " + best + ")");
			if (pivot != k) {
				final int rowK = k * sampleCount, rowP = pivot * sampleCount;
				for (int j = 0; j < sampleCount; j++) {
					final double tmp = lu[rowK + j];
					lu[rowK + j] = lu[rowP + j];
					lu[rowP + j] = tmp;
				}
				final int tmp = pivots[k];
				pivots[k] = pivots[pivot];
				pivots[pivot] = tmp;
			}
			final double diagonal = lu[k * sampleCount + k];
			for (int i = k + 1; i < sampleCount; i++) {
				final double factor = lu[i * sampleCount + k] / diagonal;
				lu[i * sampleCount + k] = factor;
				for (int j = k + 1; j < sampleCount; j++)
					lu[i * sampleCount + j] -= factor * lu[k * sampleCount + j];
			}
		}

		return new Prepared(vertexCount, driverCount, sampleCount, ids, weights, beta, binds, rotations, indices, deltas, lu,
			pivots);
	}

	private static double poseDistance (double[][] a, double[][] b, double[] weights, int driverCount) {
		double distance = 0;
		for (int d = 0; d < driverCount; d++) {
			double dot = a[d][0] * b[d][0] + a[d][1] * b[d][1] + a[d][2] * b[d][2] + a[d][3] * b[d][3];
			if (dot > 1.0) dot = 1.0;
			else if (dot < -1.0) dot = -1.0;
			distance += weights[d] * (1.0 - dot * dot);
		}
		return distance;
	}

	private static boolean isNeutral (double[][] relative) {
		for (int d = 0; d < relative.length; d++)
			if (1.0 - Math.abs(relative[d][3]) > EQUIVALENT_EPSILON) return false;
		return true;
	}

	private static boolean equivalent (double[][] a, double[][] b) {
		for (int d = 0; d < a.length; d++) {
			final double dot = a[d][0] * b[d][0] + a[d][1] * b[d][1] + a[d][2] * b[d][2] + a[d][3] * b[d][3];
			if (1.0 - Math.abs(dot) > EQUIVALENT_EPSILON) return false;
		}
		return true;
	}

	private static double[] unitQuaternion (Quaternion q, String what) {
		if (q == null) throw new IllegalArgumentException(what + " is null");
		final double x = q.x, y = q.y, z = q.z, w = q.w;
		if (!isFinite(x) || !isFinite(y) || !isFinite(z) || !isFinite(w))
			throw new IllegalArgumentException(what + " is not finite");
		final double length = Math.sqrt(x * x + y * y + z * z + w * w);
		if (Math.abs(length - 1.0) > UNIT_EPSILON)
			throw new IllegalArgumentException(what + " is not a unit quaternion");
		return new double[] {x / length, y / length, z / length, w / length};
	}

	private static double[] conjugate (double[] q) {
		return new double[] {-q[0], -q[1], -q[2], q[3]};
	}

	/** Multiplies two quaternions in the form a * b. */
	private static double[] multiply (double[] a, double[] b) {
		return new double[] {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
			a[3] * b[1] + a[1] * b[3] + a[2] * b[0] - a[0] * b[2],
			a[3] * b[2] + a[2] * b[3] + a[0] * b[1] - a[1] * b[0],
			a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
	}

	/** Extracts the rotation from a driver local matrix, requiring a finite, positive, uniform scale plus rotation and
	 * translation (no shear, reflection or non-uniform scale). */
	private static double[] rotationFromLocalMatrix (Matrix4 matrix, String id) {
		final float[] v = matrix.val;
		for (int i = 0; i < 16; i++)
			if (!isFinite(v[i])) throw new IllegalArgumentException("driver " + id + " local matrix is not finite");
		if (Math.abs(v[Matrix4.M30]) > MATRIX_TOLERANCE || Math.abs(v[Matrix4.M31]) > MATRIX_TOLERANCE
			|| Math.abs(v[Matrix4.M32]) > MATRIX_TOLERANCE || Math.abs(v[Matrix4.M33] - 1f) > MATRIX_TOLERANCE)
			throw new IllegalArgumentException("driver " + id + " local matrix is not affine");

		// The matrix is column major: each column of the upper-left 3x3 is a scaled rotation axis.
		double ax = v[Matrix4.M00], ay = v[Matrix4.M10], az = v[Matrix4.M20];
		double bx = v[Matrix4.M01], by = v[Matrix4.M11], bz = v[Matrix4.M21];
		double cx = v[Matrix4.M02], cy = v[Matrix4.M12], cz = v[Matrix4.M22];
		final double s0 = Math.sqrt(ax * ax + ay * ay + az * az);
		final double s1 = Math.sqrt(bx * bx + by * by + bz * bz);
		final double s2 = Math.sqrt(cx * cx + cy * cy + cz * cz);
		if (!(s0 > 0) || !(s1 > 0) || !(s2 > 0))
			throw new IllegalArgumentException("driver " + id + " local matrix is singular");
		final double max = Math.max(s0, Math.max(s1, s2));
		final double min = Math.min(s0, Math.min(s1, s2));
		if (max - min > MATRIX_TOLERANCE * max)
			throw new IllegalArgumentException("driver " + id + " local matrix has non-uniform scale");

		ax /= s0;
		ay /= s0;
		az /= s0;
		bx /= s1;
		by /= s1;
		bz /= s1;
		cx /= s2;
		cy /= s2;
		cz /= s2;
		if (Math.abs(ax * bx + ay * by + az * bz) > MATRIX_TOLERANCE
			|| Math.abs(ax * cx + ay * cy + az * cz) > MATRIX_TOLERANCE
			|| Math.abs(bx * cx + by * cy + bz * cz) > MATRIX_TOLERANCE)
			throw new IllegalArgumentException("driver " + id + " local matrix contains shear");
		final double determinant = ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx);
		if (!(determinant > 0))
			throw new IllegalArgumentException("driver " + id + " local matrix contains a reflection");
		if (Math.abs(determinant - 1.0) > MATRIX_TOLERANCE)
			throw new IllegalArgumentException("driver " + id + " local matrix is not a pure rotation with uniform scale");

		// Elements of the orthonormal rotation matrix (row i, column j).
		final double r00 = ax, r01 = bx, r02 = cx;
		final double r10 = ay, r11 = by, r12 = cy;
		final double r20 = az, r21 = bz, r22 = cz;
		final double trace = r00 + r11 + r22;
		final double[] q = new double[4];
		if (trace > 0) {
			final double s = Math.sqrt(trace + 1.0) * 2.0;
			q[3] = 0.25 * s;
			q[0] = (r21 - r12) / s;
			q[1] = (r02 - r20) / s;
			q[2] = (r10 - r01) / s;
		} else if (r00 > r11 && r00 > r22) {
			final double s = Math.sqrt(1.0 + r00 - r11 - r22) * 2.0;
			q[3] = (r21 - r12) / s;
			q[0] = 0.25 * s;
			q[1] = (r01 + r10) / s;
			q[2] = (r02 + r20) / s;
		} else if (r11 > r22) {
			final double s = Math.sqrt(1.0 + r11 - r00 - r22) * 2.0;
			q[3] = (r02 - r20) / s;
			q[0] = (r01 + r10) / s;
			q[1] = 0.25 * s;
			q[2] = (r12 + r21) / s;
		} else {
			final double s = Math.sqrt(1.0 + r22 - r00 - r11) * 2.0;
			q[3] = (r10 - r01) / s;
			q[0] = (r02 + r20) / s;
			q[1] = (r12 + r21) / s;
			q[2] = 0.25 * s;
		}
		final double length = Math.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		return new double[] {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
	}

	private static boolean ownsPart (ModelInstance instance, NodePart part) {
		for (int i = 0; i < instance.nodes.size; i++)
			if (ownsPart(instance.nodes.get(i), part)) return true;
		return false;
	}

	private static boolean ownsPart (Node node, NodePart part) {
		for (int i = 0; i < node.parts.size; i++)
			if (node.parts.get(i) == part) return true;
		for (int i = 0; i < node.getChildCount(); i++)
			if (ownsPart(node.getChild(i), part)) return true;
		return false;
	}

	private static boolean isFinite (Matrix4 matrix) {
		for (int i = 0; i < 16; i++)
			if (!isFinite(matrix.val[i])) return false;
		return true;
	}

	/** @return whether the bottom row of the matrix is (0, 0, 0, 1) within a small tolerance. */
	private static boolean isAffine (Matrix4 matrix) {
		final float[] v = matrix.val;
		return Math.abs(v[Matrix4.M30]) <= 1e-5f && Math.abs(v[Matrix4.M31]) <= 1e-5f && Math.abs(v[Matrix4.M32]) <= 1e-5f
			&& Math.abs(v[Matrix4.M33] - 1f) <= 1e-5f;
	}

	private static boolean isFinite (double value) {
		return !Double.isNaN(value) && !Double.isInfinite(value);
	}

	private static double determinant3x3 (Matrix4 matrix) {
		final float[] v = matrix.val;
		final double m00 = v[Matrix4.M00], m01 = v[Matrix4.M01], m02 = v[Matrix4.M02];
		final double m10 = v[Matrix4.M10], m11 = v[Matrix4.M11], m12 = v[Matrix4.M12];
		final double m20 = v[Matrix4.M20], m21 = v[Matrix4.M21], m22 = v[Matrix4.M22];
		return m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) + m02 * (m10 * m21 - m11 * m20);
	}
}
