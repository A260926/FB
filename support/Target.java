import com.badlogic.gdx.graphics.g3d.Model;
import com.badlogic.gdx.graphics.g3d.ModelInstance;
import com.badlogic.gdx.graphics.g3d.model.Animation;
import com.badlogic.gdx.graphics.g3d.model.Node;
import com.badlogic.gdx.graphics.g3d.model.NodeAnimation;
import com.badlogic.gdx.graphics.g3d.model.NodeKeyframe;
import com.badlogic.gdx.graphics.g3d.model.NodePart;
import com.badlogic.gdx.graphics.g3d.utils.AnimationController;
import com.badlogic.gdx.graphics.g3d.utils.BaseAnimationController;
import com.badlogic.gdx.graphics.g3d.utils.PoseSpaceDeformer;
import com.badlogic.gdx.graphics.g3d.utils.PoseSpaceDeformer.Prepared;
import com.badlogic.gdx.graphics.g3d.utils.PoseSpaceDeformer.Sample;
import com.badlogic.gdx.math.Matrix4;
import com.badlogic.gdx.math.Quaternion;
import com.badlogic.gdx.math.Vector3;
import com.badlogic.gdx.utils.Array;
import com.badlogic.gdx.utils.ArrayMap;

/** Headless verification for {@link PoseSpaceDeformer}. Everything is procedural: no window, no OpenGL, no native
 * libraries. The assertions land on final world-space points, never on kernel weights or intermediate quaternions. */
public class Target {
	static final double BETA = 4.0;
	static final int VERTEX_COUNT = 5;
	static final int CORRECTIVE_A = 2;
	static final int CORRECTIVE_B = 3;
	static final double TOLERANCE = 2e-4;

	static void check (boolean condition, String message) {
		if (!condition) throw new AssertionError(message);
	}

	static void close (Vector3 actual, Vector3 expected, String message) {
		float dx = actual.x - expected.x, dy = actual.y - expected.y, dz = actual.z - expected.z;
		double error = Math.sqrt(dx * dx + dy * dy + dz * dz);
		if (error > TOLERANCE)
			throw new AssertionError(message + " expected " + expected + " but got " + actual + " (error " + error + ")");
	}

	/** One arm: two non-collinear driver bones (elbow bending around Z, wrist twisting around X) under a non-driver
	 * root, plus a skinned NodePart with a three-bone palette and non-identity inverse binds. */
	static class Rig {
		Model model;
		Node root, elbow, wrist;
		NodePart part;

		Rig () {
			model = new Model();
			root = new Node();
			root.id = "root";
			elbow = new Node();
			elbow.id = "elbow";
			elbow.translation.set(1f, 0f, 0f);
			wrist = new Node();
			wrist.id = "wrist";
			wrist.translation.set(0f, 1f, 0f);
			root.addChild(elbow);
			elbow.addChild(wrist);
			model.nodes.add(root);

			part = new NodePart();
			part.invBoneBindTransforms = new ArrayMap<>(Node.class, Matrix4.class);
			// Deliberately non-identity inverse bind transforms.
			part.invBoneBindTransforms.put(root, new Matrix4().setToTranslation(0.5f, -0.25f, 0.75f).inv());
			part.invBoneBindTransforms.put(elbow, new Matrix4().setToTranslation(-2f, 3f, 1f).inv());
			part.invBoneBindTransforms.put(wrist, new Matrix4().setToTranslation(4f, -5f, 2f).inv());
			part.bones = new Matrix4[] {new Matrix4(), new Matrix4(), new Matrix4()};
			root.parts.add(part);
			// The instance copies this clip, retargeted at the instance nodes, for AnimationController.
			model.animations.add(clip(elbow, wrist, 90f, 90f));
			model.calculateTransforms();
		}
	}

	static Quaternion elbowPose (float degrees) {
		return new Quaternion().setFromAxis(0, 0, 1, degrees);
	}

	static Quaternion wristPose (float degrees) {
		return new Quaternion().setFromAxis(1, 0, 0, degrees);
	}

	static float[] bindPositions () {
		return new float[] {
			0.0f, 0.0f, 0.0f,
			1.0f, 1.0f, 1.0f,
			2.0f, 0.0f, -1.0f, // corrective vertex A
			0.5f, -2.0f, 3.0f, // corrective vertex B
			-1.5f, 2.5f, 0.5f};
	}

	static int[] boneIndices () {
		return new int[] {
			0, 0, 0, 0,
			1, 1, 1, 1,
			0, 1, 2, 0, // corrective vertex A uses all three palette bones
			2, 0, 1, 0, // corrective vertex B uses all three palette bones
			0, 2, 1, 0};
	}

	static float[] boneWeights () {
		return new float[] {
			1f, 0f, 0f, 0f,
			0f, 1f, 0f, 0f,
			0.5f, 0.3f, 0.2f, 0f,
			0.4f, 0.35f, 0.25f, 0f,
			0.7f, 0.2f, 0.1f, 0f};
	}

	/** Builds a one-second clip targeting the given (model or instance) driver nodes. */
	static Animation clip (Node elbowNode, Node wristNode, float elbowDegrees, float wristDegrees) {
		Animation clip = new Animation();
		clip.id = "pose";
		clip.duration = 1f;
		NodeAnimation elbowTrack = new NodeAnimation();
		elbowTrack.node = elbowNode;
		elbowTrack.rotation = new Array<>();
		elbowTrack.rotation.add(new NodeKeyframe<Quaternion>(0f, new Quaternion()));
		elbowTrack.rotation.add(new NodeKeyframe<Quaternion>(1f, elbowPose(elbowDegrees)));
		clip.nodeAnimations.add(elbowTrack);
		NodeAnimation wristTrack = new NodeAnimation();
		wristTrack.node = wristNode;
		wristTrack.rotation = new Array<>();
		wristTrack.rotation.add(new NodeKeyframe<Quaternion>(0f, new Quaternion()));
		wristTrack.rotation.add(new NodeKeyframe<Quaternion>(1f, wristPose(wristDegrees)));
		clip.nodeAnimations.add(wristTrack);
		return clip;
	}

	static class DirectPlayer extends BaseAnimationController {
		DirectPlayer (ModelInstance instance) {
			super(instance);
		}

		void sample (Animation animation, float time) {
			applyAnimation(animation, time);
		}
	}

	/** Creates an instance and drives it to the requested pose through a real animation controller. */
	static ModelInstance posed (Rig rig, float elbowDegrees, float wristDegrees) {
		ModelInstance instance = new ModelInstance(rig.model);
		Animation instanceClip = clip(instance.getNode("elbow"), instance.getNode("wrist"),
			elbowDegrees, wristDegrees);
		new DirectPlayer(instance).sample(instanceClip, 1f);
		return instance;
	}

	static NodePart partOf (ModelInstance instance) {
		return instance.nodes.first().parts.first();
	}

	static Vector3 vertex (float[] packed, int index) {
		return new Vector3(packed[index * 3], packed[index * 3 + 1], packed[index * 3 + 2]);
	}

	// --- Independent double-precision reference implementation of the whole pipeline ---

	static double[] quat (Quaternion q) {
		return new double[] {q.x, q.y, q.z, q.w};
	}

	static double[] mulConj (double[] a, double[] b) {
		double ax = -a[0], ay = -a[1], az = -a[2], aw = a[3];
		return new double[] {
			aw * b[0] + ax * b[3] + ay * b[2] - az * b[1],
			aw * b[1] + ay * b[3] + az * b[0] - ax * b[2],
			aw * b[2] + az * b[3] + ax * b[1] - ay * b[0],
			aw * b[3] - ax * b[0] - ay * b[1] - az * b[2]};
	}

	static double dot (double[] a, double[] b) {
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	}

	static double clamp (double v) {
		return Math.max(-1.0, Math.min(1.0, v));
	}

	static double referenceDistance (double[][][] a, double[][][] b, double[] weights) {
		double distance = 0.0;
		for (int bone = 0; bone < a.length; bone++) {
			double d = clamp(dot(a[bone], b[bone]));
			distance += weights[bone] * (1.0 - d * d);
		}
		return distance;
	}

	static double[] referenceSolve (double[][] matrix, double[] rhs) {
		int n = matrix.length;
		double[][] a = new double[n][n];
		for (int i = 0; i < n; i++)
			System.arraycopy(matrix[i], 0, a[i], 0, n);
		int[] piv = new int[n];
		for (int k = 0; k < n; k++) {
			int row = k;
			double best = Math.abs(a[k][k]);
			for (int i = k + 1; i < n; i++) {
				if (Math.abs(a[i][k]) > best) {
					best = Math.abs(a[i][k]);
					row = i;
				}
			}
			check(best >= 1e-10, "reference matrix unexpectedly singular");
			double[] tmp = a[k];
			a[k] = a[row];
			a[row] = tmp;
			piv[k] = row;
			for (int i = k + 1; i < n; i++) {
				a[i][k] /= a[k][k];
				for (int j = k + 1; j < n; j++)
					a[i][j] -= a[i][k] * a[k][j];
			}
		}
		double[] y = new double[n];
		for (int i = 0; i < n; i++) {
			y[i] = rhs[piv[i]];
			for (int j = 0; j < i; j++)
				y[i] -= a[i][j] * y[j];
		}
		double[] x = new double[n];
		for (int i = n - 1; i >= 0; i--) {
			x[i] = y[i];
			for (int j = i + 1; j < n; j++)
				x[i] -= a[i][j] * x[j];
			x[i] /= a[i][i];
		}
		return x;
	}

	static Vector3 referenceWorld (Matrix4[] palette, ModelInstance instance, float[] bind, int[] indices,
		float[] weights, double[] coeffs, Sample[] samples, int vertex) {
		int base = vertex * 3;
		double px = bind[base], py = bind[base + 1], pz = bind[base + 2];
		for (int i = 0; i < samples.length; i++) {
			Sample sample = samples[i];
			for (int k = 0; k < sample.vertexIndices.length; k++) {
				if (sample.vertexIndices[k] == vertex) {
					px += coeffs[i] * sample.deltas[k * 3];
					py += coeffs[i] * sample.deltas[k * 3 + 1];
					pz += coeffs[i] * sample.deltas[k * 3 + 2];
				}
			}
		}
		double sx = 0.0, sy = 0.0, sz = 0.0;
		for (int slot = 0; slot < 4; slot++) {
			float w = weights[vertex * 4 + slot];
			if (w == 0f) continue;
			float[] m = palette[indices[vertex * 4 + slot]].val;
			double bx = px * m[Matrix4.M00] + py * m[Matrix4.M01] + pz * m[Matrix4.M02] + m[Matrix4.M03];
			double by = px * m[Matrix4.M10] + py * m[Matrix4.M11] + pz * m[Matrix4.M12] + m[Matrix4.M13];
			double bz = px * m[Matrix4.M20] + py * m[Matrix4.M21] + pz * m[Matrix4.M22] + m[Matrix4.M23];
			sx += w * bx;
			sy += w * by;
			sz += w * bz;
		}
		float[] m = instance.transform.val;
		return new Vector3(
			(float)(sx * m[Matrix4.M00] + sy * m[Matrix4.M01] + sz * m[Matrix4.M02] + m[Matrix4.M03]),
			(float)(sx * m[Matrix4.M10] + sy * m[Matrix4.M11] + sz * m[Matrix4.M12] + m[Matrix4.M13]),
			(float)(sx * m[Matrix4.M20] + sy * m[Matrix4.M21] + sz * m[Matrix4.M22] + m[Matrix4.M23]));
	}

	static Sample[] buildSamples () {
		Sample neutral = new Sample(new Quaternion[] {new Quaternion(), new Quaternion()}, new int[0], new float[0]);
		Sample flex = new Sample(new Quaternion[] {elbowPose(50f), new Quaternion()},
			new int[] {CORRECTIVE_A, CORRECTIVE_B},
			new float[] {0.12f, -0.08f, 0.05f, -0.04f, 0.10f, 0.02f});
		Sample twist = new Sample(new Quaternion[] {new Quaternion(), wristPose(35f)},
			new int[] {CORRECTIVE_A}, new float[] {-0.06f, 0.09f, 0.14f});
		Sample combined = new Sample(new Quaternion[] {elbowPose(35f), wristPose(25f)},
			new int[] {CORRECTIVE_B}, new float[] {0.07f, 0.03f, -0.11f});
		return new Sample[] {neutral, flex, twist, combined};
	}

	static Prepared buildPrepared (Sample[] samples, double[] weights) {
		return PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow", "wrist"},
			new Quaternion[] {new Quaternion(), new Quaternion()}, weights, BETA, samples);
	}

	static double[][][] relativeRotations (Sample sample) {
		double[] bind = quat(new Quaternion());
		return new double[][][] {mulConj(bind, quat(sample.rotations[0])), mulConj(bind, quat(sample.rotations[1]))};
	}

	public static void main (String[] args) {
		Rig rig = new Rig();
		Sample[] samples = buildSamples();
		double[] driverWeights = {1.0, 0.7};
		Prepared prepared = buildPrepared(samples, driverWeights);

		float[] bind = bindPositions();
		int[] indices = boneIndices();
		float[] weights = boneWeights();

		// --- every sample pose reproduces its own deltas; the neutral pose is unchanged ---
		float[][] samplePoses = {{0f, 0f}, {50f, 0f}, {0f, 35f}, {35f, 25f}};
		for (int sampleIndex = 0; sampleIndex < samples.length; sampleIndex++) {
			ModelInstance instance = posed(rig, samplePoses[sampleIndex][0], samplePoses[sampleIndex][1]);
			Matrix4[] palette = partOf(instance).bones;
			float[] out = new float[VERTEX_COUNT * 3];
			prepared.evaluate(instance, partOf(instance), bind, indices, weights, out);
			double[] expectedCoeffs = new double[samples.length];
			expectedCoeffs[sampleIndex] = 1.0;
			for (int v = 0; v < VERTEX_COUNT; v++) {
				close(vertex(out, v),
					referenceWorld(palette, instance, bind, indices, weights, expectedCoeffs, samples, v),
					"sample " + sampleIndex + " reproduction at vertex " + v);
			}
		}

		// --- an in-between pose is solved by the independent small matrix ---
		ModelInstance mixed = posed(rig, 22f, 14f);
		double[][][] sampleRels = new double[samples.length][][];
		for (int i = 0; i < samples.length; i++)
			sampleRels[i] = relativeRotations(samples[i]);
		double[][][] currentRels = new double[][][] {
			mulConj(quat(new Quaternion()), quat(elbowPose(22f))),
			mulConj(quat(new Quaternion()), quat(wristPose(14f)))};
		double[][] kernelMatrix = new double[samples.length][samples.length];
		double[] kernelVector = new double[samples.length];
		for (int i = 0; i < samples.length; i++) {
			kernelVector[i] = Math.exp(-BETA * referenceDistance(currentRels, sampleRels[i], driverWeights));
			for (int j = 0; j < samples.length; j++)
				kernelMatrix[i][j] = Math.exp(-BETA * referenceDistance(sampleRels[i], sampleRels[j], driverWeights));
		}
		double[] coeffs = referenceSolve(kernelMatrix, kernelVector);
		float[] mixedOut = new float[VERTEX_COUNT * 3];
		prepared.evaluate(mixed, partOf(mixed), bind, indices, weights, mixedOut);
		for (int v = 0; v < VERTEX_COUNT; v++) {
			close(vertex(mixedOut, v),
				referenceWorld(partOf(mixed).bones, mixed, bind, indices, weights, coeffs, samples, v),
				"in-between kernel solve at vertex " + v);
		}

		// --- negating every driver quaternion leaves the result unchanged. The local matrices are set straight from
		// the negated quaternions (bypassing slerp) so the deformer's own sign-invariant kernel is exercised. ---
		ModelInstance negated = new ModelInstance(rig.model);
		// Mark the drivers animated so calculateTransforms() keeps the manually set local matrices (the animation
		// controller sets this same flag when it writes localTransform directly).
		negated.getNode("elbow").isAnimated = true;
		negated.getNode("wrist").isAnimated = true;
		Quaternion negElbow = elbowPose(22f);
		negElbow.x = -negElbow.x; negElbow.y = -negElbow.y; negElbow.z = -negElbow.z; negElbow.w = -negElbow.w;
		Quaternion negWrist = wristPose(14f);
		negWrist.x = -negWrist.x; negWrist.y = -negWrist.y; negWrist.z = -negWrist.z; negWrist.w = -negWrist.w;
		negated.getNode("elbow").localTransform.set(new Vector3(1f, 0f, 0f), negElbow, new Vector3(1f, 1f, 1f));
		negated.getNode("wrist").localTransform.set(new Vector3(0f, 1f, 0f), negWrist, new Vector3(1f, 1f, 1f));
		negated.calculateTransforms();
		float[] negatedOut = new float[VERTEX_COUNT * 3];
		prepared.evaluate(negated, partOf(negated), bind, indices, weights, negatedOut);
		for (int v = 0; v < VERTEX_COUNT; v++)
			close(vertex(negatedOut, v), vertex(mixedOut, v), "sign-invariant pose at vertex " + v);

		// --- the real AnimationController leaves Node.rotation at its bind value; the local matrix carries the pose ---
		ModelInstance controllerInstance = new ModelInstance(rig.model);
		AnimationController animationController = new AnimationController(controllerInstance);
		animationController.setAnimation("pose", -1);
		animationController.update(0.5f);
		check(controllerInstance.getNode("elbow").rotation.isIdentity(), "elbow Node.rotation stays the bind value");
		check(controllerInstance.getNode("wrist").rotation.isIdentity(), "wrist Node.rotation stays the bind value");
		Quaternion matrixElbow = new Quaternion();
		controllerInstance.getNode("elbow").localTransform.getRotation(matrixElbow, true);
		check(Math.abs(matrixElbow.getAngle() - 45f) < 1e-3f, "animated local matrix carries the blended rotation");
		float[] controllerOut = new float[VERTEX_COUNT * 3];
		prepared.evaluate(controllerInstance, partOf(controllerInstance), bind, indices, weights, controllerOut);
		ModelInstance halfPose = posed(rig, 45f, 45f);
		float[] halfOut = new float[VERTEX_COUNT * 3];
		prepared.evaluate(halfPose, partOf(halfPose), bind, indices, weights, halfOut);
		for (int v = 0; v < VERTEX_COUNT; v++)
			close(vertex(controllerOut, v), vertex(halfOut, v), "AnimationController half-pose at vertex " + v);

		// --- non-identity inverse bind, multi-bone weights, rotated/scaled/translated instance transform: the
		// corrective delta is applied before the palette skinning ---
		ModelInstance dressed = posed(rig, 28f, 16f);
		dressed.transform.set(new Vector3(3f, -2f, 1.5f), new Quaternion().setFromAxis(0, 1, 0, 33f),
			new Vector3(1.7f, 1.7f, 1.7f));
		double[][][] dressedRels = new double[][][] {
			mulConj(quat(new Quaternion()), quat(elbowPose(28f))),
			mulConj(quat(new Quaternion()), quat(wristPose(16f)))};
		double[] dressedKernel = new double[samples.length];
		for (int i = 0; i < samples.length; i++)
			dressedKernel[i] = Math.exp(-BETA * referenceDistance(dressedRels, sampleRels[i], driverWeights));
		double[] dressedCoeffs = referenceSolve(kernelMatrix, dressedKernel);
		float[] dressedOut = new float[VERTEX_COUNT * 3];
		prepared.evaluate(dressed, partOf(dressed), bind, indices, weights, dressedOut);
		for (int v = 0; v < VERTEX_COUNT; v++) {
			close(vertex(dressedOut, v),
				referenceWorld(partOf(dressed).bones, dressed, bind, indices, weights, dressedCoeffs, samples, v),
				"pre-skin corrective under full instance transform at vertex " + v);
		}
		Prepared zeroPrepared = PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow", "wrist"},
			new Quaternion[] {new Quaternion(), new Quaternion()}, driverWeights, BETA,
			new Sample[] {new Sample(new Quaternion[] {new Quaternion(), new Quaternion()}, new int[0], new float[0])});
		float[] zeroOut = new float[VERTEX_COUNT * 3];
		zeroPrepared.evaluate(dressed, partOf(dressed), bind, indices, weights, zeroOut);
		check(vertex(dressedOut, CORRECTIVE_A).dst(vertex(zeroOut, CORRECTIVE_A)) > 1e-3f,
			"corrective delta survives skinning and the instance transform");

		// --- the prepared result keeps its own copy: mutating the caller's arrays changes nothing ---
		Sample[] working = buildSamples();
		Prepared independent = buildPrepared(working, driverWeights);
		working[1].deltas[0] = 999f;
		working[1].vertexIndices = new int[0];
		working[1].deltas = new float[0];
		ModelInstance flexInstance = posed(rig, 50f, 0f);
		float[] flexOut = new float[VERTEX_COUNT * 3];
		independent.evaluate(flexInstance, partOf(flexInstance), bind, indices, weights, flexOut);
		double[] flexCoeffs = new double[samples.length];
		flexCoeffs[1] = 1.0;
		for (int v = 0; v < VERTEX_COUNT; v++) {
			close(vertex(flexOut, v),
				referenceWorld(partOf(flexInstance).bones, flexInstance, bind, indices, weights, flexCoeffs, samples, v),
				"prepared result independent of later input mutation at vertex " + v);
		}

		// --- two instances interleave on one prepared result, including true in-place (bind == out) output ---
		ModelInstance instanceA = posed(rig, 30f, 10f);
		ModelInstance instanceB = posed(rig, 12f, 30f);
		float[] inPlace = bind.clone();
		prepared.evaluate(instanceA, partOf(instanceA), inPlace, indices, weights, inPlace);
		prepared.evaluate(instanceB, partOf(instanceB), bind, indices, weights, new float[VERTEX_COUNT * 3]);
		prepared.evaluate(instanceA, partOf(instanceA), inPlace, indices, weights, inPlace);
		float[] separateA = new float[VERTEX_COUNT * 3];
		prepared.evaluate(instanceA, partOf(instanceA), bind, indices, weights, separateA);
		for (int v = 0; v < VERTEX_COUNT; v++)
			close(vertex(inPlace, v), vertex(separateA, v), "interleaved in-place output at vertex " + v);
		float[] separateB = new float[VERTEX_COUNT * 3];
		prepared.evaluate(instanceB, partOf(instanceB), bind, indices, weights, separateB);
		float[] separateB2 = new float[VERTEX_COUNT * 3];
		prepared.evaluate(instanceB, partOf(instanceB), bind, indices, weights, separateB2);
		for (int v = 0; v < VERTEX_COUNT; v++)
			close(vertex(separateB, v), vertex(separateB2, v), "interleaved second instance at vertex " + v);

		// --- near-singular kernels (equivalent poses, including sign flips) are rejected ---
		Quaternion flipped = elbowPose(50f);
		Sample duplicatedSign = new Sample(new Quaternion[] {
			new Quaternion(-flipped.x, -flipped.y, -flipped.z, -flipped.w), new Quaternion()},
			new int[] {CORRECTIVE_A}, new float[] {0.01f, 0.01f, 0.01f});
		Sample[] duplicatedSamples = {samples[0], samples[1], samples[2], samples[3], duplicatedSign};
		expectIllegalArgument(() -> buildPrepared(duplicatedSamples, driverWeights),
			"sign-flipped duplicate pose must be rejected");

		// Tiny but (in float) distinct orientation: the pivoted kernel must fall below the 1e-10 pivot limit.
		Sample nearDuplicate = new Sample(new Quaternion[] {elbowPose(50f + 1e-5f), new Quaternion()},
			new int[] {CORRECTIVE_A}, new float[] {0.01f, 0.01f, 0.01f});
		Sample[] nearSamples = {samples[0], samples[1], samples[2], samples[3], nearDuplicate};
		expectIllegalArgument(() -> buildPrepared(nearSamples, driverWeights),
			"nearly coincident pose must make the kernel singular");

		// Two poses close to neutral and to each other, but their mutual distance still clears the equivalent-pose
		// guard (1e-9). With a small beta the pivoted LU element itself drops below 1e-10, exercising the pivot rule
		// rather than the duplicate-pose guard.
		double pivotBeta = 0.0001;
		Sample pivotA = new Sample(new Quaternion[] {elbowPose(0.02f)}, new int[] {1}, new float[] {0.01f, 0f, 0f});
		Sample pivotB = new Sample(new Quaternion[] {elbowPose(0.03f)}, new int[] {2}, new float[] {0f, 0.01f, 0f});
		Sample[] pivotSet = {
			new Sample(new Quaternion[] {new Quaternion()}, new int[0], new float[0]), pivotA, pivotB};
		expectIllegalArgument(
			() -> PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow"},
				new Quaternion[] {new Quaternion()}, new double[] {1.0}, pivotBeta, pivotSet),
			"sub-threshold elimination pivot must be rejected");

		// Comfortably separated poses with a normal beta stay well conditioned and must succeed.
		Sample separatedA = new Sample(new Quaternion[] {elbowPose(10f)}, new int[] {1}, new float[] {0.01f, 0f, 0f});
		Sample separatedB = new Sample(new Quaternion[] {elbowPose(20f)}, new int[] {2}, new float[] {0f, 0.01f, 0f});
		Sample[] separatedSet = {
			new Sample(new Quaternion[] {new Quaternion()}, new int[0], new float[0]), separatedA, separatedB};
		check(PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow"}, new Quaternion[] {new Quaternion()},
			new double[] {1.0}, BETA, separatedSet) != null, "well separated poses must be accepted");

		// --- an ordinary well-conditioned small sample set still succeeds ---
		check(buildPrepared(buildSamples(), driverWeights) != null,
			"ordinary well-conditioned samples must be accepted");

		// --- validation failures never touch the output ---
		float[] sentinel = new float[VERTEX_COUNT * 3];
		for (int i = 0; i < sentinel.length; i++)
			sentinel[i] = 7.25f + i;
		float[] guarded = sentinel.clone();
		ModelInstance guardInstance = new ModelInstance(rig.model);
		guardInstance.calculateTransforms();
		NodePart guardPart = partOf(guardInstance);

		int[] badIndices = boneIndices();
		badIndices[0] = 99;
		expectIllegalArgument(() -> prepared.evaluate(guardInstance, guardPart, bind, badIndices, weights, guarded),
			"out of range bone index must be rejected");
		assertSentinel(guarded, sentinel, "output unchanged after bad index");

		float[] badWeights = weights.clone();
		badWeights[0] = 0.4f;
		expectIllegalArgument(() -> prepared.evaluate(guardInstance, guardPart, bind, indices, badWeights, guarded),
			"weights not summing to one must be rejected");
		assertSentinel(guarded, sentinel, "output unchanged after bad weights");

		float[] negativeWeight = weights.clone();
		negativeWeight[6] = -0.1f;
		expectIllegalArgument(() -> prepared.evaluate(guardInstance, guardPart, bind, indices, negativeWeight, guarded),
			"negative bone weight must be rejected");
		assertSentinel(guarded, sentinel, "output unchanged after negative weight");

		float[] wrongLength = new float[VERTEX_COUNT * 3 - 1];
		expectIllegalArgument(() -> prepared.evaluate(guardInstance, guardPart, bind, indices, weights, wrongLength),
			"wrong output length must be rejected");
		assertSentinel(guarded, sentinel, "output unchanged after wrong length");

		NodePart foreignPart = partOf(new ModelInstance(rig.model));
		expectIllegalArgument(() -> prepared.evaluate(guardInstance, foreignPart, bind, indices, weights, guarded),
			"NodePart from another instance must be rejected");
		assertSentinel(guarded, sentinel, "output unchanged after foreign part");

		NodePart paletteLess = new NodePart();
		paletteLess.invBoneBindTransforms = new ArrayMap<>(Node.class, Matrix4.class);
		guardInstance.nodes.first().parts.add(paletteLess);
		expectIllegalArgument(() -> prepared.evaluate(guardInstance, paletteLess, bind, indices, weights, guarded),
			"missing palette must be rejected");
		assertSentinel(guarded, sentinel, "output unchanged after missing palette");

		float[] wrongBind = new float[VERTEX_COUNT * 3 + 3];
		expectIllegalArgument(() -> prepared.evaluate(guardInstance, guardPart, wrongBind, indices, weights, guarded),
			"wrong bind position length must be rejected");
		assertSentinel(guarded, sentinel, "output unchanged after wrong bind length");

		ModelInstance badPaletteInstance = new ModelInstance(rig.model);
		NodePart badPalettePart = partOf(badPaletteInstance);
		badPalettePart.bones[1].val[Matrix4.M03] = Float.NaN;
		expectIllegalArgument(
			() -> prepared.evaluate(badPaletteInstance, badPalettePart, bind, indices, weights, guarded),
			"non-finite palette must be rejected");
		assertSentinel(guarded, sentinel, "output unchanged after non-finite palette");

		// --- prepare-time validation ---
		Sample neutralTwoDrivers = new Sample(new Quaternion[] {new Quaternion(), new Quaternion()}, new int[0],
			new float[0]);
		expectIllegalArgument(() -> PoseSpaceDeformer.prepare(0, new String[] {"elbow"},
			new Quaternion[] {new Quaternion()}, new double[] {1.0}, BETA,
			new Sample[] {new Sample(new Quaternion[] {new Quaternion()}, new int[0], new float[0])}),
			"zero vertex count must be rejected");
		expectIllegalArgument(() -> PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow", "elbow"},
			new Quaternion[] {new Quaternion(), new Quaternion()}, new double[] {1.0, 1.0}, BETA,
			new Sample[] {neutralTwoDrivers}), "duplicate driver id must be rejected");
		expectIllegalArgument(() -> PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow", "wrist"},
			new Quaternion[] {new Quaternion(), new Quaternion(0f, 0f, 0f, 2f)}, new double[] {1.0, 1.0}, BETA,
			new Sample[] {neutralTwoDrivers}), "non-unit rotation must be rejected");
		expectIllegalArgument(() -> PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow"},
			new Quaternion[] {new Quaternion()}, new double[] {0.0}, BETA,
			new Sample[] {new Sample(new Quaternion[] {new Quaternion()}, new int[0], new float[0])}),
			"non-positive weight must be rejected");
		expectIllegalArgument(() -> PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow"},
			new Quaternion[] {new Quaternion()}, new double[] {1.0}, 0.0,
			new Sample[] {new Sample(new Quaternion[] {new Quaternion()}, new int[0], new float[0])}),
			"non-positive beta must be rejected");
		Sample noNeutral = new Sample(new Quaternion[] {elbowPose(10f)}, new int[0], new float[0]);
		Sample outOfRange = new Sample(new Quaternion[] {new Quaternion()}, new int[] {VERTEX_COUNT + 5},
			new float[] {0f, 0f, 0f});
		expectIllegalArgument(() -> PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow"},
			new Quaternion[] {new Quaternion()}, new double[] {1.0}, BETA, new Sample[] {noNeutral, outOfRange}),
			"out of range vertex and missing neutral sample must be rejected");
		Sample neutralOneDriver = new Sample(new Quaternion[] {new Quaternion()}, new int[0], new float[0]);
		Sample repeatedVertex = new Sample(new Quaternion[] {new Quaternion()}, new int[] {1, 1},
			new float[] {0f, 0f, 0f, 0f, 0f, 0f});
		expectIllegalArgument(() -> PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow"},
			new Quaternion[] {new Quaternion()}, new double[] {1.0}, BETA,
			new Sample[] {neutralOneDriver, repeatedVertex}), "repeated vertex index must be rejected");
		Sample nonFiniteDelta = new Sample(new Quaternion[] {new Quaternion()}, new int[] {1},
			new float[] {0f, Float.NaN, 0f});
		expectIllegalArgument(() -> PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow"},
			new Quaternion[] {new Quaternion()}, new double[] {1.0}, BETA,
			new Sample[] {neutralOneDriver, nonFiniteDelta}), "non-finite delta must be rejected");
		expectIllegalArgument(() -> PoseSpaceDeformer.prepare(VERTEX_COUNT, new String[] {"elbow"},
			new Quaternion[] {new Quaternion()}, new double[] {1.0}, BETA, new Sample[0]),
			"empty sample array must be rejected");

		// --- illegal driver local transforms are rejected before any output is written ---
		ModelInstance sheared = new ModelInstance(rig.model);
		sheared.getNode("elbow").localTransform.val[Matrix4.M01] = 0.35f;
		expectIllegalArgument(
			() -> prepared.evaluate(sheared, partOf(sheared), bind, indices, weights, guarded),
			"sheared driver local transform must be rejected");
		assertSentinel(guarded, sentinel, "output unchanged after sheared driver");
		ModelInstance nonUniform = new ModelInstance(rig.model);
		nonUniform.getNode("elbow").localTransform.scale(2f, 1f, 1f);
		expectIllegalArgument(
			() -> prepared.evaluate(nonUniform, partOf(nonUniform), bind, indices, weights, guarded),
			"non-uniform driver scale must be rejected");
		ModelInstance reflected = new ModelInstance(rig.model);
		reflected.getNode("elbow").localTransform.scale(-1f, -1f, -1f);
		expectIllegalArgument(
			() -> prepared.evaluate(reflected, partOf(reflected), bind, indices, weights, guarded),
			"reflected driver transform must be rejected");
		// An ordinary positive uniform scale is perfectly legal.
		ModelInstance uniformScaled = new ModelInstance(rig.model);
		uniformScaled.getNode("elbow").localTransform.scale(1.5f, 1.5f, 1.5f);
		prepared.evaluate(uniformScaled, partOf(uniformScaled), bind, indices, weights,
			new float[VERTEX_COUNT * 3]);

		// --- a missing driver node is rejected (palette-legal indices so the driver lookup is the failure) ---
		Model missingModel = new Model();
		Node loneRoot = new Node();
		loneRoot.id = "root";
		missingModel.nodes.add(loneRoot);
		NodePart lonePart = new NodePart();
		lonePart.invBoneBindTransforms = new ArrayMap<>(Node.class, Matrix4.class);
		lonePart.invBoneBindTransforms.put(loneRoot, new Matrix4());
		lonePart.bones = new Matrix4[] {new Matrix4()};
		loneRoot.parts.add(lonePart);
		ModelInstance missingInstance = new ModelInstance(missingModel);
		int[] zeroIndices = new int[VERTEX_COUNT * 4];
		float[] unitWeights = new float[VERTEX_COUNT * 4];
		for (int v = 0; v < VERTEX_COUNT; v++)
			unitWeights[v * 4] = 1f;
		expectIllegalArgument(
			() -> prepared.evaluate(missingInstance, partOf(missingInstance), bind, zeroIndices, unitWeights,
				new float[VERTEX_COUNT * 3]),
			"missing driver node must be rejected");

		System.out.println("GDX_CORRECTIVE_OK prepare kernel skinning sign-invariance interleaving validation");
	}

	interface ThrowingRunnable {
		void run ();
	}

	static void expectIllegalArgument (ThrowingRunnable runnable, String message) {
		try {
			runnable.run();
		} catch (IllegalArgumentException expected) {
			return;
		}
		throw new AssertionError(message);
	}

	static void assertSentinel (float[] actual, float[] sentinel, String message) {
		check(actual.length == sentinel.length, message + " (length)");
		for (int i = 0; i < sentinel.length; i++)
			check(actual[i] == sentinel[i], message + " at component " + i);
	}
}
