import com.badlogic.gdx.graphics.g3d.Model;
import com.badlogic.gdx.graphics.g3d.ModelInstance;
import com.badlogic.gdx.graphics.g3d.model.*;
import com.badlogic.gdx.graphics.g3d.utils.BaseAnimationController;
import com.badlogic.gdx.math.*;
import com.badlogic.gdx.utils.Array;
import com.badlogic.gdx.utils.ArrayMap;
public class Current {
 static class Player extends BaseAnimationController {
  Player(ModelInstance m){super(m);}
  void sample(Animation a,float t){applyAnimation(a,t);}
 }
 static void check(boolean b,String s){if(!b)throw new AssertionError(s);}
 public static void main(String[] args){
  Model model=new Model();Node root=new Node();root.id="root";
  Node hand=new Node();hand.id="hand";hand.translation.set(2,0,0);root.addChild(hand);
  model.nodes.add(root);model.calculateTransforms();
  ModelInstance instance=new ModelInstance(model);
  root=instance.getNode("root");hand=instance.getNode("hand");
  NodePart skin=new NodePart();skin.invBoneBindTransforms=new ArrayMap<>(Node.class,Matrix4.class);
  skin.invBoneBindTransforms.put(hand,new Matrix4().setToTranslation(-2,0,0));
  skin.bones=new Matrix4[]{new Matrix4()};root.parts.add(skin);
  Animation clip=new Animation();clip.id="turn";clip.duration=1;
  NodeAnimation track=new NodeAnimation();track.node=root;track.rotation=new Array<>();
  track.rotation.add(new NodeKeyframe<Quaternion>(0,new Quaternion()));
  track.rotation.add(new NodeKeyframe<Quaternion>(1,new Quaternion().setFromAxis(0,0,1,90)));
  clip.nodeAnimations.add(track);new Player(instance).sample(clip,.5f);
  Vector3 tip=new Vector3(3,0,0).mul(skin.bones[0]).mul(instance.transform);
  check(Math.abs(tip.x-3/Math.sqrt(2))<2e-5,"palette x");
  check(Math.abs(tip.y-3/Math.sqrt(2))<2e-5,"palette y");
  check(root.rotation.isIdentity(),"animation leaves base rotation alone");
  Quaternion actual=root.localTransform.getRotation(new Quaternion(),true);
  check(Math.abs(actual.getAngle()-45)<1e-3,"actual animated matrix");
  instance.transform.setToTranslation(5,0,0);
  Vector3 shifted=new Vector3(3,0,0).mul(skin.bones[0]).mul(instance.transform);
  check(Math.abs(shifted.x-tip.x-5)<2e-5,"world transform once");
  System.out.println("GDX_POSE_BASELINE_OK animated_local palette inverse_bind world");
 }
}
