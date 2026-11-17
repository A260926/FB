/***
 * ASM: a very small and fast Java bytecode manipulation framework
 * Copyright (c) 2000-2011 INRIA, France Telecom
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
package org.objectweb.asm.tree.analysis;

import java.util.List;

import org.objectweb.asm.Handle;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.Type;
import org.objectweb.asm.tree.AbstractInsnNode;
import org.objectweb.asm.tree.FieldInsnNode;
import org.objectweb.asm.tree.IntInsnNode;
import org.objectweb.asm.tree.InvokeDynamicInsnNode;
import org.objectweb.asm.tree.LdcInsnNode;
import org.objectweb.asm.tree.MethodInsnNode;
import org.objectweb.asm.tree.MethodNode;
import org.objectweb.asm.tree.MultiANewArrayInsnNode;
import org.objectweb.asm.tree.TypeInsnNode;

/**
 * An {@link Interpreter} for {@link InitializationValue} values, which tracks
 * how object references are allocated and initialized.
 *
 * The types and slot sizes produced for the ordinary bytecode instructions
 * follow the same conventions as {@link BasicInterpreter}, except that
 * reference types are kept precisely instead of being collapsed to
 * <code>java/lang/Object</code>. On top of this, a NEW instruction produces
 * an {@link InitializationValue.Kind#NEW} value whose identity is the
 * instruction itself, the <code>this</code> parameter of a constructor is an
 * {@link InitializationValue.Kind#UNINITIALIZED_THIS} value, and when an
 * <code>invokespecial &lt;init&gt;</code> invocation returns normally, all
 * the live aliases of its receiver, on the stack and in the local variables,
 * become {@link InitializationValue.Kind#VALUE} values of the allocated type
 * (or of the class to which the constructor belongs, for
 * <code>this</code>). This interpreter does not load any class: the merge of
 * two distinct reference types is <code>java/lang/Object</code>.
 *
 * @author Eric Bruneton
 */
public class InitializationInterpreter extends
        Interpreter<InitializationValue> implements Opcodes {

    private static final Type OBJECT_TYPE = Type
            .getObjectType("java/lang/Object");

    public InitializationInterpreter() {
        super(ASM5);
    }

    protected InitializationInterpreter(final int api) {
        super(api);
    }

    @Override
    public InitializationValue newValue(final Type type) {
        if (type == null) {
            return InitializationValue.top();
        }
        switch (type.getSort()) {
        case Type.VOID:
            return null;
        case Type.BOOLEAN:
        case Type.CHAR:
        case Type.BYTE:
        case Type.SHORT:
        case Type.INT:
        case Type.FLOAT:
        case Type.LONG:
        case Type.DOUBLE:
        case Type.ARRAY:
        case Type.OBJECT:
            return InitializationValue.value(type);
        default:
            throw new Error("Internal error");
        }
    }

    @Override
    public InitializationValue newEntryValue(final Type type,
            final String owner, final MethodNode m, final int local) {
        if (local == 0 && type != null && (m.access & ACC_STATIC) == 0
                && "<init>".equals(m.name)) {
            return InitializationValue.uninitializedThis(type);
        }
        return newValue(type);
    }

    @Override
    public InitializationValue newOperation(final AbstractInsnNode insn)
            throws AnalyzerException {
        switch (insn.getOpcode()) {
        case ACONST_NULL:
            return InitializationValue.value(Type.getObjectType("null"));
        case ICONST_M1:
        case ICONST_0:
        case ICONST_1:
        case ICONST_2:
        case ICONST_3:
        case ICONST_4:
        case ICONST_5:
        case BIPUSH:
        case SIPUSH:
            return InitializationValue.value(Type.INT_TYPE);
        case LCONST_0:
        case LCONST_1:
            return InitializationValue.value(Type.LONG_TYPE);
        case FCONST_0:
        case FCONST_1:
        case FCONST_2:
            return InitializationValue.value(Type.FLOAT_TYPE);
        case DCONST_0:
        case DCONST_1:
            return InitializationValue.value(Type.DOUBLE_TYPE);
        case LDC:
            Object cst = ((LdcInsnNode) insn).cst;
            if (cst instanceof Integer) {
                return InitializationValue.value(Type.INT_TYPE);
            } else if (cst instanceof Float) {
                return InitializationValue.value(Type.FLOAT_TYPE);
            } else if (cst instanceof Long) {
                return InitializationValue.value(Type.LONG_TYPE);
            } else if (cst instanceof Double) {
                return InitializationValue.value(Type.DOUBLE_TYPE);
            } else if (cst instanceof String) {
                return InitializationValue.value(Type
                        .getObjectType("java/lang/String"));
            } else if (cst instanceof Type) {
                int sort = ((Type) cst).getSort();
                if (sort == Type.OBJECT || sort == Type.ARRAY) {
                    return InitializationValue.value(Type
                            .getObjectType("java/lang/Class"));
                } else if (sort == Type.METHOD) {
                    return InitializationValue.value(Type
                            .getObjectType("java/lang/invoke/MethodType"));
                } else {
                    throw new IllegalArgumentException("Illegal LDC constant "
                            + cst);
                }
            } else if (cst instanceof Handle) {
                return InitializationValue.value(Type
                        .getObjectType("java/lang/invoke/MethodHandle"));
            } else {
                throw new IllegalArgumentException("Illegal LDC constant "
                        + cst);
            }
        case JSR:
            return InitializationValue.value(Type.VOID_TYPE);
        case GETSTATIC:
            return InitializationValue.value(Type
                    .getType(((FieldInsnNode) insn).desc));
        case NEW:
            return InitializationValue.newInstance((TypeInsnNode) insn);
        default:
            throw new Error("Internal error.");
        }
    }

    @Override
    public InitializationValue copyOperation(final AbstractInsnNode insn,
            final InitializationValue value) throws AnalyzerException {
        return value;
    }

    @Override
    public InitializationValue unaryOperation(final AbstractInsnNode insn,
            final InitializationValue value) throws AnalyzerException {
        switch (insn.getOpcode()) {
        case INEG:
        case IINC:
        case L2I:
        case F2I:
        case D2I:
        case I2B:
        case I2C:
        case I2S:
            return InitializationValue.value(Type.INT_TYPE);
        case FNEG:
        case I2F:
        case L2F:
        case D2F:
            return InitializationValue.value(Type.FLOAT_TYPE);
        case LNEG:
        case I2L:
        case F2L:
        case D2L:
            return InitializationValue.value(Type.LONG_TYPE);
        case DNEG:
        case I2D:
        case L2D:
        case F2D:
            return InitializationValue.value(Type.DOUBLE_TYPE);
        case IFEQ:
        case IFNE:
        case IFLT:
        case IFGE:
        case IFGT:
        case IFLE:
        case TABLESWITCH:
        case LOOKUPSWITCH:
        case IRETURN:
        case LRETURN:
        case FRETURN:
        case DRETURN:
        case ARETURN:
        case PUTSTATIC:
            return null;
        case GETFIELD:
            return InitializationValue.value(Type
                    .getType(((FieldInsnNode) insn).desc));
        case NEWARRAY:
            switch (((IntInsnNode) insn).operand) {
            case T_BOOLEAN:
                return InitializationValue.value(Type.getType("[Z"));
            case T_CHAR:
                return InitializationValue.value(Type.getType("[C"));
            case T_BYTE:
                return InitializationValue.value(Type.getType("[B"));
            case T_SHORT:
                return InitializationValue.value(Type.getType("[S"));
            case T_INT:
                return InitializationValue.value(Type.getType("[I"));
            case T_FLOAT:
                return InitializationValue.value(Type.getType("[F"));
            case T_DOUBLE:
                return InitializationValue.value(Type.getType("[D"));
            case T_LONG:
                return InitializationValue.value(Type.getType("[J"));
            default:
                throw new AnalyzerException(insn, "Invalid array type");
            }
        case ANEWARRAY:
            String desc = ((TypeInsnNode) insn).desc;
            return InitializationValue.value(Type.getType("["
                    + Type.getObjectType(desc)));
        case ARRAYLENGTH:
            return InitializationValue.value(Type.INT_TYPE);
        case ATHROW:
            return null;
        case CHECKCAST:
            desc = ((TypeInsnNode) insn).desc;
            return InitializationValue.value(Type.getObjectType(desc));
        case INSTANCEOF:
            return InitializationValue.value(Type.INT_TYPE);
        case MONITORENTER:
        case MONITOREXIT:
        case IFNULL:
        case IFNONNULL:
            return null;
        default:
            throw new Error("Internal error.");
        }
    }

    @Override
    public InitializationValue binaryOperation(final AbstractInsnNode insn,
            final InitializationValue value1, final InitializationValue value2)
            throws AnalyzerException {
        switch (insn.getOpcode()) {
        case IALOAD:
        case BALOAD:
        case CALOAD:
        case SALOAD:
        case IADD:
        case ISUB:
        case IMUL:
        case IDIV:
        case IREM:
        case ISHL:
        case ISHR:
        case IUSHR:
        case IAND:
        case IOR:
        case IXOR:
            return InitializationValue.value(Type.INT_TYPE);
        case FALOAD:
        case FADD:
        case FSUB:
        case FMUL:
        case FDIV:
        case FREM:
            return InitializationValue.value(Type.FLOAT_TYPE);
        case LALOAD:
        case LADD:
        case LSUB:
        case LMUL:
        case LDIV:
        case LREM:
        case LSHL:
        case LSHR:
        case LUSHR:
        case LAND:
        case LOR:
        case LXOR:
            return InitializationValue.value(Type.LONG_TYPE);
        case DALOAD:
        case DADD:
        case DSUB:
        case DMUL:
        case DDIV:
        case DREM:
            return InitializationValue.value(Type.DOUBLE_TYPE);
        case AALOAD:
            return InitializationValue.value(OBJECT_TYPE);
        case LCMP:
        case FCMPL:
        case FCMPG:
        case DCMPL:
        case DCMPG:
            return InitializationValue.value(Type.INT_TYPE);
        case IF_ICMPEQ:
        case IF_ICMPNE:
        case IF_ICMPLT:
        case IF_ICMPGE:
        case IF_ICMPGT:
        case IF_ICMPLE:
        case IF_ACMPEQ:
        case IF_ACMPNE:
        case PUTFIELD:
            return null;
        default:
            throw new Error("Internal error.");
        }
    }

    @Override
    public InitializationValue ternaryOperation(final AbstractInsnNode insn,
            final InitializationValue value1, final InitializationValue value2,
            final InitializationValue value3) throws AnalyzerException {
        return null;
    }

    @Override
    public InitializationValue naryOperation(final AbstractInsnNode insn,
            final List<? extends InitializationValue> values)
            throws AnalyzerException {
        int opcode = insn.getOpcode();
        if (opcode == MULTIANEWARRAY) {
            return newValue(Type.getType(((MultiANewArrayInsnNode) insn).desc));
        } else if (opcode == INVOKEDYNAMIC) {
            return newValue(Type
                    .getReturnType(((InvokeDynamicInsnNode) insn).desc));
        } else {
            return newValue(Type.getReturnType(((MethodInsnNode) insn).desc));
        }
    }

    @Override
    public InitializationValue invokeOperation(final AbstractInsnNode insn,
            final List<? extends InitializationValue> values,
            final Frame<InitializationValue> frame) throws AnalyzerException {
        InitializationValue result = naryOperation(insn, values);
        if (insn.getOpcode() == INVOKESPECIAL
                && "<init>".equals(((MethodInsnNode) insn).name)) {
            InitializationValue receiver = values.get(0);
            InitializationValue.Kind kind = receiver.getKind();
            if (kind == InitializationValue.Kind.NEW
                    || kind == InitializationValue.Kind.UNINITIALIZED_THIS) {
                // The constructor returned normally on this edge: all the
                // live aliases of the receiver become initialized values of
                // the allocated type (or of the class to which the
                // constructor belongs, for this), even when the invoked
                // constructor is a superclass one.
                InitializationValue initialized = InitializationValue
                        .value(receiver.getType());
                for (int i = 0; i < frame.getLocals(); ++i) {
                    if (receiver.equals(frame.getLocal(i))) {
                        frame.setLocal(i, initialized);
                    }
                }
                for (int i = 0; i < frame.getStackSize(); ++i) {
                    if (receiver.equals(frame.getStack(i))) {
                        frame.setStack(i, initialized);
                    }
                }
            }
        }
        return result;
    }

    @Override
    public void returnOperation(final AbstractInsnNode insn,
            final InitializationValue value, final InitializationValue expected)
            throws AnalyzerException {
    }

    @Override
    public InitializationValue merge(final InitializationValue v,
            final InitializationValue w) {
        if (v.equals(w)) {
            return v;
        }
        if (v.getKind() == InitializationValue.Kind.VALUE
                && w.getKind() == InitializationValue.Kind.VALUE) {
            Type vType = v.getType();
            Type wType = w.getType();
            if (isReference(vType) && isReference(wType)) {
                if (isNull(vType)) {
                    return w;
                } else if (isNull(wType)) {
                    return v;
                } else if (OBJECT_TYPE.equals(vType)) {
                    return v;
                } else if (OBJECT_TYPE.equals(wType)) {
                    return w;
                } else {
                    return InitializationValue.value(OBJECT_TYPE);
                }
            }
        }
        return InitializationValue.top();
    }

    private static boolean isReference(final Type type) {
        return type.getSort() == Type.OBJECT || type.getSort() == Type.ARRAY;
    }

    private static boolean isNull(final Type type) {
        return type.getSort() == Type.OBJECT
                && "null".equals(type.getInternalName());
    }
}
