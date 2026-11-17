package task;
import java.util.*;
import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;
import org.objectweb.asm.tree.analysis.*;
import org.objectweb.asm.tree.analysis.InitializationValue.Kind;
/** Contract checks for InitializationInterpreter: allocation identity, alias initialization, merge rules. */
public final class InitializationContract implements Opcodes {

 public static TaskCheck.Case[] cases() {
  return new TaskCheck.Case[]{
   new TaskCheck.Case("aliases","stackAndLocalAliases",InitializationContract::stackAndLocalAliases),
   new TaskCheck.Case("aliases","twoAllocationsSameType",InitializationContract::twoAllocationsSameType),
   new TaskCheck.Case("aliases","constructorWideArgs",InitializationContract::constructorWideArgs),
   new TaskCheck.Case("flow","normalVsExceptionalSuccessors",InitializationContract::normalVsExceptionalSuccessors),
   new TaskCheck.Case("flow","branchJoins",InitializationContract::branchJoins),
   new TaskCheck.Case("flow","loopFixpoint",InitializationContract::loopFixpoint),
   new TaskCheck.Case("flow","crossMethodReuse",InitializationContract::crossMethodReuse),
   new TaskCheck.Case("compatibility","basicInterpreterUnchanged",InitializationContract::basicInterpreterUnchanged),
   new TaskCheck.Case("compatibility","sourceInterpreterUnchanged",InitializationContract::sourceInterpreterUnchanged),
   new TaskCheck.Case("compatibility","defaultExtensionPoints",InitializationContract::defaultExtensionPoints),
   new TaskCheck.Case("compatibility","plainInstructionTypes",InitializationContract::plainInstructionTypes),
  };
 }

 // ---------------------------------------------------------------- aliases

 /** NEW/DUP/ASTORE share one identity; invokespecial <init> initializes every alias at once. */
 static void stackAndLocalAliases(TaskCheck.Check c) throws Exception {
  ClassNode node=Fixture.read(Fixture.ordinary());MethodNode m=method(node,"factory");
  Frame<InitializationValue>[] f=analyze(node,m);
  AbstractInsnNode allocation=m.instructions.get(0);
  c.check(f[0].getLocal(0).getKind()==Kind.TOP&&f[0].getLocal(0).getType()==null,"empty slot is TOP");
  c.check(f[1].getStack(0).getKind()==Kind.NEW,"NEW pushes uninitialized reference");
  c.check(f[1].getStack(0).getAllocation()==allocation,"NEW carries the TypeInsnNode identity");
  c.check("java/lang/Object".equals(type(f[1].getStack(0))),"NEW type is the allocated type");
  c.check(f[2].getStack(0)==f[2].getStack(1),"DUP propagates the same source");
  c.check(f[3].getLocal(0)==f[3].getStack(0),"ASTORE/ALOAD propagate the same source");
  c.check(f[3].getLocal(0).getKind()==Kind.NEW&&f[3].getLocal(0).getAllocation()==allocation,"local alias still uninitialized before <init>");
  c.check(f[4].getLocal(0).getKind()==Kind.VALUE,"local alias initialized after <init>");
  c.check("java/lang/Object".equals(type(f[4].getLocal(0))),"initialized alias keeps the allocated type");
  c.check(f[4].getLocal(0).getAllocation()==null,"getAllocation() is null after initialization");
  c.check(f[5].getStack(0).getKind()==Kind.VALUE,"returned value is initialized");
 }

 /** Two NEW instructions of the same type are distinct identities and initialize independently. */
 static void twoAllocationsSameType(TaskCheck.Check c) throws Exception {
  ClassNode node=Fixture.read(twoNew());MethodNode m=method(node,"make2");
  Frame<InitializationValue>[] f=analyze(node,m);
  c.check(f[2].getStack(0).getKind()==Kind.NEW&&f[2].getStack(0).getAllocation()==m.instructions.get(0),"first NEW identity");
  c.check(f[6].getStack(0).getKind()==Kind.NEW&&f[6].getStack(0).getAllocation()==m.instructions.get(4),"second NEW identity");
  c.check(!f[2].getStack(0).equals(f[6].getStack(0)),"same-type NEWs are not mixed up");
  c.check(f[3].getStack(0).getKind()==Kind.VALUE&&"java/lang/StringBuilder".equals(type(f[3].getStack(0))),"first instance initialized by its own <init>");
  c.check(f[6].getLocal(0).getKind()==Kind.VALUE,"first instance stays initialized while second is NEW");
  c.check(f[6].getStack(0).getKind()==Kind.NEW,"second NEW not affected by first <init>");
  c.check(f[8].getLocal(0).getKind()==Kind.VALUE&&f[8].getLocal(1).getKind()==Kind.VALUE,"both initialized at return");
  c.check("java/lang/StringBuilder".equals(type(f[8].getLocal(1))),"second initialized type");
 }

 /** Constructor entry: local0 is UNINITIALIZED_THIS, wide arguments occupy two slots, super <init> initializes this as the own class. */
 static void constructorWideArgs(TaskCheck.Check c) throws Exception {
  ClassNode node=Fixture.read(Fixture.point());MethodNode m=method(node,"<init>");
  Frame<InitializationValue>[] f=analyze(node,m);
  c.check(f[0].getLocal(0).getKind()==Kind.UNINITIALIZED_THIS,"constructor local0 is UNINITIALIZED_THIS");
  c.check("task/Point".equals(type(f[0].getLocal(0))),"UNINITIALIZED_THIS type is the own class");
  c.check(f[0].getLocal(1).getKind()==Kind.VALUE&&f[0].getLocal(1).getType()==Type.LONG_TYPE&&f[0].getLocal(1).getSize()==2,"long parameter");
  c.check(f[0].getLocal(2).getKind()==Kind.TOP,"second slot of long is TOP");
  c.check(f[0].getLocal(3).getKind()==Kind.VALUE&&f[0].getLocal(3).getType()==Type.DOUBLE_TYPE&&f[0].getLocal(3).getSize()==2,"double parameter");
  c.check(f[0].getLocal(4).getKind()==Kind.TOP,"second slot of double is TOP");
  c.check(f[2].getLocal(0).getKind()==Kind.VALUE,"this initialized after super <init>");
  c.check("task/Point".equals(type(f[2].getLocal(0))),"super constructor does not turn this into the super class");
  c.check(f[2].getLocal(0).getAllocation()==null,"this has no allocation site");
 }

 // ---------------------------------------------------------------- flow

 /** The exceptional edge keeps the pre-call frame; the normal successor sees initialized aliases. */
 static void normalVsExceptionalSuccessors(TaskCheck.Check c) throws Exception {
  ClassNode node=Fixture.read(guarded());MethodNode m=method(node,"make");
  Frame<InitializationValue>[] f=analyze(node,m);
  AbstractInsnNode allocation=m.instructions.get(0);
  c.check(f[4].getLocal(0).getKind()==Kind.NEW&&f[4].getLocal(0).getAllocation()==allocation,"local uninitialized before <init>");
  c.check(f[6].getLocal(0).getKind()==Kind.VALUE&&"java/lang/Object".equals(type(f[6].getLocal(0))),"normal successor initialized");
  c.check(f[8].getLocal(0).getKind()==Kind.NEW,"exception edge uses the pre-call frame");
  c.check(f[8].getLocal(0).getAllocation()==allocation,"exception edge keeps the allocation identity");
  c.check(f[8].getStack(0).getKind()==Kind.VALUE&&"java/lang/Throwable".equals(type(f[8].getStack(0))),"handler stack holds the exception");
  c.check(f[10].getLocal(1).getKind()==Kind.VALUE&&"java/lang/Throwable".equals(type(f[10].getLocal(1))),"exception stored in handler local");
 }

 /** Branch joins: same identity kept, distinct identities or mixed initialization become TOP, reference types merge without a class hierarchy. */
 static void branchJoins(TaskCheck.Check c) throws Exception {
  ClassNode node=Fixture.read(flow());
  Frame<InitializationValue>[] shared=analyze(node,method(node,"shared"));
  MethodNode sharedM=method(node,"shared");
  c.check(shared[8].getStack(0).getKind()==Kind.NEW&&shared[8].getStack(0).getAllocation()==sharedM.instructions.get(0),"same uninitialized identity kept at join");
  c.check(shared[8].getLocal(1).getKind()==Kind.NEW,"joined alias still uninitialized");
  c.check(shared[9].getLocal(1).getKind()==Kind.VALUE,"joined alias initialized after <init>");
  Frame<InitializationValue>[] mixed=analyze(node,method(node,"mixed"));
  c.check(mixed[10].getLocal(1).getKind()==Kind.TOP&&mixed[10].getLocal(1).getType()==null,"uninitialized mixed with initialized becomes TOP");
  Frame<InitializationValue>[] branch=analyze(node,method(node,"branch"));
  c.check(branch[13].getLocal(1).getKind()==Kind.VALUE&&"java/lang/Object".equals(type(branch[13].getLocal(1))),"distinct reference types merge to Object");
  Frame<InitializationValue>[] nullJoin=analyze(node,method(node,"nullJoin"));
  c.check(nullJoin[9].getLocal(1).getKind()==Kind.VALUE&&"java/lang/Object".equals(type(nullJoin[9].getLocal(1))),"null merged with reference yields the reference");
  Frame<InitializationValue>[] sameType=analyze(node,method(node,"sameType"));
  c.check(sameType[10].getLocal(1).getKind()==Kind.VALUE&&"java/lang/StringBuilder".equals(type(sameType[10].getLocal(1))),"same reference type kept at join");
 }

 /** A NEW inside a loop keeps a stable identity while the analysis iterates to a fixpoint. */
 static void loopFixpoint(TaskCheck.Check c) throws Exception {
  ClassNode node=Fixture.read(flow());MethodNode m=method(node,"loop");
  Frame<InitializationValue>[] f=analyze(node,m);
  c.check(f[0].getLocal(1).getKind()==Kind.TOP,"entry TOP merged with initialized back edge is TOP");
  c.check(f[5].getStack(0).getKind()==Kind.NEW&&f[5].getStack(0).getAllocation()==m.instructions.get(3),"NEW identity stable across loop iterations");
  c.check(f[6].getStack(0).getKind()==Kind.VALUE&&"java/lang/Object".equals(type(f[6].getStack(0))),"initialized inside the loop");
  c.check(f[7].getLocal(1).getKind()==Kind.VALUE,"local initialized on the back edge");
 }

 /** One analyzer and interpreter can analyze several methods in a row without leaking entry or NEW state. */
 static void crossMethodReuse(TaskCheck.Check c) throws Exception {
  ClassNode node=Fixture.read(reuse());
  Analyzer<InitializationValue> analyzer=new Analyzer<InitializationValue>(new InitializationInterpreter());
  MethodNode init=method(node,"<init>"),instance=method(node,"instanceMethod"),factory=method(node,"staticFactory");
  Frame<InitializationValue>[] f1=analyzer.analyze(node.name,init);
  c.check(f1[0].getLocal(0).getKind()==Kind.UNINITIALIZED_THIS&&"task/Reuse".equals(type(f1[0].getLocal(0))),"constructor entry this");
  c.check(f1[2].getLocal(0).getKind()==Kind.VALUE&&"task/Reuse".equals(type(f1[2].getLocal(0))),"constructor this initialized as own class");
  Frame<InitializationValue>[] f2=analyzer.analyze(node.name,instance);
  c.check(f2[0].getLocal(0).getKind()==Kind.VALUE&&"task/Reuse".equals(type(f2[0].getLocal(0))),"instance method this is VALUE, not UNINITIALIZED_THIS");
  c.check(f2[0].getLocal(1).getKind()==Kind.TOP,"unused local is TOP");
  Frame<InitializationValue>[] f3=analyzer.analyze(node.name,factory);
  c.check(f3[0].getLocal(0).getKind()==Kind.TOP,"static method has no this");
  c.check(!f2[2].getStack(0).equals(f3[2].getStack(0)),"NEW identities are per instruction");
  InitializationValue before=f1[1].getLocal(0);
  Frame<InitializationValue>[] again=analyzer.analyze(node.name,init);
  c.check(f1[0].getLocal(0).getKind()==Kind.UNINITIALIZED_THIS,"old frames not mutated by later analyze calls");
  c.check(before.getKind()==Kind.UNINITIALIZED_THIS,"old values not mutated by later instructions");
  c.check(again[0].getLocal(0).equals(f1[0].getLocal(0)),"re-analysis yields equal entry values");
  c.check(again[0].getLocal(0).getKind()==Kind.UNINITIALIZED_THIS,"constructor state not leaked across analyze calls");
 }

 // ---------------------------------------------------------------- compatibility

 /** BasicInterpreter frames and merge semantics are unchanged. */
 static void basicInterpreterUnchanged(TaskCheck.Check c) throws Exception {
  ClassNode node=Fixture.read(Fixture.ordinary());MethodNode m=method(node,"factory");
  Frame<BasicValue>[] f=new Analyzer<BasicValue>(new BasicInterpreter()).analyze(node.name,m);
  c.check(f[3].getStackSize()==1&&f[3].getLocal(0)==f[3].getStack(0),"basic frame shape at <init>");
  c.check(f[3].getLocal(0)==BasicValue.REFERENCE_VALUE&&f[4].getLocal(0)==BasicValue.REFERENCE_VALUE,"basic values stay plain references");
  BasicInterpreter i=new BasicInterpreter();
  c.check(i.newValue(null)==BasicValue.UNINITIALIZED_VALUE,"basic newValue(null)");
  c.check(i.merge(BasicValue.INT_VALUE,BasicValue.REFERENCE_VALUE)==BasicValue.UNINITIALIZED_VALUE,"basic merge of distinct values");
  c.check(BasicValue.LONG_VALUE.getSize()==2&&BasicValue.INT_VALUE.getSize()==1,"basic slot sizes");
 }

 /** SourceInterpreter frames are unchanged. */
 static void sourceInterpreterUnchanged(TaskCheck.Check c) throws Exception {
  ClassNode node=Fixture.read(Fixture.ordinary());MethodNode m=method(node,"factory");
  Frame<SourceValue>[] f=new Analyzer<SourceValue>(new SourceInterpreter()).analyze(node.name,m);
  c.check(f[3].getLocal(0).insns.size()==1&&f[3].getLocal(0).insns.contains(m.instructions.get(2)),"local sourced from ASTORE");
  c.check(f[3].getStack(0).insns.size()==1&&f[3].getStack(0).insns.contains(m.instructions.get(0)),"stack slot sourced from NEW");
  c.check(f[4].getLocal(0).insns.contains(m.instructions.get(2))&&f[4].getLocal(0).getSize()==1,"source survives <init>");
  c.check(new SourceInterpreter().newValue(null).getSize()==1,"source newValue(null) size");
 }

 /** The new extension points default to the historical behavior for the existing interpreters. */
 static void defaultExtensionPoints(TaskCheck.Check c) throws Exception {
  BasicInterpreter basic=new BasicInterpreter();
  c.check(basic.newEntryValue(Type.INT_TYPE,"task/X",new MethodNode(),0)==BasicValue.INT_VALUE,"newEntryValue defaults to newValue");
  c.check(basic.newEntryValue(null,"task/X",new MethodNode(),0)==BasicValue.UNINITIALIZED_VALUE,"newEntryValue defaults for null type");
  MethodInsnNode call=new MethodInsnNode(INVOKESTATIC,"task/X","f","()I");
  c.check(basic.invokeOperation(call,Collections.<BasicValue>emptyList(),new Frame<BasicValue>(0,1))==BasicValue.INT_VALUE,"invokeOperation defaults to naryOperation");
  SourceInterpreter source=new SourceInterpreter();
  c.check(source.newEntryValue(null,"task/X",new MethodNode(),0).getSize()==1,"source newEntryValue default");
  Frame<BasicValue> frame=new Frame<BasicValue>(1,1);
  frame.push(BasicValue.INT_VALUE);frame.setStack(0,BasicValue.FLOAT_VALUE);
  c.check(frame.getStack(0)==BasicValue.FLOAT_VALUE,"Frame.setStack replaces a stack slot");
  boolean thrown=false;
  try { frame.setStack(1,BasicValue.INT_VALUE); } catch(IndexOutOfBoundsException expected) { thrown=true; }
  c.check(thrown,"Frame.setStack bounds check");
 }

 /** Ordinary instruction types and slot sizes follow the ASM conventions. */
 static void plainInstructionTypes(TaskCheck.Check c) throws Exception {
  ClassNode node=Fixture.read(math());MethodNode m=method(node,"add");
  Frame<InitializationValue>[] f=analyze(node,m);
  c.check(f[0].getLocal(0).getKind()==Kind.VALUE&&f[0].getLocal(0).getType()==Type.INT_TYPE,"int parameter type");
  c.check(f[0].getLocal(1).getKind()==Kind.VALUE&&f[0].getLocal(1).getType()==Type.INT_TYPE,"second int parameter type");
  c.check(f[2].getStack(0).getType()==Type.INT_TYPE&&f[2].getStack(1).getType()==Type.INT_TYPE,"IADD operands typed int");
  c.check(f[3].getStack(0).getType()==Type.INT_TYPE&&f[3].getStack(0).getSize()==1,"IADD result typed int, size 1");
  Frame<BasicValue>[] basic=new Analyzer<BasicValue>(new BasicInterpreter()).analyze(node.name,m);
  c.check(basic[3].getStack(0)==BasicValue.INT_VALUE,"BasicInterpreter agrees on the convention");
 }

 // ---------------------------------------------------------------- fixtures

 static Frame<InitializationValue>[] analyze(ClassNode c,MethodNode m) throws Exception {
  return new Analyzer<InitializationValue>(new InitializationInterpreter()).analyze(c.name,m);
 }
 static MethodNode method(ClassNode c,String name) {
  for(MethodNode m:c.methods)if(m.name.equals(name))return m;
  throw new NoSuchElementException(name);
 }
 static String type(InitializationValue v){ return v.getType()==null?"null":v.getType().getInternalName(); }

 /** Two allocations of the same type, initialized independently. */
 static byte[] twoNew() {
  ClassWriter w=new ClassWriter(ClassWriter.COMPUTE_MAXS);
  w.visit(V1_5,ACC_PUBLIC,"task/Pair",null,"java/lang/Object",null);
  MethodVisitor m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"make2","()V",null,null);
  m.visitCode();
  m.visitTypeInsn(NEW,"java/lang/StringBuilder");m.visitInsn(DUP);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/StringBuilder","<init>","()V");m.visitVarInsn(ASTORE,0);
  m.visitTypeInsn(NEW,"java/lang/StringBuilder");m.visitInsn(DUP);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/StringBuilder","<init>","()V");m.visitVarInsn(ASTORE,1);
  m.visitInsn(RETURN);m.visitMaxs(0,0);m.visitEnd();w.visitEnd();
  return w.toByteArray();
 }

 /** A try/catch whose region covers only the invokespecial <init> instruction. */
 static byte[] guarded() {
  ClassWriter w=new ClassWriter(ClassWriter.COMPUTE_MAXS);
  w.visit(V1_5,ACC_PUBLIC,"task/Guarded",null,"java/lang/Object",null);
  MethodVisitor m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"make","()Ljava/lang/Object;",null,null);
  m.visitCode();
  Label l0=new Label(),l1=new Label(),l2=new Label();
  m.visitTryCatchBlock(l0,l1,l2,"java/lang/Throwable");
  m.visitTypeInsn(NEW,"java/lang/Object");m.visitInsn(DUP);m.visitVarInsn(ASTORE,0);
  m.visitLabel(l0);m.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V");
  m.visitLabel(l1);m.visitVarInsn(ALOAD,0);m.visitInsn(ARETURN);
  m.visitLabel(l2);m.visitVarInsn(ASTORE,1);m.visitInsn(ACONST_NULL);m.visitInsn(ARETURN);
  m.visitMaxs(0,0);m.visitEnd();w.visitEnd();
  return w.toByteArray();
 }

 /** Branch, join and loop shapes over NEW and initialized values. */
 static byte[] flow() {
  ClassWriter w=new ClassWriter(ClassWriter.COMPUTE_MAXS);
  w.visit(V1_5,ACC_PUBLIC,"task/Flow",null,"java/lang/Object",null);
  MethodVisitor m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"shared","(I)Ljava/lang/Object;",null,null);
  m.visitCode();Label sharedL=new Label();
  m.visitTypeInsn(NEW,"java/lang/Object");m.visitInsn(DUP);m.visitVarInsn(ASTORE,1);
  m.visitVarInsn(ILOAD,0);m.visitJumpInsn(IFEQ,sharedL);m.visitVarInsn(ILOAD,0);m.visitInsn(POP);
  m.visitLabel(sharedL);m.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V");
  m.visitVarInsn(ALOAD,1);m.visitInsn(ARETURN);m.visitMaxs(0,0);m.visitEnd();
  m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"mixed","(I)Ljava/lang/Object;",null,null);
  m.visitCode();Label mixedL1=new Label(),mixedL2=new Label();
  m.visitTypeInsn(NEW,"java/lang/Object");m.visitInsn(DUP);m.visitVarInsn(ASTORE,1);
  m.visitVarInsn(ILOAD,0);m.visitJumpInsn(IFEQ,mixedL1);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V");m.visitJumpInsn(GOTO,mixedL2);
  m.visitLabel(mixedL1);m.visitInsn(POP);
  m.visitLabel(mixedL2);m.visitVarInsn(ALOAD,1);m.visitInsn(ARETURN);m.visitMaxs(0,0);m.visitEnd();
  m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"branch","(I)Ljava/lang/Object;",null,null);
  m.visitCode();Label branchL1=new Label(),branchL2=new Label();
  m.visitVarInsn(ILOAD,0);m.visitJumpInsn(IFEQ,branchL1);
  m.visitTypeInsn(NEW,"java/lang/StringBuilder");m.visitInsn(DUP);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/StringBuilder","<init>","()V");m.visitVarInsn(ASTORE,1);
  m.visitJumpInsn(GOTO,branchL2);
  m.visitLabel(branchL1);m.visitTypeInsn(NEW,"java/lang/StringBuffer");m.visitInsn(DUP);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/StringBuffer","<init>","()V");m.visitVarInsn(ASTORE,1);
  m.visitLabel(branchL2);m.visitVarInsn(ALOAD,1);m.visitInsn(ARETURN);m.visitMaxs(0,0);m.visitEnd();
  m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"nullJoin","(I)Ljava/lang/Object;",null,null);
  m.visitCode();Label nullL=new Label();
  m.visitInsn(ACONST_NULL);m.visitVarInsn(ASTORE,1);m.visitVarInsn(ILOAD,0);m.visitJumpInsn(IFEQ,nullL);
  m.visitTypeInsn(NEW,"java/lang/Object");m.visitInsn(DUP);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V");m.visitVarInsn(ASTORE,1);
  m.visitLabel(nullL);m.visitVarInsn(ALOAD,1);m.visitInsn(ARETURN);m.visitMaxs(0,0);m.visitEnd();
  m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"sameType","(I)Ljava/lang/Object;",null,null);
  m.visitCode();Label sameL=new Label();
  m.visitTypeInsn(NEW,"java/lang/StringBuilder");m.visitInsn(DUP);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/StringBuilder","<init>","()V");m.visitVarInsn(ASTORE,1);
  m.visitVarInsn(ILOAD,0);m.visitJumpInsn(IFEQ,sameL);
  m.visitTypeInsn(NEW,"java/lang/StringBuilder");m.visitInsn(DUP);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/StringBuilder","<init>","()V");m.visitVarInsn(ASTORE,1);
  m.visitLabel(sameL);m.visitVarInsn(ALOAD,1);m.visitInsn(ARETURN);m.visitMaxs(0,0);m.visitEnd();
  m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"loop","(I)V",null,null);
  m.visitCode();Label loopL=new Label(),loopEnd=new Label();
  m.visitLabel(loopL);m.visitVarInsn(ILOAD,0);m.visitJumpInsn(IFEQ,loopEnd);
  m.visitTypeInsn(NEW,"java/lang/Object");m.visitInsn(DUP);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V");m.visitVarInsn(ASTORE,1);
  m.visitIincInsn(0,-1);m.visitJumpInsn(GOTO,loopL);
  m.visitLabel(loopEnd);m.visitInsn(RETURN);m.visitMaxs(0,0);m.visitEnd();
  w.visitEnd();
  return w.toByteArray();
 }

 /** A constructor, an instance method and a static factory in one class. */
 static byte[] reuse() {
  ClassWriter w=new ClassWriter(ClassWriter.COMPUTE_MAXS);
  w.visit(V1_5,ACC_PUBLIC,"task/Reuse",null,"java/lang/Object",null);
  MethodVisitor m=w.visitMethod(ACC_PUBLIC,"<init>","()V",null,null);
  m.visitCode();m.visitVarInsn(ALOAD,0);m.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V");
  m.visitInsn(RETURN);m.visitMaxs(0,0);m.visitEnd();
  m=w.visitMethod(ACC_PUBLIC,"instanceMethod","()Ljava/lang/Object;",null,null);
  m.visitCode();m.visitTypeInsn(NEW,"java/lang/Object");m.visitInsn(DUP);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V");m.visitVarInsn(ASTORE,1);
  m.visitVarInsn(ALOAD,1);m.visitInsn(ARETURN);m.visitMaxs(0,0);m.visitEnd();
  m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"staticFactory","()Ljava/lang/Object;",null,null);
  m.visitCode();m.visitTypeInsn(NEW,"java/lang/Object");m.visitInsn(DUP);
  m.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V");m.visitVarInsn(ASTORE,0);
  m.visitVarInsn(ALOAD,0);m.visitInsn(ARETURN);m.visitMaxs(0,0);m.visitEnd();
  w.visitEnd();
  return w.toByteArray();
 }

 /** Plain int arithmetic, for the type/slot-size conventions. */
 static byte[] math() {
  ClassWriter w=new ClassWriter(ClassWriter.COMPUTE_MAXS);
  w.visit(V1_5,ACC_PUBLIC,"task/Math",null,"java/lang/Object",null);
  MethodVisitor m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"add","(II)I",null,null);
  m.visitCode();m.visitVarInsn(ILOAD,0);m.visitVarInsn(ILOAD,1);m.visitInsn(IADD);m.visitInsn(IRETURN);
  m.visitMaxs(0,0);m.visitEnd();w.visitEnd();
  return w.toByteArray();
 }
}
