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
import com.jme3.math.Quaternion;
import com.jme3.math.Transform;
import com.jme3.math.Vector3f;
import com.jme3.scene.Geometry;
import com.jme3.scene.Mesh;
import com.jme3.scene.Node;
import com.jme3.scene.VertexBuffer;
import com.jme3.scene.VertexBuffer.Type;
import com.jme3.scene.shape.Box;
import com.jme3.util.BufferUtils;

import java.nio.ByteBuffer;

/**
 * A windowless demonstration of {@link SkinningControl} surface attachments.
 *
 * <p>A two-joint armature deforms a single triangle. A badge geometry is
 * attached to the interior of that triangle with
 * {@code addSurfaceAttachment(...)} and then follows the skinned surface as
 * the animation plays. The example performs real logical and geometric updates
 * on a plain {@link Node}; no renderer, camera, or display is required.</p>
 *
 * <p>Run it with:
 * {@code java -Djava.awt.headless=true -cp <classes> SurfaceAttachmentExample}</p>
 */
public final class SurfaceAttachmentExample {

    public static void main(String[] args) {
        System.out.println("SurfaceAttachmentExample: a badge follows a deforming"
                + " triangle through logical/geometric updates only (no renderer).");

        // --- Armature: one root joint and one child joint. -----------------
        Joint root = new Joint("root");
        root.setLocalTranslation(new Vector3f(0f, 0f, 0f));

        Joint tip = new Joint("tip");
        tip.setLocalTranslation(new Vector3f(1f, 0f, 0f));
        root.addChild(tip);

        Armature armature = new Armature(new Joint[]{root, tip});
        armature.saveBindPose();

        // --- A single skinned triangle in the XY plane. --------------------
        Mesh mesh = new Mesh();
        mesh.setBuffer(Type.Position, 3, new float[]{
                0f, 0f, 0f,
                2f, 0f, 0f,
                0f, 1f, 0f
        });
        mesh.setBuffer(Type.Normal, 3, new float[]{0f, 0f, 1f, 0f, 0f, 1f, 0f, 0f, 1f});
        mesh.setBuffer(Type.Index, 3, new short[]{0, 1, 2});
        // The base of the triangle follows the root joint, the far vertex the
        // tip joint; the third blends both.
        mesh.setBuffer(Type.BoneIndex, 4, ByteBuffer.wrap(new byte[]{
                0, 0, 0, 0,
                1, 0, 0, 0,
                0, 1, 0, 0
        }));
        mesh.setBuffer(Type.BoneWeight, 4, BufferUtils.createFloatBuffer(new float[]{
                1f, 0f, 0f, 0f,
                1f, 0f, 0f, 0f,
                0.5f, 0.5f, 0f, 0f
        }));
        mesh.setMaxNumWeights(2);
        mesh.generateBindPose();
        mesh.updateBound();

        Geometry skin = new Geometry("triangle", mesh);

        // --- Scene graph and animation. -----------------------------------
        Node actor = new Node("actor");
        actor.attachChild(skin);

        AnimClip clip = new AnimClip("wave");
        float[] times = {0f, 1f};
        TransformTrack track = new TransformTrack(tip, times,
                new Vector3f[]{new Vector3f(1f, 0f, 0f), new Vector3f(3f, 1f, 0f)},
                new Quaternion[]{
                        Quaternion.IDENTITY,
                        new Quaternion().fromAngleAxis(FastMath.HALF_PI, Vector3f.UNIT_Z)
                },
                null);
        clip.setTracks(new AnimTrack[]{track});

        AnimComposer composer = new AnimComposer();
        composer.addAnimClip(clip);
        actor.addControl(composer);
        composer.setCurrentAction("wave");
        ((BlendableAction) composer.getCurrentAction()).setTransitionLength(0);
        composer.setTime(0f);

        SkinningControl skinning = new SkinningControl(armature);
        skinning.setHardwareSkinningPreferred(false);
        // Deliberately add the skinning control last so that its ordinary
        // update runs after the composer's; the post-logical phase still sees
        // the final pose.
        actor.addControl(skinning);

        // --- Attach a badge to the middle of the deforming triangle. ------
        // The attachment is placed by a triangle index plus barycentric
        // coordinates, and an offset relative to the surface frame. Its
        // position is the barycentric blend of the three skinned vertices;
        // its orientation comes from the deformed triangle edges.
        Transform offset = new Transform(new Vector3f(0f, 0f, 0.1f),
                Quaternion.IDENTITY, new Vector3f(1f, 1f, 1f));
        Node badge = skinning.addSurfaceAttachment("badge", skin, 0,
                new Vector3f(1f / 3f, 1f / 3f, 1f / 3f), offset);

        Geometry badgeGeometry = new Geometry("badgeMesh", new Box(0.1f, 0.1f, 0.02f));
        badge.attachChild(badgeGeometry);

        // --- Drive the graph. ----------------------------------------------
        for (int frame = 0; frame <= 4; frame++) {
            composer.setTime(frame * 0.25);
            actor.updateLogicalState(0.25f);
            actor.updateGeometricState();

            BoundingBox box = (BoundingBox) badgeGeometry.getWorldBound();
            System.out.printf("t=%.2f badge=(%.3f, %.3f, %.3f) badgeCenter=(%.3f, %.3f, %.3f)%n",
                    composer.getTime(),
                    badge.getLocalTranslation().x, badge.getLocalTranslation().y,
                    badge.getLocalTranslation().z,
                    box.getCenter().x, box.getCenter().y, box.getCenter().z);
        }

        // --- Remove the badge; it is detached and no longer driven. --------
        skinning.removeSurfaceAttachment("badge");
        System.out.println("badge attached after removal: " + (badge.getParent() != null));
        System.out.println("SURFACE_ATTACHMENT_EXAMPLE_OK");
    }
}
