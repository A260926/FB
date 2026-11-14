import com.badlogic.gdx.graphics.g3d.Material;
import com.badlogic.gdx.graphics.g3d.Model;
import com.badlogic.gdx.graphics.g3d.ModelInstance;
import com.badlogic.gdx.graphics.g3d.model.Animation;
import com.badlogic.gdx.graphics.g3d.model.MeshPart;
import com.badlogic.gdx.graphics.g3d.model.Node;
import com.badlogic.gdx.graphics.g3d.model.NodeAnimation;
import com.badlogic.gdx.graphics.g3d.model.NodeKeyframe;
import com.badlogic.gdx.graphics.g3d.model.NodePart;
import com.badlogic.gdx.graphics.g3d.utils.AnimationController;
import com.badlogic.gdx.graphics.g3d.utils.PoseSpaceDeformer;
import com.badlogic.gdx.math.Matrix4;
import com.badlogic.gdx.math.Quaternion;
import com.badlogic.gdx.math.Vector3;
import com.badlogic.gdx.utils.Array;
import com.badlogic.gdx.utils.ArrayMap;

/** Headless verification for {@link PoseSpaceDeformer}. */
public class Target {
	static final int VERTEX_COUNT = 5;
	static final int DRIVER_COUNT = 2;
	static final float TOLERANCE = 2e-4f;

	static final Quaternion BIND_UPPER = axis(0, 0, 1, 20);
	static final Quaternion BIND_FORE = axis(1, 0, 0, -30);
	static final double[] DRIVER_WEIGHTS = {1.0, 0.7};
	static final double BETA = 0.8;

	// Absolute local rotations of the driver bones, one entry per sample, in the driver order (upper, fore).
	static final Quaternion[][] SAMPLE_ROTATIONS = {
		{BIND_UPPER, BIND_FORE}, // neutral
		{axis(0, 0, 1, 70), BIND_FORE},
		{BIND_UPPER, axis(1, 0, 0, 20)},
		{axis(0, 0, 1, 65), axis(1, 0, 0, 15)},
	};
	static final int[][] SAMPLE_VERTICES = {{}, {0}, {0, 1}, {1}};
	static final float[][] SAMPLE_DELTAS = {{}, {0.35f, 0.10f, -0.05f}, {-0.20f, 0.15f, 0.08f, 0.25f, -0.12f, 0.04f},
		{0.40f, 0.20f, 0.10f}};

	static final float[] BIND_POSITIONS = {1.2f, 0.0f, 0.0f, 1.8f, 0.1f, 0.0f, 2.4f, 0.0f, 0.1f, 0.5f, 0.3f, -0.2f, 0.0f,
		-0.2f, 0.4f};
	static final int[] BONE_INDICES = {0, 1, 2, 3, 1, 0, 2, 3, 2, 1, 3, 0, 3, 0, 1, 2, 0, 3, 1, 2};
	static final float[] BONE_WEIGHTS = {0.6f, 0.25f, 0.1f, 0.05f, 0.55f, 0.3f, 0.1f, 0.05f, 0.7f, 0.2f, 0.06f, 0.04f, 0.5f,
		0.3f, 0.15f, 0.05f, 0.6f, 0.2f, 0.1f, 0.1f};

	static class Rig extends AnimationController {
		Rig (ModelInstance instance) {
			super(instance);
		}

		void sample (Animation animation, float time) {
			applyAnimation(animation, time);
		}

		void blend (Animation a, float ta, Animation b, float tb, float weight) {
			applyAnimations(a, ta, b, tb, weight);
		}
	}

	static class RigModel {
		final Model model = new Model();
		final ModelInstance instance;
		final Node root, upper, fore, hand;
		final NodePart part;
		final Rig rig;

		RigModel (final Matrix4 instanceTransform) {
			final Node modelRoot = node("root", 0, 0, 0, axis(0, 0, 1, 5));
			final Node modelUpper = node("upper", 1.0f, 0, 0, BIND_UPPER);
			final Node modelFore = node("fore", 0.8f, 0, 0, BIND_FORE);
			final Node modelHand = node("hand", 0.6f, 0, 0, new Quaternion());
			modelRoot.addChild(modelUpper);
			modelUpper.addChild(modelFore);
			modelFore.addChild(modelHand);
			model.nodes.add(modelRoot);

			final NodePart modelPart = new NodePart();
			modelPart.meshPart = new MeshPart("skin", null, 0, 0, 0);
			modelPart.material = new Material();
			modelPart.invBoneBindTransforms = new ArrayMap<Node, Matrix4>(true, 4);
			modelHand.parts.add(modelPart);

			// Bind pose globals give a deliberately non-identity inverse bind for every bone.
			model.calculateTransforms();
			modelPart.invBoneBindTransforms.put(modelUpper, new Matrix4(modelUpper.globalTransform).inv());
			modelPart.invBoneBindTransforms.put(modelFore, new Matrix4(modelFore.globalTransform).inv());
			modelPart.invBoneBindTransforms.put(modelHand, new Matrix4(modelHand.globalTransform).inv());
			modelPart.invBoneBindTransforms.put(modelRoot, new Matrix4(modelRoot.globalTransform).inv());

			instance = new ModelInstance(model, new Matrix4(instanceTransform));
			root = instance.getNode("root");
			upper = instance.getNode("upper");
			fore = instance.getNode("fore");
			hand = instance.getNode("hand");
			part = hand.parts.get(0);
			rig = new Rig(instance);
		}

		Animation clip (String id, Quaternion upperRotation, Quaternion foreRotation, Quaternion rootRotation) {
			final Animation animation = new Animation();
			animation.id = id;
			animation.duration = 1f;
			addTrack(animation, upper, upperRotation);
			addTrack(animation, fore, foreRotation);
			addTrack(animation, root, rootRotation);
			instance.animations.add(animation);
			return animation;
		}
	}

	static Node node (String id, float x, float y, float z, Quaternion rotation) {
		final Node node = new Node();
		node.id = id;
		node.translation.set(x, y, z);
		node.rotation.set(rotation);
		return node;
	}

	static void addTrack (Animation animation, Node node, Quaternion rotation) {
		final NodeAnimation track = new NodeAnimation();
		track.node = node;
		track.rotation = new Array<NodeKeyframe<Quaternion>>();
		track.rotation.add(new NodeKeyframe<Quaternion>(0f, rotation));
		animation.nodeAnimations.add(track);
	}

	static Quaternion axis (float x, float y, float z, float degrees) {
		return new Quaternion().setFromAxis(x, y, z, degrees);
	}

	// ---------------------------------------------------------------- reference math (independent implementation)

	static double[] quat (Quaternion q) {
		final double[] out = {q.x, q.y, q.z, q.w};
		final double len = Math.sqrt(dot(out, out));
		for (int i = 0; i < 4; i++)
			out[i] /= len;
		return out;
	}

	static double dot (double[] a, double[] b) {
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	}

	static double[] conjugate (double[] q) {
		return new double[] {-q[0], -q[1], -q[2], q[3]};
	}

	static double[] multiply (double[] a, double[] b) {
		return new double[] {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
			a[3] * b[1] + a[1] * b[3] + a[2] * b[0] - a[0] * b[2],
			a[3] * b[2] + a[2] * b[3] + a[0] * b[1] - a[1] * b[0],
			a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
	}

	static double poseDistance (double[][] a, double[][] b, double[] weights) {
		double distance = 0;
		for (int d = 0; d < weights.length; d++) {
			double d0 = dot(a[d], b[d]);
			d0 = Math.max(-1, Math.min(1, d0));
			distance += weights[d] * (1.0 - d0 * d0);
		}
		return distance;
	}

	static double[][][] sampleRelativeRotations () {
		final double[] bindUpper = quat(BIND_UPPER), bindFore = quat(BIND_FORE);
		final double[][][] out = new double[SAMPLE_ROTATIONS.length][2][];
		for (int s = 0; s < SAMPLE_ROTATIONS.length; s++) {
			out[s][0] = multiply(conjugate(bindUpper), quat(SAMPLE_ROTATIONS[s][0]));
			out[s][1] = multiply(conjugate(bindFore), quat(SAMPLE_ROTATIONS[s][1]));
		}
		return out;
	}

	static double[][] kernelMatrix (double[][][] poses, double[] weights, double beta) {
		final double[][] k = new double[poses.length][poses.length];
		for (int i = 0; i < poses.length; i++)
			for (int j = 0; j < poses.length; j++)
				k[i][j] = Math.exp(-beta * poseDistance(poses[i], poses[j], weights));
		return k;
	}

	static double[] solve (double[][] matrix, double[] rhs) {
		final int n = rhs.length;
		final double[][] a = new double[n][n + 1];
		for (int i = 0; i < n; i++) {
			System.arraycopy(matrix[i], 0, a[i], 0, n);
			a[i][n] = rhs[i];
		}
		for (int c = 0; c < n; c++) {
			int pivot = c;
			for (int r = c + 1; r < n; r++)
				if (Math.abs(a[r][c]) > Math.abs(a[pivot][c])) pivot = r;
			final double[] tmp = a[c];
			a[c] = a[pivot];
			a[pivot] = tmp;
			for (int r = c + 1; r < n; r++) {
				final double f = a[r][c] / a[c][c];
				for (int col = c; col <= n; col++)
					a[r][col] -= f * a[c][col];
			}
		}
		final double[] x = new double[n];
		for (int r = n - 1; r >= 0; r--) {
			double sum = a[r][n];
			for (int c = r + 1; c < n; c++)
				sum -= a[r][c] * x[c];
			x[r] = sum / a[r][r];
		}
		return x;
	}

	static double[][] sampleDeltasPerVertex () {
		final double[][] out = new double[SAMPLE_ROTATIONS.length][VERTEX_COUNT * 3];
		for (int s = 0; s < SAMPLE_ROTATIONS.length; s++) {
			for (int e = 0; e < SAMPLE_VERTICES[s].length; e++) {
				for (int k = 0; k < 3; k++)
					out[s][SAMPLE_VERTICES[s][e] * 3 + k] = SAMPLE_DELTAS[s][e * 3 + k];
			}
		}
		return out;
	}

	static double[] coefficientsForPose (RigModel rig, double[][][] sampleRelative, double[][] kernel, double[] weights,
		double beta) {
		final double[][] current = new double[DRIVER_COUNT][];
		for (int d = 0; d < DRIVER_COUNT; d++) {
			final Node driver = rig.instance.getNode(d == 0 ? "upper" : "fore", true);
			final Quaternion rotation = new Quaternion();
			driver.localTransform.getRotation(rotation, true);
			final double[] bind = quat(d == 0 ? BIND_UPPER : BIND_FORE);
			current[d] = multiply(conjugate(bind), quat(rotation));
		}
		final double[] k = new double[sampleRelative.length];
		for (int s = 0; s < sampleRelative.length; s++)
			k[s] = Math.exp(-beta * poseDistance(current, sampleRelative[s], weights));
		return solve(kernel, k);
	}

	static float[] expectedWorld (RigModel rig, double[] coefficients, double[][] deltasPerVertex) {
		final double[] delta = new double[VERTEX_COUNT * 3];
		for (int s = 0; s < coefficients.length; s++)
			for (int i = 0; i < delta.length; i++)
				delta[i] += coefficients[s] * deltasPerVertex[s][i];

		final float[] out = new float[VERTEX_COUNT * 3];
		final Matrix4[] palette = rig.part.bones;
		final float[] wt = rig.instance.transform.val;
		for (int v = 0; v < VERTEX_COUNT; v++) {
			final double x = BIND_POSITIONS[v * 3] + delta[v * 3];
			final double y = BIND_POSITIONS[v * 3 + 1] + delta[v * 3 + 1];
			final double z = BIND_POSITIONS[v * 3 + 2] + delta[v * 3 + 2];
			double mx = 0, my = 0, mz = 0;
			for (int j = 0; j < 4; j++) {
				final float weight = BONE_WEIGHTS[v * 4 + j];
				if (weight == 0f) continue;
				final float[] m = palette[BONE_INDICES[v * 4 + j]].val;
				mx += weight * (m[Matrix4.M00] * x + m[Matrix4.M01] * y + m[Matrix4.M02] * z + m[Matrix4.M03]);
				my += weight * (m[Matrix4.M10] * x + m[Matrix4.M11] * y + m[Matrix4.M12] * z + m[Matrix4.M13]);
				mz += weight * (m[Matrix4.M20] * x + m[Matrix4.M21] * y + m[Matrix4.M22] * z + m[Matrix4.M23]);
			}
			out[v * 3] = (float)(wt[Matrix4.M00] * mx + wt[Matrix4.M01] * my + wt[Matrix4.M02] * mz + wt[Matrix4.M03]);
			out[v * 3 + 1] = (float)(wt[Matrix4.M10] * mx + wt[Matrix4.M11] * my + wt[Matrix4.M12] * mz + wt[Matrix4.M13]);
			out[v * 3 + 2] = (float)(wt[Matrix4.M20] * mx + wt[Matrix4.M21] * my + wt[Matrix4.M22] * mz + wt[Matrix4.M23]);
		}
		return out;
	}

	// ---------------------------------------------------------------- helpers

	static void check (boolean condition, String message) {
		if (!condition) throw new AssertionError(message);
	}

	static void compareWorld (String what, float[] actual, float[] expected) {
		for (int i = 0; i < expected.length; i++) {
			final float diff = Math.abs(actual[i] - expected[i]);
			if (diff > TOLERANCE)
				throw new AssertionError(what + " component " + i + " differs: actual=" + actual[i] + " expected=" + expected[i]
					+ " diff=" + diff);
		}
	}

	interface Throwing {
		void run ();
	}

	static void expectIllegalArgument (String what, Throwing runnable) {
		try {
			runnable.run();
		} catch (IllegalArgumentException expected) {
			return;
		} catch (RuntimeException other) {
			throw new AssertionError(what + " threw " + other.getClass().getSimpleName() + " instead of IllegalArgumentException");
		}
		throw new AssertionError(what + " did not throw IllegalArgumentException");
	}

	static PoseSpaceDeformer.Sample[] buildSamples () {
		final PoseSpaceDeformer.Sample[] samples = new PoseSpaceDeformer.Sample[SAMPLE_ROTATIONS.length];
		for (int s = 0; s < SAMPLE_ROTATIONS.length; s++) {
			final Quaternion[] rotations = new Quaternion[DRIVER_COUNT];
			for (int d = 0; d < DRIVER_COUNT; d++)
				rotations[d] = new Quaternion(SAMPLE_ROTATIONS[s][d]);
			samples[s] = new PoseSpaceDeformer.Sample(rotations, SAMPLE_VERTICES[s].clone(), SAMPLE_DELTAS[s].clone());
		}
		return samples;
	}

	static PoseSpaceDeformer.Prepared prepareMain () {
		final Quaternion[] bindRotations = {new Quaternion(BIND_UPPER), new Quaternion(BIND_FORE)};
		return PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, bindRotations,
			new double[] {DRIVER_WEIGHTS[0], DRIVER_WEIGHTS[1]}, BETA, buildSamples());
	}

	static RigModel rigWithClips (Matrix4 transform, Animation[] outClips) {
		final RigModel rig = new RigModel(transform);
		outClips[0] = rig.clip("neutral", BIND_UPPER, BIND_FORE, axis(0, 0, 1, 5));
		outClips[1] = rig.clip("pose1", axis(0, 0, 1, 70), BIND_FORE, axis(0, 0, 1, 5));
		outClips[2] = rig.clip("pose2", BIND_UPPER, axis(1, 0, 0, 20), axis(0, 0, 1, 5));
		outClips[3] = rig.clip("pose3", axis(0, 0, 1, 65), axis(1, 0, 0, 15), axis(0, 0, 1, 5));
		outClips[4] = rig.clip("extrapolate", axis(0, 0, 1, 50), axis(1, 0, 0, -75), axis(0, 0, 1, 30));
		outClips[5] = rig.clip("blendB", axis(0, 0, 1, 65), axis(1, 0, 0, 15), axis(0, 0, 1, 40));
		return rig;
	}

	static double quatDistance (Quaternion a, Quaternion b) {
		final double dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
		return Math.abs(1.0 - Math.abs(dot));
	}

	static boolean matrixClose (Matrix4 a, Matrix4 b, float epsilon) {
		for (int i = 0; i < 16; i++)
			if (Math.abs(a.val[i] - b.val[i]) > epsilon) return false;
		return true;
	}

	/** Deliberately wrong pipeline used to prove the ordering matters: skin first, then add the correction in world space. */
	static float[] wrongOrderWorld (RigModel rig, double[] coefficients, double[][] deltasPerVertex) {
		final float[] correct = expectedWorld(rig, new double[coefficients.length], deltasPerVertex);
		final double[] delta = new double[VERTEX_COUNT * 3];
		for (int s = 0; s < coefficients.length; s++)
			for (int i = 0; i < delta.length; i++)
				delta[i] += coefficients[s] * deltasPerVertex[s][i];
		final float[] wt = rig.instance.transform.val;
		for (int v = 0; v < VERTEX_COUNT; v++) {
			final double dx = wt[Matrix4.M00] * delta[v * 3] + wt[Matrix4.M01] * delta[v * 3 + 1]
				+ wt[Matrix4.M02] * delta[v * 3 + 2];
			final double dy = wt[Matrix4.M10] * delta[v * 3] + wt[Matrix4.M11] * delta[v * 3 + 1]
				+ wt[Matrix4.M12] * delta[v * 3 + 2];
			final double dz = wt[Matrix4.M20] * delta[v * 3] + wt[Matrix4.M21] * delta[v * 3 + 1]
				+ wt[Matrix4.M22] * delta[v * 3 + 2];
			correct[v * 3] += (float)dx;
			correct[v * 3 + 1] += (float)dy;
			correct[v * 3 + 2] += (float)dz;
		}
		return correct;
	}

	// ---------------------------------------------------------------- tests

	public static void main (String[] args) {
		final Animation[] clipsA = new Animation[6];
		final Matrix4 transformA = new Matrix4().setToTranslation(1.5f, -0.5f, 2f).rotate(0, 1, 0, 25).scl(1.3f);
		final RigModel a = rigWithClips(transformA, clipsA);
		final Animation[] clipsB = new Animation[6];
		final RigModel b = rigWithClips(new Matrix4().setToTranslation(-2f, 1f, 0.5f), clipsB);

		final PoseSpaceDeformer.Prepared prepared = prepareMain();
		final double[][][] sampleRelative = sampleRelativeRotations();
		final double[][] kernel = kernelMatrix(sampleRelative, DRIVER_WEIGHTS, BETA);
		final double[][] deltasPerVertex = sampleDeltasPerVertex();

		// Every sample pose reproduces its authored deltas, and the neutral pose leaves the bind mesh untouched.
		for (int s = 0; s < SAMPLE_ROTATIONS.length; s++) {
			a.rig.sample(clipsA[s], 0f);
			final double[] coefficients = coefficientsForPose(a, sampleRelative, kernel, DRIVER_WEIGHTS, BETA);
			for (int other = 0; other < coefficients.length; other++) {
				final double expectedCoefficient = other == s ? 1.0 : 0.0;
				check(Math.abs(coefficients[other] - expectedCoefficient) < 1e-6,
					"sample " + s + " coefficient " + other + " should be " + expectedCoefficient);
			}
			final float[] expected = expectedWorld(a, coefficients, deltasPerVertex);
			final float[] actual = new float[BIND_POSITIONS.length];
			prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, actual);
			compareWorld("sample " + s, actual, expected);
			if (s == 0) {
				final double[] zero = new double[SAMPLE_ROTATIONS.length];
				compareWorld("neutral", actual, expectedWorld(a, zero, deltasPerVertex));
			}
		}

		// Blended animation: Node.rotation must still be the bind value while the animated local matrix drives the pose.
		check(quatDistance(a.instance.getNode("upper").rotation, BIND_UPPER) < 1e-3, "upper Node.rotation must remain the bind value");
		check(quatDistance(a.instance.getNode("fore").rotation, BIND_FORE) < 1e-3, "fore Node.rotation must remain the bind value");
		a.rig.blend(clipsA[1], 0f, clipsA[3], 0f, 0.5f);
		final Quaternion blendedUpper = new Quaternion();
		final Quaternion blendedFore = new Quaternion();
		a.instance.getNode("upper").localTransform.getRotation(blendedUpper, true);
		a.instance.getNode("fore").localTransform.getRotation(blendedFore, true);
		check(quatDistance(blendedUpper, BIND_UPPER) > 1e-2, "blended upper local matrix differs from bind");
		check(quatDistance(a.instance.getNode("upper").rotation, blendedUpper) > 1e-2,
			"Node.rotation and animated local rotation differ");
		final double[] blendCoefficients = coefficientsForPose(a, sampleRelative, kernel, DRIVER_WEIGHTS, BETA);
		final float[] blendActual = new float[BIND_POSITIONS.length];
		prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, blendActual);
		compareWorld("blended animation", blendActual, expectedWorld(a, blendCoefficients, deltasPerVertex));

		// Pose outside the sampled region: the full kernel solve yields negative interpolation coefficients.
		a.rig.sample(clipsA[4], 0f);
		final double[] extrapolated = coefficientsForPose(a, sampleRelative, kernel, DRIVER_WEIGHTS, BETA);
		boolean negative = false;
		for (int s = 0; s < extrapolated.length; s++)
			if (extrapolated[s] < -0.01) negative = true;
		check(negative, "expected negative interpolation coefficients for an extrapolated pose");
		final float[] extrapolatedActual = new float[BIND_POSITIONS.length];
		prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, extrapolatedActual);
		compareWorld("extrapolated pose", extrapolatedActual, expectedWorld(a, extrapolated, deltasPerVertex));

		// Sign flipped driver rotations describe the same pose.
		a.rig.sample(clipsA[1], 0f);
		final float[] beforeFlip = new float[BIND_POSITIONS.length];
		prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, beforeFlip);
		final Node upperNode = a.instance.getNode("upper", true);
		final Quaternion currentUpper = new Quaternion();
		upperNode.localTransform.getRotation(currentUpper, true);
		final Quaternion flipped = new Quaternion(currentUpper).mul(-1f);
		upperNode.localTransform.set(upperNode.translation, flipped, upperNode.scale);
		upperNode.isAnimated = true;
		a.instance.calculateTransforms();
		final float[] afterFlip = new float[BIND_POSITIONS.length];
		prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, afterFlip);
		compareWorld("negated driver rotation", afterFlip, beforeFlip);
		a.rig.sample(clipsA[1], 0f); // restore

		// In-place output.
		a.rig.sample(clipsA[3], 0f);
		final double[] inPlaceCoefficients = coefficientsForPose(a, sampleRelative, kernel, DRIVER_WEIGHTS, BETA);
		final float[] expectedInPlace = expectedWorld(a, inPlaceCoefficients, deltasPerVertex);
		final float[] inPlace = BIND_POSITIONS.clone();
		prepared.evaluate(a.instance, a.part, inPlace, BONE_INDICES, BONE_WEIGHTS, inPlace);
		compareWorld("in-place output", inPlace, expectedInPlace);

		// Ordering proof: applying the correction after skinning would land on visibly different world points.
		final float[] wrongOrder = wrongOrderWorld(a, inPlaceCoefficients, deltasPerVertex);
		float largestDifference = 0f;
		for (int i = 0; i < wrongOrder.length; i++)
			largestDifference = Math.max(largestDifference, Math.abs(wrongOrder[i] - inPlace[i]));
		check(largestDifference > 1e-2f, "correction must be applied before skinning; difference was " + largestDifference);

		// Two instances, interleaved, sharing one prepared result.
		a.rig.sample(clipsA[4], 0f);
		final float[] firstA = new float[BIND_POSITIONS.length];
		prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, firstA);
		b.rig.sample(clipsB[2], 0f);
		final double[] expectedB = coefficientsForPose(b, sampleRelative, kernel, DRIVER_WEIGHTS, BETA);
		final float[] actualB = new float[BIND_POSITIONS.length];
		prepared.evaluate(b.instance, b.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, actualB);
		compareWorld("second instance", actualB, expectedWorld(b, expectedB, deltasPerVertex));
		final float[] secondA = new float[BIND_POSITIONS.length];
		prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, secondA);
		compareWorld("interleaved re-evaluation", secondA, firstA);

		// Prepared data is independent from the caller-owned input arrays.
		final Quaternion[] mutableBind = {new Quaternion(BIND_UPPER), new Quaternion(BIND_FORE)};
		final double[] mutableWeights = {DRIVER_WEIGHTS[0], DRIVER_WEIGHTS[1]};
		final String[] mutableIds = {"upper", "fore"};
		final PoseSpaceDeformer.Sample[] mutableSamples = buildSamples();
		final PoseSpaceDeformer.Prepared copyPrepared = PoseSpaceDeformer.prepare(VERTEX_COUNT, mutableIds, mutableBind,
			mutableWeights, BETA, mutableSamples);
		mutableIds[0] = "missing";
		mutableBind[0].set(0, 0, 0, 2f);
		mutableWeights[0] = 99.0;
		mutableSamples[3].rotations[0].set(1, 0, 0, 0);
		mutableSamples[3].vertexIndices[0] = 4;
		mutableSamples[3].deltas[0] = 123f;
		a.rig.sample(clipsA[3], 0f);
		final float[] copyActual = new float[BIND_POSITIONS.length];
		copyPrepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, copyActual);
		compareWorld("prepared copy independence", copyActual, expectedWorld(a, inPlaceCoefficients, deltasPerVertex));

		// Robustness: equivalent poses (including negated quaternions) are rejected.
		expectIllegalArgument("duplicate sample", new Throwing() {
			public void run () {
				final PoseSpaceDeformer.Sample[] samples = buildSamples();
				samples[2] = new PoseSpaceDeformer.Sample(new Quaternion[] {new Quaternion(SAMPLE_ROTATIONS[1][0]),
					new Quaternion(SAMPLE_ROTATIONS[1][1])}, new int[0], new float[0]);
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, new Quaternion[] {
					new Quaternion(BIND_UPPER), new Quaternion(BIND_FORE)}, new double[] {1.0, 0.7}, BETA, samples);
			}
		});
		expectIllegalArgument("sign flipped duplicate", new Throwing() {
			public void run () {
				final PoseSpaceDeformer.Sample[] samples = buildSamples();
				samples[2] = new PoseSpaceDeformer.Sample(new Quaternion[] {
					new Quaternion(SAMPLE_ROTATIONS[1][0]).mul(-1f), new Quaternion(SAMPLE_ROTATIONS[1][1])}, new int[0],
					new float[0]);
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, new Quaternion[] {
					new Quaternion(BIND_UPPER), new Quaternion(BIND_FORE)}, new double[] {1.0, 0.7}, BETA, samples);
			}
		});

		// Near singular kernel from an under-weighted driver.
		expectIllegalArgument("near singular kernel", new Throwing() {
			public void run () {
					final PoseSpaceDeformer.Sample[] samples = {
					new PoseSpaceDeformer.Sample(new Quaternion[] {new Quaternion(), new Quaternion()}, new int[0], new float[0]),
					new PoseSpaceDeformer.Sample(new Quaternion[] {new Quaternion(), axis(1, 0, 0, 20)}, new int[] {0},
						new float[] {1, 0, 0}),
					new PoseSpaceDeformer.Sample(new Quaternion[] {new Quaternion(), axis(1, 0, 0, 80)}, new int[] {0},
						new float[] {2, 0, 0}),
					new PoseSpaceDeformer.Sample(new Quaternion[] {new Quaternion(), axis(1, 0, 0, -50)}, new int[] {0},
						new float[] {3, 0, 0})};
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, new Quaternion[] {new Quaternion(),
					new Quaternion()}, new double[] {1.0, 1e-13}, BETA, samples);
			}
		});

		// Input validation.
		// (driver ids, bind rotations and weights are supplied fresh per case to keep each rejection isolated.)
		expectIllegalArgument("zero vertices", new Throwing() {
			public void run () {
				PoseSpaceDeformer.prepare(0, new String[] {"upper", "fore"}, new Quaternion[] {new Quaternion(),
					new Quaternion()}, new double[] {1, 1}, BETA, buildSamples());
			}
		});
		expectIllegalArgument("empty drivers", new Throwing() {
			public void run () {
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[0], new Quaternion[0], new double[0], BETA, buildSamples());
			}
		});
		expectIllegalArgument("duplicate ids", new Throwing() {
			public void run () {
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "upper"}, new Quaternion[] {new Quaternion(),
					new Quaternion()}, new double[] {1, 1}, BETA, buildSamples());
			}
		});
		expectIllegalArgument("bad beta", new Throwing() {
			public void run () {
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, new Quaternion[] {new Quaternion(),
					new Quaternion()}, new double[] {1, 1}, 0.0, buildSamples());
			}
		});
		expectIllegalArgument("bad weight", new Throwing() {
			public void run () {
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, new Quaternion[] {new Quaternion(),
					new Quaternion()}, new double[] {1, 0}, BETA, buildSamples());
			}
		});
		expectIllegalArgument("non unit rotation", new Throwing() {
			public void run () {
				final Quaternion[] bind = {new Quaternion(BIND_UPPER), new Quaternion(BIND_FORE)};
				final PoseSpaceDeformer.Sample[] samples = buildSamples();
				samples[1].rotations[0].set(0, 0, 0, 2f);
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, bind, new double[] {1, 0.7}, BETA,
					samples);
			}
		});
		expectIllegalArgument("non finite rotation", new Throwing() {
			public void run () {
				final Quaternion[] bind = {new Quaternion(BIND_UPPER), new Quaternion(BIND_FORE)};
				final PoseSpaceDeformer.Sample[] samples = buildSamples();
				samples[1].rotations[0].set(Float.NaN, 0, 0, 1);
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, bind, new double[] {1, 0.7}, BETA,
					samples);
			}
		});
		expectIllegalArgument("out of range vertex", new Throwing() {
			public void run () {
				final Quaternion[] bind = {new Quaternion(BIND_UPPER), new Quaternion(BIND_FORE)};
				final PoseSpaceDeformer.Sample[] samples = buildSamples();
				samples[1].vertexIndices[0] = VERTEX_COUNT;
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, bind, new double[] {1, 0.7}, BETA,
					samples);
			}
		});
		expectIllegalArgument("duplicate vertex", new Throwing() {
			public void run () {
				final Quaternion[] bind = {new Quaternion(BIND_UPPER), new Quaternion(BIND_FORE)};
				final PoseSpaceDeformer.Sample[] samples = buildSamples();
				samples[2] = new PoseSpaceDeformer.Sample(new Quaternion[] {new Quaternion(SAMPLE_ROTATIONS[2][0]),
					new Quaternion(SAMPLE_ROTATIONS[2][1])}, new int[] {1, 1}, new float[] {1, 2, 3, 4, 5, 6});
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, bind, new double[] {1, 0.7}, BETA,
					samples);
			}
		});
		expectIllegalArgument("non finite delta", new Throwing() {
			public void run () {
				final Quaternion[] bind = {new Quaternion(BIND_UPPER), new Quaternion(BIND_FORE)};
				final PoseSpaceDeformer.Sample[] samples = buildSamples();
				samples[1].deltas[0] = Float.NaN;
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, bind, new double[] {1, 0.7}, BETA,
					samples);
			}
		});
		expectIllegalArgument("delta length", new Throwing() {
			public void run () {
				final Quaternion[] bind = {new Quaternion(BIND_UPPER), new Quaternion(BIND_FORE)};
				final PoseSpaceDeformer.Sample[] samples = buildSamples();
				samples[1].deltas = new float[] {1, 2};
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, bind, new double[] {1, 0.7}, BETA,
					samples);
			}
		});
		expectIllegalArgument("missing neutral", new Throwing() {
			public void run () {
				final Quaternion[] bind = {new Quaternion(BIND_UPPER), new Quaternion(BIND_FORE)};
				final PoseSpaceDeformer.Sample[] samples = buildSamples();
				samples[0].deltas = new float[] {0.1f, 0.2f, 0.3f};
				samples[0].vertexIndices = new int[] {0};
				PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"upper", "fore"}, bind, new double[] {1, 0.7}, BETA,
					samples);
			}
		});

		// Evaluation validation: invalid input must throw and leave the output untouched.
		final float[] sentinel = new float[BIND_POSITIONS.length];
		for (int i = 0; i < sentinel.length; i++)
			sentinel[i] = 7.0f;

		expectIllegalArgument("other instance part", new Throwing() {
			public void run () {
				prepared.evaluate(b.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, sentinel);
			}
		});
		expectIllegalArgument("wrong bind length", new Throwing() {
			public void run () {
				prepared.evaluate(a.instance, a.part, new float[BIND_POSITIONS.length - 1], BONE_INDICES, BONE_WEIGHTS, sentinel);
			}
		});
		expectIllegalArgument("wrong output length", new Throwing() {
			public void run () {
				prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, new float[3]);
			}
		});
		expectIllegalArgument("wrong bone index length", new Throwing() {
			public void run () {
				prepared.evaluate(a.instance, a.part, BIND_POSITIONS, new int[3], BONE_WEIGHTS, sentinel);
			}
		});
		expectIllegalArgument("invalid bone index", new Throwing() {
			public void run () {
				final int[] bad = BONE_INDICES.clone();
				bad[0] = 99;
				prepared.evaluate(a.instance, a.part, BIND_POSITIONS, bad, BONE_WEIGHTS, sentinel);
			}
		});
		expectIllegalArgument("illegal index in zero weight slot", new Throwing() {
			public void run () {
				final int[] bad = BONE_INDICES.clone();
				final float[] weights = BONE_WEIGHTS.clone();
				weights[0] = 1f;
				weights[1] = weights[2] = weights[3] = 0f;
				bad[1] = 99;
				prepared.evaluate(a.instance, a.part, BIND_POSITIONS, bad, weights, sentinel);
			}
		});
		expectIllegalArgument("invalid weight", new Throwing() {
			public void run () {
				final float[] bad = BONE_WEIGHTS.clone();
				bad[0] = -1f;
				prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, bad, sentinel);
			}
		});
		expectIllegalArgument("weights do not sum to one", new Throwing() {
			public void run () {
				final float[] bad = BONE_WEIGHTS.clone();
				bad[0] = 0.2f;
				prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, bad, sentinel);
			}
		});
		a.rig.sample(clipsA[1], 0f);
		final Matrix4[] savedBones = a.part.bones;
		a.part.bones = null;
		expectIllegalArgument("missing palette", new Throwing() {
			public void run () {
				prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, sentinel);
			}
		});
		a.part.bones = savedBones;
		final float savedFinite = a.part.bones[0].val[0];
		a.part.bones[0].val[0] = Float.NaN;
		expectIllegalArgument("non finite palette", new Throwing() {
			public void run () {
				prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, sentinel);
			}
		});
		a.part.bones[0].val[0] = savedFinite;

		// Driver local matrix constraints: non-uniform scale, shear and missing node.
		final Node upperDriver = a.instance.getNode("upper", true);
		final Matrix4 savedLocal = new Matrix4(upperDriver.localTransform);
		upperDriver.localTransform.set(upperDriver.translation, upperDriver.rotation, new Vector3(2, 1, 1));
		upperDriver.isAnimated = true;
		a.instance.calculateTransforms();
		expectIllegalArgument("non uniform driver scale", new Throwing() {
			public void run () {
				prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, sentinel);
			}
		});
		upperDriver.localTransform.set(savedLocal);
		upperDriver.localTransform.val[Matrix4.M01] += 0.5f;
		upperDriver.isAnimated = true;
		a.instance.calculateTransforms();
		expectIllegalArgument("sheared driver", new Throwing() {
			public void run () {
				prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, sentinel);
			}
		});
		upperDriver.localTransform.set(savedLocal);
		a.instance.calculateTransforms();
		final String savedId = upperDriver.id;
		upperDriver.id = "renamedUpper";
		expectIllegalArgument("missing driver", new Throwing() {
			public void run () {
				prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, sentinel);
			}
		});
		upperDriver.id = savedId;

		boolean untouched = true;
		for (int i = 0; i < sentinel.length; i++)
			if (sentinel[i] != 7.0f) untouched = false;
		check(untouched, "failed evaluations must not modify the output array");

		// Drivers, palettes and the prepared result remain usable after the failed calls.
		a.rig.sample(clipsA[3], 0f);
		final double[] recovered = coefficientsForPose(a, sampleRelative, kernel, DRIVER_WEIGHTS, BETA);
		final float[] recoveredActual = new float[BIND_POSITIONS.length];
		prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, recoveredActual);
		compareWorld("evaluation after rejections", recoveredActual, expectedWorld(a, recovered, deltasPerVertex));

		// The prepared data and the instance animation state are not modified by evaluation.
		check(quatDistance(a.instance.getNode("upper").rotation, BIND_UPPER) < 1e-3, "Node.rotation must stay untouched");
		final Matrix4 referenceBones = new Matrix4(a.part.bones[0]);
		final float[] scratch = new float[BIND_POSITIONS.length];
		prepared.evaluate(a.instance, a.part, BIND_POSITIONS, BONE_INDICES, BONE_WEIGHTS, scratch);
		check(matrixClose(a.part.bones[0], referenceBones, 1e-6f), "palette must stay untouched");

		System.out.println("GDX_CORRECTIVE_OK");
	}
}
