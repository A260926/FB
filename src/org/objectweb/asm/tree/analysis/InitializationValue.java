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

import org.objectweb.asm.Type;
import org.objectweb.asm.tree.TypeInsnNode;

/**
 * An immutable {@link Value} that tracks the initialization state of object
 * references, as computed by {@link InitializationInterpreter}.
 *
 * A value is either {@link Kind#TOP} (no information, e.g. an empty slot or
 * the result of an incompatible merge), {@link Kind#VALUE} (an initialized
 * value of some type), {@link Kind#NEW} (a reference produced by a given NEW
 * instruction, whose constructor has not yet returned normally) or
 * {@link Kind#UNINITIALIZED_THIS} (the <code>this</code> reference of a
 * constructor, before a superclass or sibling constructor has returned
 * normally). Copies of a value made by DUP, ALOAD, ASTORE and similar
 * instructions share the same identity, so that all the aliases of an
 * uninitialized reference can be initialized at once when its constructor
 * returns normally.
 *
 * @author Eric Bruneton
 */
public final class InitializationValue implements Value {

    /**
     * The kind of an {@link InitializationValue}.
     */
    public enum Kind {

        /**
         * No information: an empty slot, or the merge of incompatible values.
         * The type of such a value is <tt>null</tt>.
         */
        TOP,

        /**
         * An initialized value, of any type.
         */
        VALUE,

        /**
         * A reference produced by a NEW instruction, whose constructor has
         * not yet returned normally. The allocation site of such a value is
         * the NEW instruction itself.
         */
        NEW,

        /**
         * The <code>this</code> reference of a constructor, before a
         * superclass or sibling constructor has returned normally.
         */
        UNINITIALIZED_THIS
    }

    private static final InitializationValue TOP = new InitializationValue(
            Kind.TOP, null, null);

    private final Kind kind;

    private final Type type;

    private final TypeInsnNode allocation;

    private InitializationValue(final Kind kind, final Type type,
            final TypeInsnNode allocation) {
        this.kind = kind;
        this.type = type;
        this.allocation = allocation;
    }

    /**
     * Returns the TOP value, which carries no type information.
     *
     * @return the TOP value.
     */
    public static InitializationValue top() {
        return TOP;
    }

    /**
     * Returns an initialized value of the given type.
     *
     * @param type
     *            the type of the value, or <tt>null</tt> as a synonym for
     *            the TOP value.
     * @return an initialized value of the given type.
     */
    public static InitializationValue value(final Type type) {
        if (type == null) {
            return top();
        }
        return new InitializationValue(Kind.VALUE, type, null);
    }

    /**
     * Returns an uninitialized reference produced by the given NEW
     * instruction. The identity of the returned value is the instruction
     * itself: two distinct NEW instructions, even of the same type, produce
     * distinct values.
     *
     * @param insn
     *            a NEW instruction.
     * @return the uninitialized reference it produces.
     */
    public static InitializationValue newInstance(final TypeInsnNode insn) {
        return new InitializationValue(Kind.NEW, Type.getObjectType(insn.desc),
                insn);
    }

    /**
     * Returns the uninitialized <code>this</code> reference of a constructor
     * of the given class.
     *
     * @param type
     *            the class to which the constructor belongs.
     * @return the uninitialized <code>this</code> reference of its
     *         constructors.
     */
    public static InitializationValue uninitializedThis(final Type type) {
        return new InitializationValue(Kind.UNINITIALIZED_THIS, type, null);
    }

    /**
     * Returns the kind of this value.
     *
     * @return the kind of this value.
     */
    public Kind getKind() {
        return kind;
    }

    /**
     * Returns the type of this value: <tt>null</tt> for TOP values, the
     * allocated type for NEW values, the class to which the constructor
     * belongs for UNINITIALIZED_THIS values, and the type of the value itself
     * otherwise.
     *
     * @return the type of this value.
     */
    public Type getType() {
        return type;
    }

    /**
     * Returns the NEW instruction that allocated this value.
     *
     * @return the NEW instruction that allocated this value, or <tt>null</tt>
     *         if this value is not an uninitialized NEW reference (in
     *         particular, once it has been initialized).
     */
    public TypeInsnNode getAllocation() {
        return allocation;
    }

    public int getSize() {
        return type == Type.LONG_TYPE || type == Type.DOUBLE_TYPE ? 2 : 1;
    }

    @Override
    public boolean equals(final Object value) {
        if (value == this) {
            return true;
        } else if (value instanceof InitializationValue) {
            InitializationValue other = (InitializationValue) value;
            return kind == other.kind && allocation == other.allocation
                    && (type == null ? other.type == null : type
                            .equals(other.type));
        } else {
            return false;
        }
    }

    @Override
    public int hashCode() {
        int hash = kind.hashCode();
        hash = 31 * hash + (type == null ? 0 : type.hashCode());
        hash = 31 * hash
                + (allocation == null ? 0 : System.identityHashCode(allocation));
        return hash;
    }

    @Override
    public String toString() {
        switch (kind) {
        case TOP:
            return ".";
        case NEW:
            return "NEW(" + type.getInternalName() + "@"
                    + Integer.toHexString(System.identityHashCode(allocation))
                    + ")";
        case UNINITIALIZED_THIS:
            return "UNINITIALIZED_THIS(" + type.getInternalName() + ")";
        default:
            return type.getDescriptor();
        }
    }
}
