package task;
import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;
import org.objectweb.asm.tree.analysis.*;
/** Prints the frames on both sides of each constructor call, as tracked by InitializationInterpreter. */
public final class InitializationDemo implements Opcodes {
 public static void main(String[] args) throws Exception {
  demo(Fixture.read(Fixture.ordinary()));
  demo(Fixture.read(Fixture.point()));
 }
 static void demo(ClassNode c) throws Exception {
  for(MethodNode m:c.methods){
   Frame<InitializationValue>[] f=new Analyzer<InitializationValue>(new InitializationInterpreter()).analyze(c.name,m);
   System.out.println("METHOD "+c.name+"."+m.name+m.desc);
   for(int i=0;i<m.instructions.size();i++){
    AbstractInsnNode insn=m.instructions.get(i);
    if(insn.getOpcode()==INVOKESPECIAL&&"<init>".equals(((MethodInsnNode)insn).name)){
     System.out.println("  before <init> "+frame(f[i]));
     System.out.println("  after  <init> "+frame(f[i+1]));
    }
   }
  }
 }
 static String frame(Frame<InitializationValue> f){
  StringBuilder b=new StringBuilder("locals=[");
  for(int i=0;i<f.getLocals();i++){if(i>0)b.append(", ");b.append(i).append(':').append(f.getLocal(i));}
  b.append("] stack=[");
  for(int i=0;i<f.getStackSize();i++){if(i>0)b.append(", ");b.append(f.getStack(i));}
  return b.append(']').toString();
 }
}
