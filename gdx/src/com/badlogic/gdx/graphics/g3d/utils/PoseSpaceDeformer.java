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
import com.badlogic.gdx.math.Vector3;

/** Pose space deformation (corrective shapes) for skinned meshes.
 * <p>
 * The deformer interpolates sparse per-vertex corrective displacements from a set of joint poses using a quaternion pose
 * kernel and exact kernel interpolation:
 *
 * <pre>
 * r_b = bindRotation_b^-1 * localRotation_b
 * d(p, s) = sum_b weight_b * (1 - dot(r_b(p), r_b(s))^2)   (quaternion dots clamped to [-1, 1])
 * K(p, s) = exp(-beta * d(p, s))
 * delta(p) = k(p)^T * K^-1 * D
 * </pre>
 *
 * The common sample kernel matrix K is LU decomposed with partial pivoting once during {@link #prepare}; evaluation only
 * solves against the already factored matrix. The interpolation is the exact kernel solution: no kernel normalization,
 * no clamping of the (possibly negative) coefficients and no regularization.
 * <p>
 * The class is pure Java: it neither touches OpenGL/JNI nor pulls in any additional dependency. All kernel and linear
 * algebra work is done in double precision; only the produced positions are floats. A {@link Prepared} result is
 * immutable and owns private copies of every input, so it can be shared and evaluated against several
 * {@link ModelInstance}s, including interleaved evaluations.
 *
 * @author badlogic */
public class PoseSpaceDeformer {
	/** A pivoted elimination element smaller than this in absolute value makes the kernel matrix singular enough to
	 * reject. */
	private static final double PIVOT_EPSILON = 1e-10;
	/** Two poses whose squared distance is within this are treated as the same orientation. The distance is a squared
	 * quantity (1 - dot^2), so the threshold tolerates ordinary float round-off in quaternions built different ways. */
	private static final double ORIENTATION_EPSILON = 1e-9;
	/** Tolerance for accepting a quaternion as unit length. */
	private static final double UNIT_EPSILON = 1e-6;
	/** Relative tolerance for the structural checks on matrices. */
	private static final double MATRIX_EPSILON = 1e-6;
	private static final double WEIGHT_EPSILON = 1e-5;

	private PoseSpaceDeformer () {
	}

	/** A single corrective sample: the local rotations of every driver bone plus a sparse set of vertex displacements in
	 * bind mesh space. Vertices not listed in {@link #vertexIndices} have a zero delta. */
	public static class Sample {
		/** Unit local rotation of each driver bone, in the same order as the driver ids passed to {@link #prepare}. */
		public Quaternion[] rotations;
		/** Vertex indices touched by this sample; indices must not repeat within one sample. */
		public int[] vertexIndices;
		/** Three float displacement components per entry of {@link #vertexIndices}, xyz packed. */
		public float[] deltas;

		public Sample () {
		}

		public Sample (Quaternion[] rotations, int[] vertexIndices, float[] deltas) {
			this.rotations = rotations;
			this.vertexIndices = vertexIndices;
			this.deltas = deltas;
		}
	}

	/** The prepared, immutable result of {@link #prepare}. Owns private copies of all inputs and the LU decomposition of
	 * the sample kernel matrix. Safe to share between instances; evaluations never mutate it. */
	public static class Prepared {
		final int vertexCount;
		final int driverCount;
		final String[] driverIds;
		/** Bind rotations as unit quaternions, one {x,y,z,w} tuple per driver. */
		final double[][] bindRotations;
		final double[] driverWeights;
		final double beta;
		/** Per sample, per driver relative rotations r = bind^-1 * sample rotation, each a {x,y,z,w} tuple. */
		final double[][][] sampleRelativeRotations;
		/** Per sample packed per-vertex deltas (vertexCount * 3 entries), bind mesh space. */
		final double[][] sampleDeltas;
		/** LU decomposition (with partial pivoting) of the sample kernel matrix, row-major. */
		final double[][] lu;
		final int[] pivot;

		Prepared (int vertexCount, String[] driverIds, double[][] bindRotations, double[] driverWeights, double beta,
			double[][][] sampleRelativeRotations, double[][] sampleDeltas, double[][] lu, int[] pivot) {
			this.vertexCount = vertexCount;
			this.driverCount = driverIds.length;
			this.driverIds = driverIds;
			this.bindRotations = bindRotations;
			this.driverWeights = driverWeights;
			this.beta = beta;
			this.sampleRelativeRotations = sampleRelativeRotations;
			this.sampleDeltas = sampleDeltas;
			this.lu = lu;
			this.pivot = pivot;
		}

		/** Evaluates the corrective deformation followed by four-weight linear skinning.
		 * <p>
		 * The caller must already have run the animation mixing ({@code AnimationController.update} or a
		 * {@link BaseAnimationController} blend) and {@link ModelInstance#calculateTransforms()}. For every vertex the
		 * interpolated delta is first added to the bind position, the result is transformed by the part's
		 * {@link NodePart#bones} palette using the supplied four bone indices/weights, and finally multiplied by
		 * {@link ModelInstance#transform}. The mesh node's global transform is not applied again and the GPU mesh is not
		 * touched.
		 * @param instance the instance supplying driver node transforms, the bone palette and the world transform
		 * @param part a {@link NodePart} owned by {@code instance} with an up to date {@link NodePart#bones} palette
		 * @param bindPositions packed xyz bind positions, length {@code vertexCount * 3}; may alias {@code outWorld}
		 * @param boneIndices four palette indices per vertex, length {@code vertexCount * 4}
		 * @param boneWeights four non-negative weights per vertex summing to one, length {@code vertexCount * 4};
		 *           zero-weight slots still need legal indices
		 * @param outWorld receives the resulting world positions, same length as {@code bindPositions}; it is only
		 *           written after every input has been validated, so a failed call leaves its previous contents intact
		 * @throws IllegalArgumentException if any input is missing, malformed or not usable for this prepared result */
		public void evaluate (ModelInstance instance, NodePart part, float[] bindPositions, int[] boneIndices,
			float[] boneWeights, float[] outWorld) {
			// --- validation first, so a rejected call never leaves a partially written output ---
			if (instance == null) throw new IllegalArgumentException("ModelInstance must not be null.");
			if (part == null) throw new IllegalArgumentException("NodePart must not be null.");
			if (!partOwnedByInstance(instance, part))
				throw new IllegalArgumentException("NodePart does not belong to the given ModelInstance.");
			if (part.bones == null) throw new IllegalArgumentException("NodePart has no bone palette (bones is null).");
			final Matrix4[] palette = part.bones;
			for (int i = 0; i < palette.length; i++) {
				if (palette[i] == null)
					throw new IllegalArgumentException("Bone palette contains a null matrix at index " + i + ".");
				if (!isFinite(palette[i].val))
					throw new IllegalArgumentException("Bone palette matrix at index " + i + " is not finite.");
			}
			if (bindPositions == null || bindPositions.length != vertexCount * 3)
				throw new IllegalArgumentException("bindPositions must have length vertexCount * 3 (" + (vertexCount * 3) + ").");
			if (!isFinite(bindPositions)) throw new IllegalArgumentException("bindPositions contains a non-finite value.");
			if (outWorld == null || outWorld.length != bindPositions.length)
				throw new IllegalArgumentException("outWorld must have the same length as bindPositions.");
			if (boneIndices == null || boneIndices.length != vertexCount * 4)
				throw new IllegalArgumentException("boneIndices must have length vertexCount * 4 (" + (vertexCount * 4) + ").");
			if (boneWeights == null || boneWeights.length != vertexCount * 4)
				throw new IllegalArgumentException("boneWeights must have length vertexCount * 4 (" + (vertexCount * 4) + ").");
			for (int v = 0; v < vertexCount; v++) {
				double weightSum = 0.0;
				for (int s = 0; s < 4; s++) {
					final int slot = v * 4 + s;
					final int idx = boneIndices[slot];
					if (idx < 0 || idx >= palette.length)
						throw new IllegalArgumentException("Bone index " + idx + " at vertex " + v + " slot " + s
							+ " is outside the palette of size " + palette.length + ".");
					final float w = boneWeights[slot];
					if (!(w >= 0f))
						throw new IllegalArgumentException("Bone weight at vertex " + v + " slot " + s + " is negative or NaN.");
					weightSum += w;
				}
				if (Math.abs(weightSum - 1.0) > WEIGHT_EPSILON)
					throw new IllegalArgumentException("Bone weights of vertex " + v + " sum to " + weightSum + ", expected 1.");
			}
			final Matrix4 instanceTransform = instance.transform;
			if (instanceTransform == null) throw new IllegalArgumentException("ModelInstance transform must not be null.");
			if (!isFinite(instanceTransform.val))
				throw new IllegalArgumentException("ModelInstance transform is not finite.");
			if (!isAffine(instanceTransform.val))
				throw new IllegalArgumentException("ModelInstance transform must be an affine matrix.");
			if (determinantAffine3(instanceTransform.val) == 0.0)
				throw new IllegalArgumentException("ModelInstance transform is not invertible.");

			// --- driver rotations, read from the actual animated local matrices ---
			final double[][] currentRelative = new double[driverCount][4];
			final Quaternion localRotation = new Quaternion();
			for (int b = 0; b < driverCount; b++) {
				final Node node = instance.getNode(driverIds[b], true);
				if (node == null)
					throw new IllegalArgumentException("Driver bone '" + driverIds[b] + "' was not found in the ModelInstance.");
				final Matrix4 local = node.localTransform;
				if (!isFinite(local.val))
					throw new IllegalArgumentException("Local transform of driver '" + driverIds[b] + "' is not finite.");
				if (!isRotationWithPositiveUniformScale(local.val))
					throw new IllegalArgumentException("Local transform of driver '" + driverIds[b]
						+ "' must be a rotation with positive uniform scale and translation, without shear or reflection.");
				local.getRotation(localRotation, true);
				final double[] q = {localRotation.x, localRotation.y, localRotation.z, localRotation.w};
				if (!isFinite(q) || Math.abs(dot(q, q) - 1.0) > UNIT_EPSILON)
					throw new IllegalArgumentException(
						"Rotation extracted from driver '" + driverIds[b] + "' is not a unit quaternion.");
				multiplyConjugate(bindRotations[b], q, currentRelative[b]);
			}

			// --- kernel against every sample and one solve against the prepared LU factors ---
			final int sampleCount = sampleRelativeRotations.length;
			final double[] kernel = new double[sampleCount];
			for (int i = 0; i < sampleCount; i++)
				kernel[i] = Math.exp(-beta * poseDistance(currentRelative, sampleRelativeRotations[i], driverWeights));
			final double[] coefficients = solve(lu, pivot, kernel);

			// --- interpolate deltas, then skin, all into a private buffer before touching outWorld ---
			final float[] result = new float[vertexCount * 3];
			final Vector3 corrected = new Vector3();
			final Vector3 posed = new Vector3();
			final Vector3 skinned = new Vector3();
			for (int v = 0; v < vertexCount; v++) {
				final int base = v * 3;
				double dx = 0.0, dy = 0.0, dz = 0.0;
				for (int i = 0; i < sampleCount; i++) {
					final double c = coefficients[i];
					if (c == 0.0) continue;
					final double[] d = sampleDeltas[i];
					dx += c * d[base];
					dy += c * d[base + 1];
					dz += c * d[base + 2];
				}
				corrected.set(bindPositions[base] + (float)dx, bindPositions[base + 1] + (float)dy,
					bindPositions[base + 2] + (float)dz);

				skinned.set(0f, 0f, 0f);
				for (int s = 0; s < 4; s++) {
					final float w = boneWeights[v * 4 + s];
					if (w == 0f) continue;
					final Matrix4 bone = palette[boneIndices[v * 4 + s]];
					posed.set(corrected).mul(bone);
					skinned.mulAdd(posed, w);
				}
				skinned.mul(instanceTransform);
				result[base] = skinned.x;
				result[base + 1] = skinned.y;
				result[base + 2] = skinned.z;
			}
			System.arraycopy(result, 0, outWorld, 0, result.length);
		}
	}

	/** Builds the immutable prepared deformer.
	 * @param vertexCount positive number of vertices of the part
	 * @param driverIds non-empty, unique ids of the driver bones, in driver order
	 * @param bindRotations unit local bind rotation of each driver, in driver order
	 * @param driverWeights positive per-driver kernel weights, in driver order
	 * @param beta positive kernel width parameter
	 * @param samples corrective samples; must contain exactly one neutral sample equivalent to the bind pose with zero
	 *           deltas, and no two samples may describe equivalent poses (a quaternion sign flip is the same pose)
	 * @return the prepared deformer with private copies of the data and the decomposed kernel matrix
	 * @throws IllegalArgumentException if any argument is malformed, a pose is duplicated or the kernel matrix is
	 *            numerically singular (a pivoted elimination pivot below 1e-10) */
	public static Prepared prepare (int vertexCount, String[] driverIds, Quaternion[] bindRotations, double[] driverWeights,
		double beta, Sample[] samples) {
		if (vertexCount <= 0) throw new IllegalArgumentException("vertexCount must be greater than zero.");
		if (driverIds == null || driverIds.length == 0)
			throw new IllegalArgumentException("At least one driver bone is required.");
		final int driverCount = driverIds.length;
		for (int b = 0; b < driverCount; b++) {
			if (driverIds[b] == null) throw new IllegalArgumentException("Driver id at index " + b + " is null.");
			for (int j = 0; j < b; j++) {
				if (driverIds[b].equals(driverIds[j]))
					throw new IllegalArgumentException("Duplicate driver id: " + driverIds[b]);
			}
		}
		if (bindRotations == null || bindRotations.length != driverCount)
			throw new IllegalArgumentException("bindRotations must contain one unit quaternion per driver.");
		final double[][] bind = new double[driverCount][];
		for (int b = 0; b < driverCount; b++) {
			if (bindRotations[b] == null) throw new IllegalArgumentException("bindRotation at index " + b + " is null.");
			bind[b] = toUnitQuaternion(bindRotations[b], "bindRotation[" + b + "]");
		}
		if (driverWeights == null || driverWeights.length != driverCount)
			throw new IllegalArgumentException("driverWeights must contain one positive weight per driver.");
		final double[] weights = new double[driverCount];
		for (int b = 0; b < driverCount; b++) {
			if (Double.isNaN(driverWeights[b]) || Double.isInfinite(driverWeights[b]) || driverWeights[b] <= 0.0)
				throw new IllegalArgumentException("driverWeights[" + b + "] must be finite and positive.");
			weights[b] = driverWeights[b];
		}
		if (Double.isNaN(beta) || Double.isInfinite(beta) || beta <= 0.0)
			throw new IllegalArgumentException("beta must be finite and positive.");
		if (samples == null || samples.length == 0)
			throw new IllegalArgumentException("At least the neutral sample is required.");
		final int sampleCount = samples.length;

		final double[][][] relative = new double[sampleCount][driverCount][4];
		final double[][] deltas = new double[sampleCount][vertexCount * 3];
		int neutralIndex = -1;
		for (int i = 0; i < sampleCount; i++) {
			final Sample sample = samples[i];
			if (sample == null) throw new IllegalArgumentException("Sample at index " + i + " is null.");
			if (sample.rotations == null || sample.rotations.length != driverCount)
				throw new IllegalArgumentException("Sample " + i + " must provide one rotation per driver.");
			if (sample.vertexIndices == null)
				throw new IllegalArgumentException("Sample " + i + " vertexIndices must not be null.");
			if (sample.deltas == null || sample.deltas.length != sample.vertexIndices.length * 3)
				throw new IllegalArgumentException("Sample " + i + " deltas must contain three components per vertex index.");

			boolean neutral = true;
			for (int b = 0; b < driverCount; b++) {
				if (sample.rotations[b] == null)
					throw new IllegalArgumentException("Sample " + i + " rotation for driver " + b + " is null.");
				final double[] q = toUnitQuaternion(sample.rotations[b], "sample " + i + " rotation[" + b + "]");
				multiplyConjugate(bind[b], q, relative[i][b]);
				// Relative to the bind pose, a neutral orientation is identity up to quaternion sign.
				if (1.0 - relative[i][b][3] * relative[i][b][3] > ORIENTATION_EPSILON) neutral = false;
			}
			for (int k = 0; k < sample.vertexIndices.length; k++) {
				final int vertexIndex = sample.vertexIndices[k];
				if (vertexIndex < 0 || vertexIndex >= vertexCount)
					throw new IllegalArgumentException("Sample " + i + " references vertex " + vertexIndex
						+ " outside [0," + vertexCount + ").");
				for (int previous = 0; previous < k; previous++) {
					if (sample.vertexIndices[previous] == vertexIndex)
						throw new IllegalArgumentException("Sample " + i + " lists vertex " + vertexIndex + " more than once.");
				}
				for (int c = 0; c < 3; c++) {
					final float delta = sample.deltas[k * 3 + c];
					if (Float.isNaN(delta) || Float.isInfinite(delta))
						throw new IllegalArgumentException("Sample " + i + " contains a non-finite delta.");
					deltas[i][vertexIndex * 3 + c] = delta;
					if (delta != 0f) neutral = false;
				}
			}
			if (neutral) {
				if (neutralIndex >= 0)
					throw new IllegalArgumentException(
						"More than one neutral (bind equivalent, zero-delta) sample was provided.");
				neutralIndex = i;
			}
		}
		if (neutralIndex < 0)
			throw new IllegalArgumentException("No neutral sample equivalent to the bind pose with zero deltas was provided.");

		// Equivalent poses, including opposite quaternion signs, are duplicates and must not enter the kernel matrix.
		for (int i = 0; i < sampleCount; i++) {
			for (int j = i + 1; j < sampleCount; j++) {
				if (poseDistance(relative[i], relative[j], weights) <= ORIENTATION_EPSILON)
					throw new IllegalArgumentException("Samples " + i + " and " + j
						+ " describe equivalent poses; duplicate poses are not allowed.");
			}
		}

		// Build the common kernel matrix and factor it once, here in the prepare phase.
		final double[][] kernel = new double[sampleCount][sampleCount];
		for (int i = 0; i < sampleCount; i++) {
			kernel[i][i] = 1.0;
			for (int j = i + 1; j < sampleCount; j++) {
				final double value = Math.exp(-beta * poseDistance(relative[i], relative[j], weights));
				kernel[i][j] = value;
				kernel[j][i] = value;
			}
		}
		final int[] pivot = new int[sampleCount];
		decomposeLU(kernel, pivot);

		final String[] driverIdsCopy = new String[driverCount];
		System.arraycopy(driverIds, 0, driverIdsCopy, 0, driverCount);
		final double[] weightsCopy = new double[driverCount];
		System.arraycopy(weights, 0, weightsCopy, 0, driverCount);
		return new Prepared(vertexCount, driverIdsCopy, bind, weightsCopy, beta, relative, deltas, kernel, pivot);
	}

	/** Squared pose distance between two sets of per-driver relative unit quaternions. */
	private static double poseDistance (double[][] a, double[][] b, double[] weights) {
		double distance = 0.0;
		for (int bone = 0; bone < a.length; bone++) {
			final double d = clamp(dot(a[bone], b[bone]), -1.0, 1.0);
			distance += weights[bone] * (1.0 - d * d);
		}
		return distance;
	}

	/** In-place LU decomposition with partial pivoting. Throws on a pivot whose absolute value is below
	 * {@link #PIVOT_EPSILON}; no regularization is added and no sample is silently dropped. */
	private static void decomposeLU (double[][] matrix, int[] pivot) {
		final int n = matrix.length;
		for (int k = 0; k < n; k++) {
			int pivotRow = k;
			double pivotValue = Math.abs(matrix[k][k]);
			for (int i = k + 1; i < n; i++) {
				final double candidate = Math.abs(matrix[i][k]);
				if (candidate > pivotValue) {
					pivotValue = candidate;
					pivotRow = i;
				}
			}
			if (pivotValue < PIVOT_EPSILON)
				throw new IllegalArgumentException("Kernel matrix is numerically singular: pivoted element " + pivotValue
					+ " is below " + PIVOT_EPSILON + ". Remove an equivalent or nearly duplicate pose instead of regularizing.");
			if (pivotRow != k) {
				final double[] tmp = matrix[k];
				matrix[k] = matrix[pivotRow];
				matrix[pivotRow] = tmp;
			}
			pivot[k] = pivotRow;
			final double pivotInverse = 1.0 / matrix[k][k];
			for (int i = k + 1; i < n; i++) {
				matrix[i][k] *= pivotInverse;
				final double factor = matrix[i][k];
				for (int j = k + 1; j < n; j++)
					matrix[i][j] -= factor * matrix[k][j];
			}
		}
	}

	/** Solves LU x = rhs for the factors produced by {@link #decomposeLU}. The factors are only read. */
	private static double[] solve (double[][] lu, int[] pivot, double[] rhs) {
		final int n = lu.length;
		final double[] y = new double[n];
		for (int i = 0; i < n; i++) {
			double value = rhs[pivot[i]];
			for (int j = 0; j < i; j++)
				value -= lu[i][j] * y[j];
			y[i] = value;
		}
		final double[] x = new double[n];
		for (int i = n - 1; i >= 0; i--) {
			double value = y[i];
			for (int j = i + 1; j < n; j++)
				value -= lu[i][j] * x[j];
			x[i] = value / lu[i][i];
		}
		return x;
	}

	/** Stores out = a^-1 * b for unit quaternions, i.e. conjugate(a) * b. */
	private static void multiplyConjugate (double[] a, double[] b, double[] out) {
		final double ax = -a[0], ay = -a[1], az = -a[2], aw = a[3];
		final double bx = b[0], by = b[1], bz = b[2], bw = b[3];
		out[0] = aw * bx + ax * bw + ay * bz - az * by;
		out[1] = aw * by + ay * bw + az * bx - ax * bz;
		out[2] = aw * bz + az * bw + ax * by - ay * bx;
		out[3] = aw * bw - ax * bx - ay * by - az * bz;
	}

	private static double dot (double[] a, double[] b) {
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	}

	private static double clamp (double value, double min, double max) {
		return value < min ? min : (value > max ? max : value);
	}

	private static double[] toUnitQuaternion (Quaternion q, String description) {
		final double[] values = {q.x, q.y, q.z, q.w};
		if (!isFinite(values)) throw new IllegalArgumentException(description + " is not finite.");
		final double lengthSquared = dot(values, values);
		if (Math.abs(lengthSquared - 1.0) > UNIT_EPSILON)
			throw new IllegalArgumentException(description + " is not a unit quaternion (len^2=" + lengthSquared + ").");
		return values;
	}

	private static boolean isFinite (double[] values) {
		for (double value : values)
			if (Double.isNaN(value) || Double.isInfinite(value)) return false;
		return true;
	}

	private static boolean isFinite (float[] values) {
		for (float value : values)
			if (Float.isNaN(value) || Float.isInfinite(value)) return false;
		return true;
	}

	private static boolean isAffine (float[] m) {
		return m[Matrix4.M30] == 0f && m[Matrix4.M31] == 0f && m[Matrix4.M32] == 0f && m[Matrix4.M33] == 1f;
	}

	/** Determinant of an affine matrix, which equals the determinant of its upper left 3x3 block. */
	private static double determinantAffine3 (float[] m) {
		final double a00 = m[Matrix4.M00], a01 = m[Matrix4.M01], a02 = m[Matrix4.M02];
		final double a10 = m[Matrix4.M10], a11 = m[Matrix4.M11], a12 = m[Matrix4.M12];
		final double a20 = m[Matrix4.M20], a21 = m[Matrix4.M21], a22 = m[Matrix4.M22];
		return a00 * (a11 * a22 - a12 * a21) + a01 * (a12 * a20 - a10 * a22) + a02 * (a10 * a21 - a11 * a20);
	}

	/** Accepts only rotation combined with positive uniform scale (and arbitrary translation): the three columns must
	 * be mutually orthogonal, share one positive length, and the 3x3 determinant must be positive (no reflection). */
	private static boolean isRotationWithPositiveUniformScale (float[] m) {
		if (!isAffine(m)) return false;
		final double c0x = m[Matrix4.M00], c0y = m[Matrix4.M10], c0z = m[Matrix4.M20];
		final double c1x = m[Matrix4.M01], c1y = m[Matrix4.M11], c1z = m[Matrix4.M21];
		final double c2x = m[Matrix4.M02], c2y = m[Matrix4.M12], c2z = m[Matrix4.M22];
		final double l0 = Math.sqrt(c0x * c0x + c0y * c0y + c0z * c0z);
		final double l1 = Math.sqrt(c1x * c1x + c1y * c1y + c1z * c1z);
		final double l2 = Math.sqrt(c2x * c2x + c2y * c2y + c2z * c2z);
		if (!(l0 > 0.0 && l1 > 0.0 && l2 > 0.0)) return false;
		final double maxScale = Math.max(l0, Math.max(l1, l2));
		if (Math.abs(l0 - l1) > MATRIX_EPSILON * maxScale || Math.abs(l1 - l2) > MATRIX_EPSILON * maxScale)
			return false;
		if (Math.abs(c0x * c1x + c0y * c1y + c0z * c1z) > MATRIX_EPSILON * l0 * l1) return false;
		if (Math.abs(c0x * c2x + c0y * c2y + c0z * c2z) > MATRIX_EPSILON * l0 * l2) return false;
		if (Math.abs(c1x * c2x + c1y * c2y + c1z * c2z) > MATRIX_EPSILON * l1 * l2) return false;
		return determinantAffine3(m) > 0.0;
	}

	private static boolean partOwnedByInstance (ModelInstance instance, NodePart part) {
		for (Node node : instance.nodes)
			if (partOwnedByNode(node, part)) return true;
		return false;
	}

	private static boolean partOwnedByNode (Node node, NodePart part) {
		for (NodePart candidate : node.parts)
			if (candidate == part) return true;
		for (int i = 0; i < node.getChildCount(); i++)
			if (partOwnedByNode(node.getChild(i), part)) return true;
		return false;
	}
}
