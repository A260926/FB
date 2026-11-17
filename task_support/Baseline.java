package task;
import java.util.Arrays;
import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;
import org.objectweb.asm.tree.analysis.*;
public final class Baseline implements Opcodes {
 public static void main(String[] args)throws Exception {
  byte[] bytes=Fixture.ordinary(),copy=bytes.clone();ClassNode c=Fixture.read(bytes);MethodNode m=c.methods.get(0);
  Frame<BasicValue>[] f=new Analyzer<BasicValue>(new BasicInterpreter()).analyze(c.name,m);
  int at=-1;for(int i=0;i<m.instructions.size();i++)if(m.instructions.get(i).getOpcode()==INVOKESPECIAL)at=i;
  if(at<0||f[at].getStackSize()!=1||f[at].getLocal(0)!=f[at].getStack(0)||!Arrays.equals(bytes,copy))throw new AssertionError("baseline");
  System.out.println("BASELINE method="+m.name+" instructions="+m.instructions.size());
  System.out.println("BEFORE local0="+f[at].getLocal(0)+" stack0="+f[at].getStack(0));
  System.out.println("AFTER local0="+f[at+1].getLocal(0));
  System.out.println("BASELINE_OK");
 }
}
