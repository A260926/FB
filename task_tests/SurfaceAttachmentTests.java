import com.jme3.anim.AnimClip;
import com.jme3.anim.AnimComposer;
import com.jme3.anim.AnimTrack;
import com.jme3.anim.Armature;
import com.jme3.anim.Joint;
import com.jme3.anim.SkinningControl;
import com.jme3.anim.TransformTrack;
import com.jme3.anim.tween.action.BlendableAction;
import com.jme3.bounding.BoundingBox;
import com.jme3.math.FastMath;
import com.jme3.math.Matrix4f;
import com.jme3.math.Quaternion;
import com.jme3.math.Transform;
import com.jme3.math.Vector3f;
import com.jme3.scene.Geometry;
import com.jme3.scene.Mesh;
import com.jme3.scene.Node;
import com.jme3.scene.Spatial;
import com.jme3.scene.VertexBuffer;
import com.jme3.scene.VertexBuffer.Format;
import com.jme3.scene.VertexBuffer.Type;
import com.jme3.scene.control.AbstractControl;
import com.jme3.scene.control.Control;
import com.jme3.scene.control.PostLogicalControl;
import com.jme3.scene.shape.Box;
import com.jme3.util.BufferUtils;
import com.jme3.util.clone.Cloner;

import java.nio.Buffer;
import java.nio.ByteBuffer;
import java.nio.FloatBuffer;
import java.nio.IntBuffer;
import java.nio.ShortBuffer;

/**
 * Tests for SkinningControl surface attachments and the PostLogicalControl
 * update phase.
 */
public final class SurfaceAttachmentTests {

    private static final float EPS = 1e-4f;

    private SurfaceAttachmentTests() {
    }

    // =====================================================================
    // Shared fixtures
    // =====================================================================

    /**
     * An armature with four root joints:
     * j0 rotates around Z from 0 to 90 degrees over t in [0,1],
     * j1 translates from x=2 to x=3 over t in [0,1],
     * j2 and j3 are static.
     */
    private static Armature buildArmature() {
        Joint j0 = new Joint("j0");
        j0.setLocalTranslation(new Vector3f(0, 0, 0));

        Joint j1 = new Joint("j1");
        j1.setLocalTranslation(new Vector3f(2, 0, 0));

        Joint j2 = new Joint("j2");
        j2.setLocalTranslation(new Vector3f(0, 2, 0));

        Joint j3 = new Joint("j3");
        j3.setLocalTranslation(new Vector3f(0, 0, 2));

        Armature armature = new Armature(new Joint[]{j0, j1, j2, j3});
        armature.saveBindPose();
        return armature;
    }

    private static AnimClip buildClip(Joint j0, Joint j1) {
        // A two-second clip: at time t the rotation is (t/2)*90 degrees and the
        // translation is 2 + t/2, so t=1.0 gives exactly 45 degrees and +0.5.
        float[] times = {0f, 2f};

        TransformTrack rotate = new TransformTrack(j0, times,
                new Vector3f[]{new Vector3f(0, 0, 0), new Vector3f(0, 0, 0)},
                new Quaternion[]{
                        new Quaternion().fromAngleAxis(0f, Vector3f.UNIT_Z),
                        new Quaternion().fromAngleAxis(FastMath.HALF_PI, Vector3f.UNIT_Z)
                },
                null);

        TransformTrack translate = new TransformTrack(j1, times,
                new Vector3f[]{new Vector3f(2, 0, 0), new Vector3f(3, 0, 0)},
                null, null);

        AnimClip clip = new AnimClip("motion");
        clip.setTracks(new AnimTrack[]{rotate, translate});
        return clip;
    }

    /**
     * Builds a mesh with three vertices and multi-bone weights.
     * Vertex 0 uses three bones, vertex 1 uses four, vertex 2 uses one.
     */
    private static Mesh buildMesh(Format boneIndexFormat) {
        Mesh mesh = new Mesh();
        mesh.setBuffer(Type.Position, 3, new float[]{
                1, 0, 0,
                0, 1, 0,
                2, 0, 1
        });
        mesh.setBuffer(Type.Normal, 3, new float[]{
                0, 0, 1,
                0, 0, 1,
                0, 0, 1
        });
        mesh.setBuffer(Type.Index, 3, new short[]{0, 1, 2});

        float[] weights = {
                // vertex 0
                0.4f, 0.4f, 0.2f, 0f,
                // vertex 1
                0.35f, 0.25f, 0.2f, 0.2f,
                // vertex 2
                1f, 0f, 0f, 0f
        };
        byte[] bytes = {0, 1, 2, 0, 0, 1, 2, 3, 1, 0, 0, 0};
        short[] shorts = {0, 1, 2, 0, 0, 1, 2, 3, 1, 0, 0, 0};
        int[] ints = {0, 1, 2, 0, 0, 1, 2, 3, 1, 0, 0, 0};

        mesh.setBuffer(Type.BoneWeight, 4, BufferUtils.createFloatBuffer(weights));
        if (boneIndexFormat == Format.UnsignedByte || boneIndexFormat == Format.Byte) {
            mesh.setBuffer(Type.BoneIndex, 4, Format.UnsignedByte,
                    ByteBuffer.wrap(bytes));
        } else if (boneIndexFormat == Format.UnsignedShort
                || boneIndexFormat == Format.Short) {
            mesh.setBuffer(Type.BoneIndex, 4, Format.UnsignedShort,
                    ShortBuffer.wrap(shorts));
        } else {
            mesh.setBuffer(Type.BoneIndex, 4, Format.UnsignedInt,
                    IntBuffer.wrap(ints));
        }

        mesh.setMaxNumWeights(4);
        mesh.generateBindPose();
        mesh.updateBound();
        return mesh;
    }

    /**
     * A two-bone mesh matching {@link #buildNonUnitArmature()}: three vertices
     * weighted between the root and child joints.
     */
    private static Mesh buildTwoJointMesh() {
        Mesh mesh = new Mesh();
        mesh.setBuffer(Type.Position, 3, BIND_TWO);
        mesh.setBuffer(Type.Normal, 3, new float[]{0, 0, 1, 0, 0, 1, 0, 0, 1});
        mesh.setBuffer(Type.Index, 3, new short[]{0, 1, 2});
        mesh.setBuffer(Type.BoneIndex, 4, ByteBuffer.wrap(new byte[]{
                0, 1, 0, 0,
                1, 0, 0, 0,
                0, 1, 0, 0
        }));
        mesh.setBuffer(Type.BoneWeight, 4, BufferUtils.createFloatBuffer(BONE_WEIGHT_TWO));
        mesh.setMaxNumWeights(2);
        mesh.generateBindPose();
        mesh.updateBound();
        return mesh;
    }

    private static final float[] BIND_TWO = {
            0.5f, 0.5f, 0.5f,
            1.5f, 0.75f, 0.25f,
            0.25f, 1.25f, 1f
    };
    private static final int[] BONE_INDEX_TWO = {
            0, 1, 0, 0,
            1, 0, 0, 0,
            0, 1, 0, 0
    };
    private static final float[] BONE_WEIGHT_TWO = {
            0.25f, 0.75f, 0f, 0f,
            0.6f, 0.4f, 0f, 0f,
            0.5f, 0.5f, 0f, 0f
    };

    private static final class Scene {
        Node root = new Node("actor");
        Node holder = null;
        Geometry geometry;
        Armature armature;
        AnimComposer composer;
        SkinningControl skin;
        Joint j0;
        Joint j1;
        Joint j2;
        Joint j3;
    }

    private static Scene buildScene(Format boneIndexFormat, boolean hardwarePreferred) {
        Scene scene = new Scene();
        scene.armature = buildArmature();
        scene.j0 = scene.armature.getJoint("j0");
        scene.j1 = scene.armature.getJoint("j1");
        scene.j2 = scene.armature.getJoint("j2");
        scene.j3 = scene.armature.getJoint("j3");

        scene.geometry = new Geometry("skin", buildMesh(boneIndexFormat));
        scene.root.attachChild(scene.geometry);

        scene.composer = new AnimComposer();
        AnimClip clip = buildClip(scene.j0, scene.j1);
        scene.composer.addAnimClip(clip);
        scene.root.addControl(scene.composer);
        scene.composer.setCurrentAction("motion");
        ((BlendableAction) scene.composer.getCurrentAction()).setTransitionLength(0);
        scene.composer.setTime(0);

        scene.skin = new SkinningControl(scene.armature);
        scene.skin.setHardwareSkinningPreferred(hardwarePreferred);
        scene.root.addControl(scene.skin);
        return scene;
    }

    /**
     * Recomputes the expected deformed vertex positions with an independent
     * blend of the armature palette. This mirrors the production formula but
     * shares no code with it.
     */
    private static Vector3f[] expectedVertices(Scene scene, float[] bind, int[] boneIndex,
            float[] weight, int maxWeights) {
        Matrix4f[] palette = scene.armature.computeSkinningMatrices();
        int vertexCount = bind.length / 3;
        Vector3f[] result = new Vector3f[vertexCount];
        for (int v = 0; v < vertexCount; v++) {
            float x = bind[v * 3];
            float y = bind[v * 3 + 1];
            float z = bind[v * 3 + 2];
            float rx = 0, ry = 0, rz = 0;
            for (int w = 0; w < maxWeights; w++) {
                int slot = v * 4 + w;
                float wt = weight[slot];
                Matrix4f m = palette[boneIndex[slot]];
                rx += (m.m00 * x + m.m01 * y + m.m02 * z + m.m03) * wt;
                ry += (m.m10 * x + m.m11 * y + m.m12 * z + m.m13) * wt;
                rz += (m.m20 * x + m.m21 * y + m.m22 * z + m.m23) * wt;
            }
            result[v] = new Vector3f(rx, ry, rz);
        }
        return result;
    }

    private static final float[] BIND = {
            1, 0, 0,
            0, 1, 0,
            2, 0, 1
    };
    private static final int[] BONE_INDEX = {
            0, 1, 2, 0,
            0, 1, 2, 3,
            1, 0, 0, 0
    };
    private static final float[] BONE_WEIGHT = {
            0.4f, 0.4f, 0.2f, 0f,
            0.35f, 0.25f, 0.2f, 0.2f,
            1f, 0f, 0f, 0f
    };

    private static Vector3f barycentric(float a, float b, float c) {
        return new Vector3f(a, b, c);
    }

    private static void checkNear(TaskChecks c, Vector3f actual, Vector3f expected, String message) {
        c.check(actual.distance(expected) <= EPS, message + " expected=" + expected + " actual=" + actual);
    }

    private static void checkNear(TaskChecks c, float actual, float expected, String message) {
        c.check(Math.abs(actual - expected) <= EPS, message + " expected=" + expected + " actual=" + actual);
    }

    /** Extracts the X/Y/Z basis vectors of a rotation. */
    private static Vector3f[] axes(Quaternion rotation) {
        Vector3f[] out = new Vector3f[3];
        for (int i = 0; i < 3; i++) {
            out[i] = rotation.getRotationColumn(i, new Vector3f()).normalizeLocal();
        }
        return out;
    }

    private static Vector3f frameX(Vector3f a, Vector3f b) {
        return b.subtract(a).normalizeLocal();
    }

    private static Vector3f frameZ(Vector3f a, Vector3f b, Vector3f c) {
        Vector3f e1 = b.subtract(a);
        Vector3f e2 = c.subtract(a);
        return e1.cross(e2).normalizeLocal();
    }

    private static Vector3f frameY(Vector3f x, Vector3f z) {
        return z.cross(x).normalizeLocal();
    }

    private static Vector3f blend(Vector3f a, Vector3f b, Vector3f c, Vector3f bary) {
        return new Vector3f(
                a.x * bary.x + b.x * bary.y + c.x * bary.z,
                a.y * bary.x + b.y * bary.y + c.y * bary.z,
                a.z * bary.x + b.z * bary.y + c.z * bary.z);
    }

    private static void updateFrame(Node root) {
        root.updateLogicalState(0f);
        root.updateGeometricState();
    }

    private static int countChildren(Node node) {
        return node.getChildren().size();
    }

    private static int getBufferLimit(Buffer buffer) {
        return buffer.limit();
    }

    private static int getBufferCapacity(Buffer buffer) {
        return buffer.capacity();
    }

    // =====================================================================
    // deformation
    // =====================================================================

    public static void deformation(TaskChecks c) {
        Scene scene = buildScene(Format.UnsignedByte, false);

        // Advance to t=1.0 of a 2-second clip: j0 has rotated 45 degrees and
        // j1 has moved +0.5 in x.
        scene.composer.setCurrentAction("motion");
        scene.composer.setTime(1.0);
        ((BlendableAction) scene.composer.getCurrentAction()).setTransitionLength(0);
        updateFrame(scene.root);

        Vector3f[] skinned = expectedVertices(scene, BIND, BONE_INDEX, BONE_WEIGHT, 4);

        // Attachment at barycentric (1/3,1/3,1/3), offset = translation only.
        Transform offset = new Transform(new Vector3f(0.1f, 0.2f, 0.3f),
                Quaternion.IDENTITY, Vector3f.UNIT_XYZ);
        Node attachment = scene.skin.addSurfaceAttachment("badge", scene.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), offset);

        // Attachments placed at the three vertex barycentrics independently
        // reproduce each skinned point (this exercises the shared skinning
        // evaluation without needing a software-skinning render pass).
        Node vertexA = scene.skin.addSurfaceAttachment("va", scene.geometry, 0,
                barycentric(1f, 0f, 0f), new Transform());
        Node vertexB = scene.skin.addSurfaceAttachment("vb", scene.geometry, 0,
                barycentric(0f, 1f, 0f), new Transform());
        Node vertexC = scene.skin.addSurfaceAttachment("vc", scene.geometry, 0,
                barycentric(0f, 0f, 1f), new Transform());
        checkNear(c, vertexA.getLocalTranslation(), skinned[0], "independent skinning of vertex 0");
        checkNear(c, vertexB.getLocalTranslation(), skinned[1], "independent skinning of vertex 1");
        checkNear(c, vertexC.getLocalTranslation(), skinned[2], "independent skinning of vertex 2");

        // Hard-coded expectation at t=0.5 (j0 rotated 45 degrees about Z, j1
        // translated +0.5 in X). These constants were derived by hand from the
        // bind-pose positions and bone weights, not from the production code.
        checkNear(c, skinned[0], new Vector3f(
                        0.4f * 0.70710678f + 0.4f * 1.5f + 0.2f,
                        0.4f * 0.70710678f, 0f),
                "hand-computed vertex 0");
        checkNear(c, skinned[1], new Vector3f(
                        -0.35f * 0.70710678f + 0.25f * 0.5f,
                        0.35f * 0.70710678f + 0.25f + 0.2f + 0.2f, 0f),
                "hand-computed vertex 1");
        checkNear(c, skinned[2], new Vector3f(2.5f, 0f, 1f),
                "hand-computed vertex 2");

        // Three independent skinned points.
        Vector3f a = skinned[0];
        Vector3f b = skinned[1];
        Vector3f d = skinned[2];

        Vector3f centroid = blend(a, b, d, barycentric(1f / 3f, 1f / 3f, 1f / 3f));
        Vector3f expectedX = frameX(a, b);
        Vector3f expectedZ = frameZ(a, b, d);
        Vector3f expectedY = frameY(expectedX, expectedZ);

        Vector3f localTranslation = attachment.getLocalTranslation().clone();
        // The local matrix is frame * offset, so the frame rotation also
        // rotates the offset's translation.
        Quaternion frameRotation = new Quaternion().fromAxes(expectedX, expectedY, expectedZ);
        Vector3f expectedTranslation = centroid.add(
                frameRotation.mult(offset.getTranslation(), null));
        checkNear(c, localTranslation, expectedTranslation, "attachment barycentric position");

        Quaternion localRotation = attachment.getLocalRotation().clone();
        Vector3f[] actualAxes = axes(localRotation);
        checkNear(c, actualAxes[0], expectedX, "attachment x axis");
        checkNear(c, actualAxes[1], expectedY, "attachment y axis");
        checkNear(c, actualAxes[2], expectedZ, "attachment z axis");

        // Child content: world bounding box should follow the attachment.
        Geometry child = new Geometry("badgeMesh", new Box(0.05f, 0.05f, 0.05f));
        child.setLocalTranslation(0f, 0.1f, 0f);
        attachment.attachChild(child);
        updateFrame(scene.root);

        BoundingBox bound = (BoundingBox) child.getWorldBound();
        c.check(bound != null, "badge child has a world bound");
        Vector3f expectedChildCenter = attachment.getWorldTranslation().add(
                attachment.getWorldRotation().mult(new Vector3f(0f, 0.1f, 0f), null));
        checkNear(c, bound.getCenter(), expectedChildCenter, "badge child world bound center");
        // The axis-aligned world bound of a rotated unit box has extents given
        // by the absolute row sums of its rotation matrix.
        Vector3f[] worldAxes = axes(attachment.getWorldRotation());
        float expectedXExtent = 0.05f * (Math.abs(worldAxes[0].x)
                + Math.abs(worldAxes[1].x) + Math.abs(worldAxes[2].x));
        checkNear(c, bound.getXExtent(), expectedXExtent,
                "badge child world bound x extent");

        // Move the animation forward and verify the attachment tracks.
        scene.composer.setTime(1.5);
        updateFrame(scene.root);
        Vector3f[] skinned2 = expectedVertices(scene, BIND, BONE_INDEX, BONE_WEIGHT, 4);
        Vector3f a2 = skinned2[0];
        Vector3f b2 = skinned2[1];
        Vector3f d2 = skinned2[2];
        Vector3f centroid2 = blend(a2, b2, d2, barycentric(1f / 3f, 1f / 3f, 1f / 3f));
        Quaternion frame2 = new Quaternion().fromAxes(
                frameX(a2, b2), frameY(frameX(a2, b2), frameZ(a2, b2, d2)),
                frameZ(a2, b2, d2));
        checkNear(c, attachment.getLocalTranslation().clone(),
                centroid2.add(frame2.mult(offset.getTranslation(), null)),
                "attachment follows animation");

        // A vertex fully weighted to a single translating bone verifies
        // independence. At t=1.5 the translation offset is +0.75 in x.
        checkNear(c, d2, new Vector3f(2.75f, 0f, 1f), "hand-computed vertex 2 at t=1.5");
        // Its z coordinate is unaffected by the rotation of the other bone.
        checkNear(c, b2.z, 0f, "vertex 1 stays in the z=0 plane");

        // The retained software-skinning path produces the same positions.
        try {
            java.lang.reflect.Method reset = SkinningControl.class.getDeclaredMethod("resetToBind");
            reset.setAccessible(true);
            reset.invoke(scene.skin);
            java.lang.reflect.Method deform = SkinningControl.class.getDeclaredMethod(
                    "applySoftwareSkinning", Mesh.class, Matrix4f[].class);
            deform.setAccessible(true);
            deform.invoke(scene.skin, scene.geometry.getMesh(),
                    scene.armature.computeSkinningMatrices());
            FloatBuffer out = (FloatBuffer) scene.geometry.getMesh()
                    .getBuffer(Type.Position).getData();
            for (int i = 0; i < 3; i++) {
                Vector3f actual = new Vector3f(out.get(i * 3), out.get(i * 3 + 1),
                        out.get(i * 3 + 2));
                checkNear(c, actual, skinned2[i], "software skinning vertex " + i);
            }
        } catch (ReflectiveOperationException ex) {
            throw new AssertionError("software skinning reflection failed", ex);
        }
    }

    // =====================================================================
    // frames
    // =====================================================================

    public static void frames(TaskChecks c) {
        // 1) Non-uniform scale and reflection on the geometry path must be
        //    applied to the vertices before the frame is derived.
        Scene scaled = buildScene(Format.UnsignedShort, true);
        scaled.holder = new Node("holder");
        scaled.root.detachChild(scaled.geometry);
        scaled.holder.attachChild(scaled.geometry);
        scaled.root.attachChild(scaled.holder);
        scaled.holder.setLocalScale(new Vector3f(2f, 0.5f, 1f));
        scaled.holder.setLocalTranslation(new Vector3f(0.25f, -0.5f, 0.75f));
        scaled.composer.setTime(0.25);
        updateFrame(scaled.root);

        Node scaledAttachment = scaled.skin.addSurfaceAttachment("scaled", scaled.geometry,
                0, barycentric(0.2f, 0.3f, 0.5f), new Transform());
        updateFrame(scaled.root);

        // Recompute the expected frame by transforming the skinned points
        // through the holder's local matrix.
        Vector3f[] baseSkinned = expectedVertices(scaled, BIND, BONE_INDEX, BONE_WEIGHT, 4);
        Matrix4f holderMatrix = scaled.holder.getLocalTransform().toTransformMatrix();
        Vector3f ta = holderMatrix.mult(baseSkinned[0], new Vector3f());
        Vector3f tb = holderMatrix.mult(baseSkinned[1], new Vector3f());
        Vector3f td = holderMatrix.mult(baseSkinned[2], new Vector3f());
        Vector3f sc = blend(ta, tb, td, barycentric(0.2f, 0.3f, 0.5f));
        checkNear(c, scaledAttachment.getLocalTranslation(), sc, "scaled attachment position");
        Vector3f[] scaledAxes = axes(scaledAttachment.getLocalRotation());
        checkNear(c, scaledAxes[0], frameX(ta, tb), "scaled attachment x axis");
        checkNear(c, scaledAxes[2], frameZ(ta, tb, td), "scaled attachment z axis");

        // Reflection (negative scale on one axis) must also be applied to the
        // vertices before the frame is built, and the resulting frame must
        // remain right-handed (z from the winding, y = z x x).
        Scene reflected = buildScene(Format.UnsignedByte, false);
        Node mirror = new Node("mirror");
        reflected.root.detachChild(reflected.geometry);
        mirror.attachChild(reflected.geometry);
        reflected.root.attachChild(mirror);
        mirror.setLocalScale(new Vector3f(-1f, 1f, 1f));
        reflected.composer.setTime(0.6);
        updateFrame(reflected.root);
        Node reflectedAttachment = reflected.skin.addSurfaceAttachment("mirror",
                reflected.geometry, 0, barycentric(0.25f, 0.5f, 0.25f), new Transform());
        updateFrame(reflected.root);

        Vector3f[] rSkinned = expectedVertices(reflected, BIND, BONE_INDEX, BONE_WEIGHT, 4);
        Matrix4f mirrorMatrix = mirror.getLocalTransform().toTransformMatrix();
        Vector3f ra = mirrorMatrix.mult(rSkinned[0], new Vector3f());
        Vector3f rb = mirrorMatrix.mult(rSkinned[1], new Vector3f());
        Vector3f rc = mirrorMatrix.mult(rSkinned[2], new Vector3f());
        Vector3f rX = frameX(ra, rb);
        Vector3f rZ = frameZ(ra, rb, rc);
        Vector3f rY = frameY(rX, rZ);
        Vector3f[] rAxes = axes(reflectedAttachment.getLocalRotation());
        checkNear(c, rAxes[0], rX, "reflected attachment x axis");
        checkNear(c, rAxes[1], rY, "reflected attachment y axis");
        checkNear(c, rAxes[2], rZ, "reflected attachment z axis");
        // Right-handed and unit-length even under reflection.
        checkNear(c, rX.cross(rY).distance(rZ), 0f, "reflected frame is right-handed");
        checkNear(c, rAxes[0].length(), 1f, "reflected x axis is unit length");
        checkNear(c, rAxes[1].length(), 1f, "reflected y axis is unit length");

        // 2) All three bone-index formats produce the same attachment.
        for (Format format : new Format[]{Format.UnsignedByte, Format.UnsignedShort,
                Format.UnsignedInt}) {
            Scene fmt = buildScene(format, true);
            fmt.composer.setTime(0.5);
            updateFrame(fmt.root);
            Node node = fmt.skin.addSurfaceAttachment("same", fmt.geometry, 0,
                    barycentric(0.25f, 0.25f, 0.5f), new Transform());
            Vector3f expected = blend(expectedVertices(fmt, BIND, BONE_INDEX, BONE_WEIGHT, 4)[0],
                    expectedVertices(fmt, BIND, BONE_INDEX, BONE_WEIGHT, 4)[1],
                    expectedVertices(fmt, BIND, BONE_INDEX, BONE_WEIGHT, 4)[2],
                    barycentric(0.25f, 0.25f, 0.5f));
            checkNear(c, node.getLocalTranslation(), expected,
                    "bone index format " + format + " position");
        }

        // 3) Offset rotation and positive uniform scale are applied on top of
        //    the frame (frame * offset), not the other way round.
        Scene offsets = buildScene(Format.UnsignedByte, false);
        offsets.composer.setTime(0.4);
        updateFrame(offsets.root);
        Quaternion q = new Quaternion().fromAngleAxis(0.7f, Vector3f.UNIT_Y);
        Transform offset = new Transform(new Vector3f(0.5f, -0.25f, 0.125f), q,
                new Vector3f(2f, 2f, 2f));
        Node offsetNode = offsets.skin.addSurfaceAttachment("offset", offsets.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), offset);
        updateFrame(offsets.root);

        Vector3f[] s = expectedVertices(offsets, BIND, BONE_INDEX, BONE_WEIGHT, 4);
        Vector3f cen = blend(s[0], s[1], s[2], barycentric(1f / 3f, 1f / 3f, 1f / 3f));
        Quaternion frame = offsetNode.getLocalRotation().mult(q.inverse());
        Vector3f expectedTranslation = cen.add(frame.mult(offset.getTranslation(), null));
        checkNear(c, offsetNode.getLocalTranslation(), expectedTranslation,
                "offset translation uses frame rotation only");
        checkNear(c, offsetNode.getLocalScale().x, 2f, "offset uniform scale");
        Quaternion expectedRotation = frame.mult(q);
        c.check(sameRotation(offsetNode.getLocalRotation(), expectedRotation),
                "offset rotation composes on the right");

        // 4) Non-unit inverse bind matrices and non-commuting parent rotations.
        Armature armature = buildNonUnitArmature();
        Mesh mesh = buildTwoJointMesh();
        Geometry geometry = new Geometry("skin", mesh);
        Node root = new Node("actor");
        root.attachChild(geometry);
        AnimComposer composer = new AnimComposer();
        AnimClip clip = buildNonUnitClip(armature);
        composer.addAnimClip(clip);
        root.addControl(composer);
        composer.setCurrentAction("rot");
        ((BlendableAction) composer.getCurrentAction()).setTransitionLength(0);
        composer.setTime(0.5);
        SkinningControl skin = new SkinningControl(armature);
        skin.setHardwareSkinningPreferred(false);
        root.addControl(skin);
        updateFrame(root);

        Node attachment = skin.addSurfaceAttachment("rig", geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform());
        updateFrame(root);
        c.check(attachment.getLocalTranslation().length() > 0f,
                "non-unit inverse bind produces a real frame");
        c.check(Float.isFinite(attachment.getLocalTranslation().x),
                "non-unit inverse bind frame is finite");

        // The position must equal the blend of the skinned vertices, with no
        // extra bone model matrix multiplied in. Compute those vertices with an
        // independent blend of the armature palette.
        Matrix4f[] palette = armature.computeSkinningMatrices();
        Vector3f[] nonUnit = new Vector3f[3];
        for (int v = 0; v < 3; v++) {
            float x = BIND_TWO[v * 3];
            float y = BIND_TWO[v * 3 + 1];
            float z = BIND_TWO[v * 3 + 2];
            float rx = 0, ry = 0, rz = 0;
            for (int w = 0; w < 2; w++) {
                int slot = v * 4 + w;
                float wt = BONE_WEIGHT_TWO[slot];
                Matrix4f m = palette[BONE_INDEX_TWO[slot]];
                rx += (m.m00 * x + m.m01 * y + m.m02 * z + m.m03) * wt;
                ry += (m.m10 * x + m.m11 * y + m.m12 * z + m.m13) * wt;
                rz += (m.m20 * x + m.m21 * y + m.m22 * z + m.m23) * wt;
            }
            nonUnit[v] = new Vector3f(rx, ry, rz);
        }
        checkNear(c, attachment.getLocalTranslation(),
                blend(nonUnit[0], nonUnit[1], nonUnit[2],
                        barycentric(1f / 3f, 1f / 3f, 1f / 3f)),
                "non-unit inverse bind position matches the palette blend");
        Quaternion nonUnitFrame = new Quaternion().fromAxes(
                frameX(nonUnit[0], nonUnit[1]),
                frameY(frameX(nonUnit[0], nonUnit[1]),
                        frameZ(nonUnit[0], nonUnit[1], nonUnit[2])),
                frameZ(nonUnit[0], nonUnit[1], nonUnit[2]));
        c.check(sameRotation(attachment.getLocalRotation(), nonUnitFrame),
                "non-commuting rotations produce the edge-derived frame");

        // 5) A triangle that is already degenerate in the bind pose is
        //    rejected before any node is created.
        Scene bindDegen = buildScene(Format.UnsignedByte, false);
        Mesh collapsed = buildCollapsedMesh();
        Geometry collapsedGeo = new Geometry("collapsed", collapsed);
        bindDegen.root.detachChild(bindDegen.geometry);
        bindDegen.root.attachChild(collapsedGeo);
        bindDegen.root.removeControl(SkinningControl.class);
        SkinningControl collapsedSkin = new SkinningControl(bindDegen.armature);
        collapsedSkin.setHardwareSkinningPreferred(false);
        bindDegen.root.addControl(collapsedSkin);
        updateFrame(bindDegen.root);

        boolean rejected = false;
        try {
            collapsedSkin.addSurfaceAttachment("collapsed", collapsedGeo, 0,
                    barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform());
        } catch (IllegalArgumentException expected) {
            rejected = true;
        }
        c.check(rejected, "bind-pose degenerate triangle rejected");

        // 6) A triangle that is valid in the bind pose but already degenerate
        //    in the current deformed pose at creation time falls back to the
        //    bind-pose frame (through the current ancestor chain) rather than
        //    rejecting the attachment.
        Scene first = buildScene(Format.UnsignedByte, false);
        Mesh squeezeMesh = buildSqueezeMesh();
        Geometry squeezeGeo = new Geometry("squeeze", squeezeMesh);
        first.root.detachChild(first.geometry);
        first.root.attachChild(squeezeGeo);
        first.root.removeControl(SkinningControl.class);
        Armature firstArmature = buildSqueezeArmature();
        first.armature = firstArmature;
        SkinningControl firstSkin = new SkinningControl(firstArmature);
        firstSkin.setHardwareSkinningPreferred(false);
        first.root.addControl(firstSkin);

        AnimComposer firstComposer = new AnimComposer();
        firstComposer.addAnimClip(buildSqueezeClip(firstArmature));
        first.root.removeControl(AnimComposer.class);
        first.root.addControl(firstComposer);
        firstComposer.setCurrentAction("squeeze");
        ((BlendableAction) firstComposer.getCurrentAction()).setTransitionLength(0);
        // t=1 collapses the triangle entirely.
        firstComposer.setTime(1f);
        updateFrame(first.root);

        Node firstNode = firstSkin.addSurfaceAttachment("first", squeezeGeo, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform());
        c.check(firstNode != null, "degenerate-at-creation attachment is accepted");

        // The fallback orientation must be the bind-pose frame (identity here,
        // since the bind triangle is axis-aligned in the source space).
        Vector3f[] bindPts = bindTriangle(squeezeGeo.getMesh());
        Quaternion bindFrame = new Quaternion().fromAxes(
                frameX(bindPts[0], bindPts[1]),
                frameY(frameX(bindPts[0], bindPts[1]),
                        frameZ(bindPts[0], bindPts[1], bindPts[2])),
                frameZ(bindPts[0], bindPts[1], bindPts[2]));
        c.check(sameRotation(firstNode.getLocalRotation(), bindFrame),
                "first-frame degeneracy falls back to the bind-pose frame");

        // Buffer cursors must not be modified by add or by a logical update.
        Scene cursor = buildScene(Format.UnsignedInt, true);
        cursor.composer.setTime(0.3);
        updateFrame(cursor.root);
        Mesh meshC = cursor.geometry.getMesh();
        Buffer posData = meshC.getBuffer(Type.Position).getData();
        Buffer bindData = meshC.getBuffer(Type.BindPosePosition).getData();
        Buffer weightData = meshC.getBuffer(Type.BoneWeight).getData();
        Buffer indexData = meshC.getBuffer(Type.BoneIndex).getData();
        int posLimit = getBufferLimit(posData), posCap = getBufferCapacity(posData);
        int posPosition = posData.position();
        int bindLimit = getBufferLimit(bindData), bindCap = getBufferCapacity(bindData);
        int bindPosition = bindData.position();
        int weightLimit = getBufferLimit(weightData), weightCap = getBufferCapacity(weightData);
        int weightPosition = weightData.position();
        int indexLimit = getBufferLimit(indexData), indexCap = getBufferCapacity(indexData);
        int indexPosition = indexData.position();

        Node cursorNode = cursor.skin.addSurfaceAttachment("cursor", cursor.geometry, 0,
                barycentric(0.5f, 0.25f, 0.25f), new Transform());
        updateFrame(cursor.root);

        c.check(getBufferLimit(posData) == posLimit && getBufferCapacity(posData) == posCap,
                "position buffer limit/capacity preserved");
        c.check(getBufferLimit(bindData) == bindLimit && getBufferCapacity(bindData) == bindCap,
                "bind-pose buffer limit/capacity preserved");
        c.check(getBufferLimit(weightData) == weightLimit
                && getBufferCapacity(weightData) == weightCap,
                "bone weight buffer limit/capacity preserved");
        c.check(getBufferLimit(indexData) == indexLimit
                && getBufferCapacity(indexData) == indexCap,
                "bone index buffer limit/capacity preserved");
        c.check(posData.position() == posPosition, "position buffer cursor preserved");
        c.check(bindData.position() == bindPosition, "bind-pose buffer cursor preserved");
        c.check(weightData.position() == weightPosition, "bone weight buffer cursor preserved");
        c.check(indexData.position() == indexPosition, "bone index buffer cursor preserved");
        c.check(cursorNode != null, "cursor attachment exists");

        // The same must hold for direct buffers: verify a direct BoneIndex
        // buffer's cursor is left alone too.
        Scene direct = buildScene(Format.UnsignedByte, false);
        direct.composer.setTime(0.3);
        Mesh directMesh = direct.geometry.getMesh();
        ByteBuffer heap = ByteBuffer.wrap(new byte[]{0, 1, 2, 0, 0, 1, 2, 3, 1, 0, 0, 0});
        ByteBuffer directBuffer = BufferUtils.createByteBuffer(12);
        directBuffer.put(heap);
        directBuffer.clear();
        directMesh.clearBuffer(Type.BoneIndex);
        directMesh.setBuffer(Type.BoneIndex, 4, directBuffer);
        updateFrame(direct.root);
        int directLimit = directBuffer.limit();
        directBuffer.position(3);
        int directPosition = directBuffer.position();
        Node directNode = direct.skin.addSurfaceAttachment("direct", direct.geometry, 0,
                barycentric(0.5f, 0.25f, 0.25f), new Transform());
        updateFrame(direct.root);
        c.check(directBuffer.limit() == directLimit, "direct bone index limit preserved");
        c.check(directBuffer.position() == directPosition,
                "direct bone index cursor preserved");
        c.check(directNode != null, "direct-buffer attachment created");
    }

    private static boolean sameRotation(Quaternion a, Quaternion b) {
        Quaternion na = a.clone().normalizeLocal();
        Quaternion nb = b.clone().normalizeLocal();
        return Math.abs(na.dot(nb)) >= 1f - 1e-4f;
    }

    /** Reads the bind-pose positions of triangle 0. */
    private static Vector3f[] bindTriangle(Mesh mesh) {
        FloatBuffer bind = (FloatBuffer) mesh.getBuffer(Type.BindPosePosition).getData();
        ByteBuffer indices = (ByteBuffer) mesh.getBuffer(Type.BoneIndex).getData();
        // buildSqueezeMesh uses indices 0,1,2.
        Vector3f[] out = new Vector3f[3];
        for (int k = 0; k < 3; k++) {
            out[k] = new Vector3f(bind.get(k * 3), bind.get(k * 3 + 1), bind.get(k * 3 + 2));
        }
        return out;
    }

    /**
     * Independently evaluates the three deformed vertices of a mesh owned by a
     * cloned scene, using that clone's own armature palette.
     */
    private static Vector3f[] cloneVertices(SkinningControl skin, Geometry geometry) {
        Matrix4f[] palette = skin.getArmature().computeSkinningMatrices();
        Mesh mesh = geometry.getMesh();
        FloatBuffer bind = (FloatBuffer) mesh.getBuffer(Type.BindPosePosition).getData();
        FloatBuffer weights = (FloatBuffer) mesh.getBuffer(Type.BoneWeight).getData();
        ByteBuffer indices = (ByteBuffer) mesh.getBuffer(Type.BoneIndex).getData();
        Vector3f[] out = new Vector3f[3];
        for (int v = 0; v < 3; v++) {
            float x = bind.get(v * 3);
            float y = bind.get(v * 3 + 1);
            float z = bind.get(v * 3 + 2);
            float rx = 0, ry = 0, rz = 0;
            for (int w = 0; w < mesh.getMaxNumWeights(); w++) {
                float wt = weights.get(v * 4 + w);
                Matrix4f m = palette[indices.get(v * 4 + w) & 0xFF];
                rx += (m.m00 * x + m.m01 * y + m.m02 * z + m.m03) * wt;
                ry += (m.m10 * x + m.m11 * y + m.m12 * z + m.m13) * wt;
                rz += (m.m20 * x + m.m21 * y + m.m22 * z + m.m23) * wt;
            }
            out[v] = new Vector3f(rx, ry, rz);
        }
        return out;
    }

    /**
     * An armature with a non-unit inverse bind matrix (scale + rotation) and
     * two joints whose rotations do not commute.
     */
    private static Armature buildNonUnitArmature() {
        Joint root = new Joint("root");
        root.setLocalTranslation(new Vector3f(0.5f, 1f, -0.5f));
        root.setLocalScale(new Vector3f(1.5f, 1.5f, 1.5f));
        root.setLocalRotation(new Quaternion().fromAngleAxis(0.3f, Vector3f.UNIT_X));

        Joint child = new Joint("child");
        child.setLocalTranslation(new Vector3f(0.75f, 0.25f, 0.5f));
        child.setLocalRotation(new Quaternion().fromAngleAxis(0.6f, Vector3f.UNIT_Y));
        child.setLocalScale(new Vector3f(0.5f, 0.5f, 0.5f));
        root.addChild(child);

        Armature armature = new Armature(new Joint[]{root, child});
        armature.saveBindPose();
        return armature;
    }

    private static AnimClip buildNonUnitClip(Armature armature) {
        Joint root = armature.getJoint("root");
        Joint child = armature.getJoint("child");
        float[] times = {0f, 2f};
        TransformTrack rootTrack = new TransformTrack(root, times,
                new Vector3f[]{new Vector3f(0.5f, 1f, -0.5f), new Vector3f(0.9f, 0.6f, -0.2f)},
                new Quaternion[]{
                        new Quaternion().fromAngleAxis(0.3f, Vector3f.UNIT_X),
                        new Quaternion().fromAngleAxis(1.1f, Vector3f.UNIT_X)
                }, null);
        TransformTrack childTrack = new TransformTrack(child, times,
                new Vector3f[]{new Vector3f(0.75f, 0.25f, 0.5f), new Vector3f(0.25f, 0.9f, 0.1f)},
                new Quaternion[]{
                        new Quaternion().fromAngleAxis(0.6f, Vector3f.UNIT_Y),
                        new Quaternion().fromAngleAxis(1.4f, Vector3f.UNIT_Z)
                }, null);
        AnimClip clip = new AnimClip("rot");
        clip.setTracks(new AnimTrack[]{rootTrack, childTrack});
        return clip;
    }

    private static Mesh buildCollapsedMesh() {
        Mesh mesh = new Mesh();
        // All three vertices identical in the bind pose: degenerate.
        mesh.setBuffer(Type.Position, 3, new float[]{0.4f, 0f, 0f, 0.4f, 0f, 0f, 0.4f, 0f, 0f});
        mesh.setBuffer(Type.Normal, 3, new float[]{0, 0, 1, 0, 0, 1, 0, 0, 1});
        mesh.setBuffer(Type.Index, 3, new short[]{0, 1, 2});
        mesh.setBuffer(Type.BoneIndex, 4, ByteBuffer.wrap(new byte[]{
                0, 1, 0, 0,
                0, 1, 0, 0,
                0, 1, 0, 0
        }));
        mesh.setBuffer(Type.BoneWeight, 4, BufferUtils.createFloatBuffer(new float[]{
                0.5f, 0.5f, 0f, 0f,
                0.5f, 0.5f, 0f, 0f,
                0.5f, 0.5f, 0f, 0f
        }));
        mesh.setMaxNumWeights(2);
        mesh.generateBindPose();
        mesh.updateBound();
        return mesh;
    }

    // =====================================================================
    // lifecycle
    // =====================================================================

    /** A control that counts ordinary updates. */
    private static final class CountingControl extends AbstractControl {
        int updates;

        @Override
        protected void controlUpdate(float tpf) {
            updates++;
        }

        @Override
        protected void controlRender(com.jme3.renderer.RenderManager rm,
                com.jme3.renderer.ViewPort vp) {
        }
    }

    /** A control that counts both ordinary and post-logical updates. */
    private static final class CountingPostControl extends AbstractControl
            implements PostLogicalControl {
        int updates;
        int posts;

        @Override
        protected void controlUpdate(float tpf) {
            updates++;
        }

        @Override
        protected void controlRender(com.jme3.renderer.RenderManager rm,
                com.jme3.renderer.ViewPort vp) {
        }

        @Override
        public void postLogicalUpdate(float tpf) {
            if (enabled) {
                posts++;
            }
        }
    }

    public static void lifecycle(TaskChecks c) {
        // 1) SkinningControl is added before AnimComposer: the post phase
        //    must still see the final animation pose.
        Scene scene = buildSceneWithComposerLast(Format.UnsignedByte);
        scene.composer.setTime(0.75);
        ((BlendableAction) scene.composer.getCurrentAction()).setTransitionLength(0);
        // Force the composer to evaluate its clip before the logical update.
        updateFrame(scene.root);

        Node attachment = scene.skin.addSurfaceAttachment("badge", scene.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform());
        // A new frame with an unrendered update must still move the badge.
        scene.composer.setTime(0.25);
        updateFrame(scene.root);

        Vector3f[] skinned = expectedVertices(scene, BIND, BONE_INDEX, BONE_WEIGHT, 4);
        Vector3f centroid = blend(skinned[0], skinned[1], skinned[2],
                barycentric(1f / 3f, 1f / 3f, 1f / 3f));
        checkNear(c, attachment.getLocalTranslation(), centroid,
                "post phase sees the final animation pose");

        // 2) Ordinary updates run once per control per logical update, and
        //    post controls run exactly once in the second phase.
        CountingControl ordinary = new CountingControl();
        CountingPostControl post = new CountingPostControl();
        Node counter = new Node("counter");
        counter.addControl(ordinary);
        counter.addControl(post);
        scene.root.attachChild(counter);

        ordinary.updates = 0;
        post.updates = 0;
        post.posts = 0;
        scene.root.updateLogicalState(0.016f);
        c.check(ordinary.updates == 1, "ordinary control ran exactly once");
        c.check(post.updates == 1, "post control ordinary update ran exactly once");
        c.check(post.posts == 1, "post control post-logical update ran exactly once");

        // 3) The root's own controls participate in the post phase.
        CountingPostControl rootPost = new CountingPostControl();
        scene.root.addControl(rootPost);
        rootPost.posts = 0;
        scene.root.updateLogicalState(0.016f);
        c.check(rootPost.posts == 1, "root post control ran exactly once");

        // A disabled SkinningControl must be skipped by the post phase.
        Node disabledBadge = scene.skin.getSurfaceAttachment("badge");
        Vector3f disabledBefore = disabledBadge.getLocalTranslation().clone();
        scene.skin.setEnabled(false);
        scene.composer.setTime(0.5);
        scene.root.updateLogicalState(0.016f);
        checkNear(c, disabledBadge.getLocalTranslation(), disabledBefore,
                "disabled skinning control skips the post phase");
        scene.skin.setEnabled(true);
        scene.root.updateLogicalState(0.016f);
        Vector3f[] enabledSkinned = expectedVertices(scene, BIND, BONE_INDEX, BONE_WEIGHT, 4);
        // The badge offset is zero here because it was created with an identity
        // transform in this scene.
        checkNear(c, disabledBadge.getLocalTranslation(),
                blend(enabledSkinned[0], enabledSkinned[1], enabledSkinned[2],
                        barycentric(1f / 3f, 1f / 3f, 1f / 3f)),
                "re-enabled skinning control resumes driving");
        scene.composer.setTime(1.5);
        scene.root.updateLogicalState(0.016f);
        c.check(disabledBadge.getLocalTranslation().distance(disabledBefore) > 1e-4f,
                "re-enabled skinning control tracks the new pose");

        // 4) A deep ancestor control updating later still produces a correct
        //    post-phase attachment.
        Scene deep = buildScene(Format.UnsignedByte, false);
        Node outer = new Node("outer");
        deep.root.detachChild(deep.geometry);
        outer.attachChild(deep.geometry);
        deep.root.attachChild(outer);
        // Put the composer on the outer node so it updates after the skinning
        // control (which is on the root).
        deep.root.removeControl(AnimComposer.class);
        outer.addControl(deep.composer);
        deep.composer.setCurrentAction("motion");
        ((BlendableAction) deep.composer.getCurrentAction()).setTransitionLength(0);
        deep.composer.setTime(0.5);
        updateFrame(deep.root);

        Node deepAttachment = deep.skin.addSurfaceAttachment("deep", deep.geometry, 0,
                barycentric(0.2f, 0.3f, 0.5f), new Transform());
        deep.composer.setTime(0.1);
        updateFrame(deep.root);
        Vector3f[] deepSkinned = expectedVertices(deep, BIND, BONE_INDEX, BONE_WEIGHT, 4);
        checkNear(c, deepAttachment.getLocalTranslation(),
                blend(deepSkinned[0], deepSkinned[1], deepSkinned[2],
                        barycentric(0.2f, 0.3f, 0.5f)),
                "deep ancestor composer still yields a correct frame");

        // 5) Hardware preference must not change logical-update behaviour.
        for (boolean hardware : new boolean[]{true, false}) {
            Scene hw = buildScene(Format.UnsignedByte, hardware);
            hw.composer.setTime(0.5);
            updateFrame(hw.root);
            Node hwNode = hw.skin.addSurfaceAttachment("hw", hw.geometry, 0,
                    barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform());
            hw.composer.setTime(0.8);
            updateFrame(hw.root);
            Vector3f[] s = expectedVertices(hw, BIND, BONE_INDEX, BONE_WEIGHT, 4);
            checkNear(c, hwNode.getLocalTranslation(),
                    blend(s[0], s[1], s[2], barycentric(1f / 3f, 1f / 3f, 1f / 3f)),
                    "attachment works with hardwarePreferred=" + hardware);
            c.check(!hw.skin.isHardwareSkinningUsed(),
                    "no hardware skinning without a render manager");
        }

        // 6) Paused animation plus moving/scaling source ancestors still
        //    updates the attachment.
        Scene paused = buildScene(Format.UnsignedByte, false);
        Node mover = new Node("mover");
        paused.root.detachChild(paused.geometry);
        mover.attachChild(paused.geometry);
        paused.root.attachChild(mover);
        paused.composer.setTime(0.5);
        updateFrame(paused.root);
        Node pausedNode = paused.skin.addSurfaceAttachment("paused", paused.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform());
        Vector3f before = pausedNode.getLocalTranslation().clone();

        mover.setLocalTranslation(1.5f, -0.5f, 2f);
        mover.setLocalScale(2f, 2f, 2f);
        updateFrame(paused.root);
        Vector3f after = pausedNode.getLocalTranslation().clone();
        c.check(before.distance(after) > 0.1f, "moving ancestor updates a paused attachment");

        // 7) Degenerate fallback: as the triangle collapses, position keeps
        //    following the centroid and orientation is retained; recovering
        //    re-derives it from the edges.
        Scene degenerate = buildScene(Format.UnsignedByte, false);
        // Build a mesh whose animation collapses the triangle at t=0.5.
        Mesh squeeze = buildSqueezeMesh();
        Geometry squeezeGeo = new Geometry("squeeze", squeeze);
        degenerate.root.detachChild(degenerate.geometry);
        degenerate.root.attachChild(squeezeGeo);
        degenerate.root.removeControl(SkinningControl.class);
        Armature squeezeArmature = buildSqueezeArmature();
        SkinningControl squeezeSkin = new SkinningControl(squeezeArmature);
        squeezeSkin.setHardwareSkinningPreferred(false);
        degenerate.root.addControl(squeezeSkin);
        degenerate.armature = squeezeArmature;

        AnimComposer squeezeComposer = new AnimComposer();
        squeezeComposer.addAnimClip(buildSqueezeClip(squeezeSkin.getArmature()));
        degenerate.root.removeControl(AnimComposer.class);
        degenerate.root.addControl(squeezeComposer);
        squeezeComposer.setCurrentAction("squeeze");
        ((BlendableAction) squeezeComposer.getCurrentAction()).setTransitionLength(0);
        squeezeComposer.setTime(0f);
        updateFrame(degenerate.root);

        Node squeezeNode = squeezeSkin.addSurfaceAttachment("squeeze", squeezeGeo, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform());
        updateFrame(degenerate.root);
        Quaternion initialRotation = squeezeNode.getLocalRotation().clone();
        c.check(Quaternion.isValidQuaternion(initialRotation), "initial squeeze rotation valid");

        // Verify the non-degenerate frame is the edge-derived frame.
        {
            Vector3f[] pts = squeezePoints(degenerate, squeezeGeo);
            Quaternion expected = new Quaternion().fromAxes(
                    frameX(pts[0], pts[1]),
                    frameY(frameX(pts[0], pts[1]), frameZ(pts[0], pts[1], pts[2])),
                    frameZ(pts[0], pts[1], pts[2]));
            c.check(sameRotation(initialRotation, expected),
                    "non-degenerate orientation comes from the deformed edges");
        }

        // During the collapsed frame, the position must still follow the
        // centroid even though orientation is retained.
        squeezeComposer.setTime(1f);
        updateFrame(degenerate.root);
        Vector3f[] collapsed = squeezePoints(degenerate, squeezeGeo);
        Vector3f collapsedCentroid = blend(collapsed[0], collapsed[1], collapsed[2],
                barycentric(1f / 3f, 1f / 3f, 1f / 3f));
        checkNear(c, squeezeNode.getLocalTranslation(), collapsedCentroid,
                "degenerate position still follows the centroid");
        c.check(sameRotation(squeezeNode.getLocalRotation(), initialRotation),
                "degenerate frame keeps the last valid orientation");

        // Recovering re-derives the orientation from the edges, and offset is
        // not accumulated (offset is identity here, so compare to the freshly
        // derived frame).
        squeezeComposer.setTime(2f);
        updateFrame(degenerate.root);
        Vector3f[] recovered = squeezePoints(degenerate, squeezeGeo);
        Quaternion recoveredRotation = new Quaternion().fromAxes(
                frameX(recovered[0], recovered[1]),
                frameY(frameX(recovered[0], recovered[1]),
                        frameZ(recovered[0], recovered[1], recovered[2])),
                frameZ(recovered[0], recovered[1], recovered[2]));
        c.check(sameRotation(squeezeNode.getLocalRotation(), recoveredRotation),
                "orientation re-derived after degeneracy without accumulating offset");

        // 8) Cloning: deep-clone the whole actor and confirm full independence.
        Scene original = buildScene(Format.UnsignedByte, false);
        original.composer.setTime(0.4);
        updateFrame(original.root);
        Node originalAttachment = original.skin.addSurfaceAttachment("badge",
                original.geometry, 0, barycentric(0.3f, 0.3f, 0.4f),
                new Transform(new Vector3f(0.1f, 0.2f, 0.3f), Quaternion.IDENTITY,
                        new Vector3f(1f, 1f, 1f)));

        Node cloneRoot = Cloner.deepClone(original.root);
        SkinningControl cloneSkin = cloneRoot.getControl(SkinningControl.class);
        c.check(cloneSkin != original.skin, "clone has its own SkinningControl");
        Node cloneAttachment = cloneSkin.getSurfaceAttachment("badge");
        c.check(cloneAttachment != null && cloneAttachment != originalAttachment,
                "clone has its own attachment node");
        c.check(cloneAttachment.getParent() == cloneRoot,
                "cloned attachment is attached to the cloned actor");
        c.check(cloneSkin.getSurfaceAttachment("badge") != null,
                "clone attachment lookup works");

        Geometry cloneSource = findGeometry(cloneRoot, "skin");
        c.check(cloneSource != null, "cloned source geometry found");
        c.check(cloneSource != original.geometry, "clone owns its source geometry");

        // Moving the clone's attachment state must not touch the original.
        cloneAttachment.setLocalTranslation(9f, 9f, 9f);
        c.check(originalAttachment.getLocalTranslation().distance(new Vector3f(9f, 9f, 9f)) > 1f,
                "clone attachment transform is independent");

        // Interleaved updates on both graphs stay independent.
        cloneSkin.removeSurfaceAttachment("badge");
        c.check(cloneSkin.getSurfaceAttachment("badge") == null,
                "removed attachment is gone from the clone");
        c.check(original.skin.getSurfaceAttachment("badge") != null,
                "removing from the clone leaves the original attachment");

        // Re-add on the clone, then interleave animation times: each graph's
        // attachment must follow its own armature.
        Node cloneBadge = cloneSkin.addSurfaceAttachment("badge2", cloneSource, 0,
                barycentric(0.3f, 0.3f, 0.4f), new Transform());
        AnimComposer cloneComposer = cloneRoot.getControl(AnimComposer.class);
        c.check(cloneComposer != null && cloneComposer != original.composer,
                "clone has its own composer");
        // The cloned layer starts with no current action; restore one.
        cloneComposer.setCurrentAction("motion");
        ((BlendableAction) cloneComposer.getCurrentAction()).setTransitionLength(0);

        cloneComposer.setTime(0.2);
        original.composer.setTime(1.6);
        updateFrame(cloneRoot);
        updateFrame(original.root);

        Vector3f[] originalSkinned = expectedVertices(original, BIND, BONE_INDEX, BONE_WEIGHT, 4);
        Vector3f[] cloneSkinned = cloneVertices(cloneSkin, cloneSource);
        Vector3f oX = frameX(originalSkinned[0], originalSkinned[1]);
        Vector3f oZ = frameZ(originalSkinned[0], originalSkinned[1], originalSkinned[2]);
        Quaternion oFrame = new Quaternion().fromAxes(oX, frameY(oX, oZ), oZ);
        Vector3f oCentroid = blend(originalSkinned[0], originalSkinned[1], originalSkinned[2],
                barycentric(0.3f, 0.3f, 0.4f));
        checkNear(c, originalAttachment.getLocalTranslation(),
                oCentroid.add(oFrame.mult(new Vector3f(0.1f, 0.2f, 0.3f), null)),
                "original attachment follows the original pose");
        checkNear(c, cloneBadge.getLocalTranslation(),
                blend(cloneSkinned[0], cloneSkinned[1], cloneSkinned[2],
                        barycentric(0.3f, 0.3f, 0.4f)),
                "clone attachment follows the cloned pose");

        // The clone's attachment state is its own; interleaved updates on the
        // original must not disturb it. The clone's badge has an identity
        // offset, so its translation is just the centroid.
        cloneComposer.setTime(0.5);
        updateFrame(cloneRoot);
        Vector3f[] cloneSkinned2 = cloneVertices(cloneSkin, cloneSource);
        Vector3f cloneCentroid = blend(cloneSkinned2[0], cloneSkinned2[1], cloneSkinned2[2],
                barycentric(0.3f, 0.3f, 0.4f));
        original.composer.setTime(1.0);
        updateFrame(original.root);
        updateFrame(cloneRoot);
        checkNear(c, cloneBadge.getLocalTranslation(), cloneCentroid,
                "clone re-evaluation is unaffected by the original attachment");

        // 9) Removal detaches and stops driving.
        Node removed = original.skin.removeSurfaceAttachment("badge");
        c.check(removed != null, "removeSurfaceAttachment returns the node");
        c.check(removed.getParent() == null, "removed attachment is detached");
        c.check(original.skin.getSurfaceAttachment("badge") == null,
                "removed attachment lookup returns null");
        Vector3f removedPosition = removed.getLocalTranslation().clone();
        original.composer.setTime(0.9);
        updateFrame(original.root);
        c.check(removed.getLocalTranslation().distance(removedPosition) <= EPS,
                "removed attachment is no longer driven");

        // 10) Invalid arguments are rejected and never leave a node behind.
        Scene invalid = buildScene(Format.UnsignedByte, false);
        updateFrame(invalid.root);
        Node existing = invalid.skin.addSurfaceAttachment("ok", invalid.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform());
        int childrenBefore = countChildren(invalid.root);

        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("ok", invalid.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform()), "duplicate name");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("new", invalid.geometry, 99,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform()), "triangle out of range");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("new", invalid.geometry, -1,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform()), "negative triangle");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("new", invalid.geometry, 0,
                barycentric(0.5f, 0.5f, 0.5f), new Transform()), "barycentric sum");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("new", invalid.geometry, 0,
                barycentric(-0.1f, 0.6f, 0.5f), new Transform()), "negative barycentric");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("new", invalid.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f),
                new Transform(Vector3f.ZERO, Quaternion.IDENTITY, new Vector3f(1, 2, 1))),
                "non-uniform offset scale");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("new", invalid.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f),
                new Transform(Vector3f.ZERO, Quaternion.IDENTITY, new Vector3f(0, 0, 0))),
                "zero offset scale");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("new", invalid.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f),
                new Transform(new Vector3f(Float.NaN, 0, 0), Quaternion.IDENTITY,
                        Vector3f.UNIT_XYZ)), "non-finite offset");

        Geometry outsider = new Geometry("outsider", buildMesh(Format.UnsignedByte));
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("new", outsider, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform()), "foreign geometry");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment(null, invalid.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform()), "null name");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("", invalid.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform()), "empty name");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("new", null, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform()), "null source");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("new", invalid.geometry, 0,
                null, new Transform()), "null barycentric");
        expectIllegal(c, () -> invalid.skin.addSurfaceAttachment("new", invalid.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), null), "null offset");

        c.check(countChildren(invalid.root) == childrenBefore,
                "failed additions leave no new scene nodes");
        c.check(invalid.skin.getSurfaceAttachment("new") == null,
                "failed additions leave no attachment");
        c.check(existing != null && existing.getParent() == invalid.root,
                "existing attachment unaffected by failures");

        // 11) A morph mesh is rejected.
        Scene morph = buildScene(Format.UnsignedByte, false);
        Mesh morphMesh = buildMesh(Format.UnsignedByte);
        morphMesh.addMorphTarget(new com.jme3.scene.mesh.MorphTarget("morph"));
        Geometry morphGeo = new Geometry("morph", morphMesh);
        morph.root.detachChild(morph.geometry);
        morph.root.attachChild(morphGeo);
        morph.root.removeControl(SkinningControl.class);
        SkinningControl morphSkin = new SkinningControl(morph.armature);
        morph.root.addControl(morphSkin);
        updateFrame(morph.root);
        expectIllegal(c, () -> morphSkin.addSurfaceAttachment("morph", morphGeo, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform()), "morph mesh");

        // 12) Failure during a logical update leaves every attachment unchanged.
        Scene failing = buildScene(Format.UnsignedByte, false);
        failing.composer.setTime(0.5);
        updateFrame(failing.root);
        Node keep = failing.skin.addSurfaceAttachment("keep", failing.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform());
        Node broken = failing.skin.addSurfaceAttachment("broken", failing.geometry, 0,
                barycentric(0.5f, 0.25f, 0.25f), new Transform());
        Vector3f keepBefore = keep.getLocalTranslation().clone();
        Vector3f brokenBefore = broken.getLocalTranslation().clone();

        // Replace the source mesh so the next logical update fails.
        failing.geometry.setMesh(new Mesh());
        boolean failed = false;
        try {
            failing.root.updateLogicalState(0.016f);
        } catch (RuntimeException expected) {
            failed = true;
        }
        c.check(failed, "replaced source mesh makes the logical update fail");
        checkNear(c, keep.getLocalTranslation(), keepBefore,
                "failed update keeps the first attachment unchanged");
        checkNear(c, broken.getLocalTranslation(), brokenBefore,
                "failed update keeps the second attachment unchanged");

        // An invalid bone index must also fail loudly.
        Scene bone = buildScene(Format.UnsignedByte, false);
        bone.composer.setTime(0.5);
        updateFrame(bone.root);
        Node boneNode = bone.skin.addSurfaceAttachment("bone", bone.geometry, 0,
                barycentric(1f / 3f, 1f / 3f, 1f / 3f), new Transform());
        Vector3f boneBefore = boneNode.getLocalTranslation().clone();

        VertexBuffer badIndex = new VertexBuffer(Type.BoneIndex);
        badIndex.setupData(VertexBuffer.Usage.CpuOnly, 4, Format.UnsignedByte,
                ByteBuffer.wrap(new byte[]{9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
        Mesh boneMesh = bone.geometry.getMesh();
        boneMesh.clearBuffer(Type.BoneIndex);
        boneMesh.setBuffer(badIndex);
        boolean boneFailed = false;
        try {
            bone.root.updateLogicalState(0.016f);
        } catch (RuntimeException expected) {
            boneFailed = true;
        }
        c.check(boneFailed, "invalid bone index makes the logical update fail");
        checkNear(c, boneNode.getLocalTranslation(), boneBefore,
                "failed bone-index update keeps the attachment unchanged");

        // 13) Update-list invalidation still works after the two-phase change.
        Node root = new Node("root");
        CountingControl late = new CountingControl();
        root.updateLogicalState(0.016f);
        Node child = new Node("child");
        root.attachChild(child);
        child.addControl(late);
        root.updateLogicalState(0.016f);
        c.check(late.updates == 1, "update list picked up a newly added control");
        root.updateLogicalState(0.016f);
        c.check(late.updates == 2, "update list keeps updating the control");
    }

    private static void expectIllegal(TaskChecks c, Runnable action, String message) {
        boolean thrown = false;
        try {
            action.run();
        } catch (IllegalArgumentException expected) {
            thrown = true;
        }
        c.check(thrown, "IllegalArgumentException: " + message);
    }

    private static Geometry findGeometry(Node root, String name) {
        for (Spatial child : root.getChildren()) {
            if (child instanceof Geometry && name.equals(child.getName())) {
                return (Geometry) child;
            }
            if (child instanceof Node) {
                Geometry found = findGeometry((Node) child, name);
                if (found != null) {
                    return found;
                }
            }
        }
        return null;
    }

    private static Scene buildSceneWithComposerLast(Format boneIndexFormat) {
        Scene scene = new Scene();
        scene.armature = buildArmature();
        scene.j0 = scene.armature.getJoint("j0");
        scene.j1 = scene.armature.getJoint("j1");
        scene.j2 = scene.armature.getJoint("j2");
        scene.j3 = scene.armature.getJoint("j3");

        scene.geometry = new Geometry("skin", buildMesh(boneIndexFormat));
        scene.root.attachChild(scene.geometry);

        scene.skin = new SkinningControl(scene.armature);
        scene.skin.setHardwareSkinningPreferred(false);
        scene.root.addControl(scene.skin);

        scene.composer = new AnimComposer();
        AnimClip clip = buildClip(scene.j0, scene.j1);
        scene.composer.addAnimClip(clip);
        scene.root.addControl(scene.composer);
        scene.composer.setCurrentAction("motion");
        ((BlendableAction) scene.composer.getCurrentAction()).setTransitionLength(0);
        scene.composer.setTime(0);
        return scene;
    }

    // ---------------------------------------------------------------------
    // Degenerate squeeze fixture
    // ---------------------------------------------------------------------

    private static Armature buildSqueezeArmature() {
        Joint j0 = new Joint("s0");
        Joint j1 = new Joint("s1");
        Armature armature = new Armature(new Joint[]{j0, j1});
        armature.saveBindPose();
        return armature;
    }

    /** Builds a triangle whose vertices converge to a common point at t=0.5. */
    private static Mesh buildSqueezeMesh() {
        Mesh mesh = new Mesh();
        mesh.setBuffer(Type.Position, 3, new float[]{
                0f, 0f, 0f,
                1f, 0f, 0f,
                0f, 1f, 0f
        });
        mesh.setBuffer(Type.Normal, 3, new float[]{0, 0, 1, 0, 0, 1, 0, 0, 1});
        mesh.setBuffer(Type.Index, 3, new short[]{0, 1, 2});
        mesh.setBuffer(Type.BoneIndex, 4, ByteBuffer.wrap(new byte[]{
                0, 0, 0, 0,
                0, 0, 0, 0,
                0, 0, 0, 0
        }));
        mesh.setBuffer(Type.BoneWeight, 4, BufferUtils.createFloatBuffer(new float[]{
                1f, 0f, 0f, 0f,
                1f, 0f, 0f, 0f,
                1f, 0f, 0f, 0f
        }));
        mesh.setMaxNumWeights(1);
        mesh.generateBindPose();
        mesh.updateBound();
        return mesh;
    }

    /**
     * Animates joint s0 with a non-uniform scale that collapses one axis at
     * t=0.5, making the triangle degenerate, then restores it at t=1.
     * (A pure scale is used because it is representable as a joint transform.)
     */
    /**
     * Animates joint s0 with a non-uniform scale that collapses both the X and
     * Y extents at t=1 (making the triangle degenerate by edge length), then
     * restores it by t=2.
     */
    private static AnimClip buildSqueezeClip(Armature armature) {
        Joint j0 = armature.getJoint("s0");
        float[] times = {0f, 1f, 2f};
        TransformTrack track = new TransformTrack(j0, times,
                new Vector3f[]{new Vector3f(0, 0, 0), new Vector3f(0, 0, 0), new Vector3f(0, 0, 0)},
                null,
                new Vector3f[]{
                        new Vector3f(1f, 1f, 1f),
                        new Vector3f(1e-7f, 1e-7f, 1f),
                        new Vector3f(1f, 1f, 1f)
                });
        AnimClip clip = new AnimClip("squeeze");
        clip.setTracks(new AnimTrack[]{track});
        return clip;
    }

    /**
     * Computes the three deformed vertices of the squeeze mesh independently,
     * using the current armature palette.
     */
    private static Vector3f[] squeezePoints(Scene scene, Geometry geometry) {
        Matrix4f[] palette = scene.armature.computeSkinningMatrices();
        Mesh mesh = geometry.getMesh();
        FloatBuffer bind = (FloatBuffer) mesh.getBuffer(Type.BindPosePosition).getData();
        FloatBuffer weights = (FloatBuffer) mesh.getBuffer(Type.BoneWeight).getData();
        ByteBuffer indices = (ByteBuffer) mesh.getBuffer(Type.BoneIndex).getData();
        int[] tri = {0, 1, 2};
        Vector3f[] out = new Vector3f[3];
        for (int k = 0; k < 3; k++) {
            int v = tri[k];
            float x = bind.get(v * 3);
            float y = bind.get(v * 3 + 1);
            float z = bind.get(v * 3 + 2);
            float rx = 0, ry = 0, rz = 0;
            for (int w = 0; w < mesh.getMaxNumWeights(); w++) {
                float wt = weights.get(v * 4 + w);
                Matrix4f m = palette[indices.get(v * 4 + w) & 0xFF];
                rx += (m.m00 * x + m.m01 * y + m.m02 * z + m.m03) * wt;
                ry += (m.m10 * x + m.m11 * y + m.m12 * z + m.m13) * wt;
                rz += (m.m20 * x + m.m21 * y + m.m22 * z + m.m23) * wt;
            }
            out[k] = new Vector3f(rx, ry, rz);
        }
        return out;
    }
}
