import com.jme3.anim.*;
import com.jme3.anim.tween.action.*;
import com.jme3.math.*;
import com.jme3.scene.*;
import com.jme3.scene.VertexBuffer.Type;
import java.nio.*;
public final class Baseline {
    static void need(boolean x) { if(!x) throw new AssertionError("production skin baseline"); }
    public static void main(String[] args) throws Exception {
        Joint root=new Joint("root"), child=new Joint("child");
        root.setLocalTranslation(new Vector3f(1,0,0));
        child.setLocalTranslation(new Vector3f(1,0,0)); root.addChild(child);
        Armature arm=new Armature(new Joint[]{root,child}); arm.saveBindPose();
        Mesh mesh=new Mesh();
        mesh.setBuffer(Type.Position,3,new float[]{2,0,0,3,0,0,2,1,0});
        mesh.setBuffer(Type.Normal,3,new float[]{0,0,1,0,0,1,0,0,1});
        mesh.setBuffer(Type.Index,3,new short[]{0,1,2});
        mesh.setBuffer(Type.BoneIndex,4,ByteBuffer.wrap(new byte[]{0,1,0,0,1,0,0,0,0,1,0,0}));
        mesh.setBuffer(Type.BoneWeight,4,FloatBuffer.wrap(new float[]{.25f,.75f,0,0,.6f,.4f,0,0,.5f,.5f,0,0}));
        mesh.setMaxNumWeights(2); mesh.generateBindPose(); mesh.updateBound();
        Node model=new Node("actor"); Geometry geo=new Geometry("skin",mesh); model.attachChild(geo);
        AnimClip clip=new AnimClip("move");
        clip.setTracks(new AnimTrack[]{new TransformTrack(root,new float[]{0,1},new Vector3f[]{new Vector3f(1,0,0),new Vector3f(3,0,0)},null,null)});
        AnimComposer composer=new AnimComposer(); composer.addAnimClip(clip); model.addControl(composer);
        composer.setCurrentAction("move"); ((BlendableAction)composer.getCurrentAction()).setTransitionLength(0);
        composer.setTime(.5);
        SkinningControl skin=new SkinningControl(arm); skin.setHardwareSkinningPreferred(false); model.addControl(skin);
        model.updateLogicalState(0); model.updateGeometricState();
        need(Math.abs(root.getLocalTranslation().x-2)<1e-5f);
        java.lang.reflect.Method deform=SkinningControl.class.getDeclaredMethod("applySoftwareSkinning",Mesh.class,Matrix4f[].class);
        deform.setAccessible(true); deform.invoke(skin,mesh,arm.computeSkinningMatrices());
        FloatBuffer out=(FloatBuffer)mesh.getBuffer(Type.Position).getData();
        float[] expected={3,0,0,4,0,0,3,1,0};
        for(int i=0;i<expected.length;i++) need(Math.abs(out.get(i)-expected[i])<1e-5f);
        need(!arm.getJoint(1).getInverseModelBindMatrix().isIdentity());
        System.out.println("BASELINE_SKIN_OK vertices=3 shift=1.0");
    }
}
