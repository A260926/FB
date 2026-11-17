package task;
import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;
/** Emits inert, valid legacy class data; never defines or executes a class. */
public final class Fixture implements Opcodes {
 public static byte[] ordinary() {
  ClassWriter w=new ClassWriter(ClassWriter.COMPUTE_MAXS);
  w.visit(V1_5,ACC_PUBLIC,"task/Example",null,"java/lang/Object",null);
  MethodVisitor m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"factory","()Ljava/lang/Object;",null,null);
  m.visitCode();m.visitTypeInsn(NEW,"java/lang/Object");m.visitInsn(DUP);m.visitVarInsn(ASTORE,0);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V");
  m.visitVarInsn(ALOAD,0);m.visitInsn(ARETURN);m.visitMaxs(0,0);m.visitEnd();w.visitEnd();
  return w.toByteArray();
 }
 public static ClassNode read(byte[] data) {
  ClassNode c=new ClassNode();new ClassReader(data).accept(c,0);return c;
 }
}
