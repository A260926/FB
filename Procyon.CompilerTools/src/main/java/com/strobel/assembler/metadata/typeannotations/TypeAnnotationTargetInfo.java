/*
 * TypeAnnotationTargetInfo.java
 *
 * Copyright (c) 2024 Mike Strobel
 *
 * This source code is subject to terms and conditions of the Apache License, Version 2.0.
 * A copy of the license can be found in the License.html file at the root of this distribution.
 * By using this source code in any fashion, you are agreeing to be bound by the terms of the
 * Apache License, Version 2.0.
 *
 * You must not remove this notice, or any other, from this software.
 */

package com.strobel.assembler.metadata.typeannotations;

/**
 * The decoded {@code target_info} union of a type annotation (JVMS 4.7.20).
 *
 * <p>Each concrete subclass mirrors one of the {@code target_info} structures in
 * tables 4.7.20-D through 4.7.20-K.  All index values are the raw class file values;
 * validation against the declaring member is performed when the annotations are
 * attached to the decompiled AST, not while reading.</p>
 */
public abstract class TypeAnnotationTargetInfo {

    public abstract TypeAnnotationTargetKind getKind();

    /**
     * The zero-based index of the type parameter, formal parameter, throws clause,
     * bound or type argument named by this target, or {@code -1} when the target
     * structure carries no such index.
     */
    public int getPrimaryIndex() {
        return -1;
    }

    @Override
    public String toString() {
        return getKind().name();
    }

    /** target_info for {@code type_parameter_target} (targets 0x00, 0x01). */
    public static final class TypeParameterTarget extends TypeAnnotationTargetInfo {
        private final TypeAnnotationTargetKind _kind;
        private final int _parameterIndex;

        public TypeParameterTarget(final TypeAnnotationTargetKind kind, final int parameterIndex) {
            _kind = kind;
            _parameterIndex = parameterIndex;
        }

        @Override
        public TypeAnnotationTargetKind getKind() {
            return _kind;
        }

        public int getParameterIndex() {
            return _parameterIndex;
        }

        @Override
        public int getPrimaryIndex() {
            return _parameterIndex;
        }

        @Override
        public String toString() {
            return _kind.name() + "(parameterIndex=" + _parameterIndex + ")";
        }
    }

    /** target_info for {@code supertype_target} (target 0x10). */
    public static final class SupertypeTarget extends TypeAnnotationTargetInfo {
        /** JVMS sentinel identifying the direct superclass rather than an interface. */
        public static final int SUPERCLASS_INDEX = 65535;

        private final int _supertypeIndex;

        public SupertypeTarget(final int supertypeIndex) {
            _supertypeIndex = supertypeIndex;
        }

        @Override
        public TypeAnnotationTargetKind getKind() {
            return TypeAnnotationTargetKind.CLASS_EXTENDS;
        }

        public int getSupertypeIndex() {
            return _supertypeIndex;
        }

        public boolean isSuperclass() {
            return _supertypeIndex == SUPERCLASS_INDEX;
        }

        @Override
        public int getPrimaryIndex() {
            return _supertypeIndex;
        }

        @Override
        public String toString() {
            return "CLASS_EXTENDS(supertypeIndex=" +
                   (isSuperclass() ? "superclass" : Integer.toString(_supertypeIndex)) + ")";
        }
    }

    /** target_info for {@code type_parameter_bound_target} (targets 0x11, 0x12). */
    public static final class TypeParameterBoundTarget extends TypeAnnotationTargetInfo {
        private final TypeAnnotationTargetKind _kind;
        private final int _parameterIndex;
        private final int _boundIndex;

        public TypeParameterBoundTarget(
            final TypeAnnotationTargetKind kind,
            final int parameterIndex,
            final int boundIndex) {

            _kind = kind;
            _parameterIndex = parameterIndex;
            _boundIndex = boundIndex;
        }

        @Override
        public TypeAnnotationTargetKind getKind() {
            return _kind;
        }

        public int getParameterIndex() {
            return _parameterIndex;
        }

        public int getBoundIndex() {
            return _boundIndex;
        }

        @Override
        public int getPrimaryIndex() {
            return _parameterIndex;
        }

        @Override
        public String toString() {
            return _kind.name() + "(parameterIndex=" + _parameterIndex +
                   ", boundIndex=" + _boundIndex + ")";
        }
    }

    /** target_info for {@code empty_target} (targets 0x13, 0x14, 0x15). */
    public static final class EmptyTarget extends TypeAnnotationTargetInfo {
        private final TypeAnnotationTargetKind _kind;

        public EmptyTarget(final TypeAnnotationTargetKind kind) {
            _kind = kind;
        }

        @Override
        public TypeAnnotationTargetKind getKind() {
            return _kind;
        }
    }

    /** target_info for {@code formal_parameter_target} (target 0x16). */
    public static final class FormalParameterTarget extends TypeAnnotationTargetInfo {
        private final int _formalParameterIndex;

        public FormalParameterTarget(final int formalParameterIndex) {
            _formalParameterIndex = formalParameterIndex;
        }

        @Override
        public TypeAnnotationTargetKind getKind() {
            return TypeAnnotationTargetKind.FORMAL_PARAMETER;
        }

        /**
         * The raw {@code formal_parameter_index}; this is a source-level parameter index
         * that still has to be reconciled with hidden/synthetic parameters before use.
         */
        public int getFormalParameterIndex() {
            return _formalParameterIndex;
        }

        @Override
        public int getPrimaryIndex() {
            return _formalParameterIndex;
        }

        @Override
        public String toString() {
            return "FORMAL_PARAMETER(formalParameterIndex=" + _formalParameterIndex + ")";
        }
    }

    /** target_info for {@code throws_target} (target 0x17). */
    public static final class ThrowsTarget extends TypeAnnotationTargetInfo {
        private final int _throwsTypeIndex;

        public ThrowsTarget(final int throwsTypeIndex) {
            _throwsTypeIndex = throwsTypeIndex;
        }

        @Override
        public TypeAnnotationTargetKind getKind() {
            return TypeAnnotationTargetKind.THROWS;
        }

        public int getThrowsTypeIndex() {
            return _throwsTypeIndex;
        }

        @Override
        public int getPrimaryIndex() {
            return _throwsTypeIndex;
        }

        @Override
        public String toString() {
            return "THROWS(throwsTypeIndex=" + _throwsTypeIndex + ")";
        }
    }

    /** One {@code localvar_target} table row (JVMS table 4.7.20-I). */
    public static final class LocalVariableEntry {
        private final int _startPc;
        private final int _length;
        private final int _index;

        public LocalVariableEntry(final int startPc, final int length, final int index) {
            _startPc = startPc;
            _index = index;
            _length = length;
        }

        public int getStartPc() {
            return _startPc;
        }

        public int getLength() {
            return _length;
        }

        public int getIndex() {
            return _index;
        }

        @Override
        public String toString() {
            return "{startPc=" + _startPc + ", length=" + _length + ", index=" + _index + "}";
        }
    }

    /** target_info for {@code localvar_target} (targets 0x40, 0x41). */
    public static final class LocalVariableTarget extends TypeAnnotationTargetInfo {
        private final TypeAnnotationTargetKind _kind;
        private final java.util.List<LocalVariableEntry> _entries;

        public LocalVariableTarget(
            final TypeAnnotationTargetKind kind,
            final java.util.List<LocalVariableEntry> entries) {

            _kind = kind;
            _entries = java.util.Collections.unmodifiableList(
                new java.util.ArrayList<>(entries)
            );
        }

        @Override
        public TypeAnnotationTargetKind getKind() {
            return _kind;
        }

        public java.util.List<LocalVariableEntry> getEntries() {
            return _entries;
        }

        @Override
        public String toString() {
            return _kind.name() + _entries;
        }
    }

    /** target_info for {@code catch_target} (target 0x42). */
    public static final class CatchTarget extends TypeAnnotationTargetInfo {
        private final int _exceptionTableIndex;

        public CatchTarget(final int exceptionTableIndex) {
            _exceptionTableIndex = exceptionTableIndex;
        }

        @Override
        public TypeAnnotationTargetKind getKind() {
            return TypeAnnotationTargetKind.EXCEPTION_PARAMETER;
        }

        public int getExceptionTableIndex() {
            return _exceptionTableIndex;
        }

        @Override
        public int getPrimaryIndex() {
            return _exceptionTableIndex;
        }

        @Override
        public String toString() {
            return "EXCEPTION_PARAMETER(exceptionTableIndex=" + _exceptionTableIndex + ")";
        }
    }

    /** target_info for {@code offset_target} (targets 0x43-0x46). */
    public static final class OffsetTarget extends TypeAnnotationTargetInfo {
        private final TypeAnnotationTargetKind _kind;
        private final int _offset;

        public OffsetTarget(final TypeAnnotationTargetKind kind, final int offset) {
            _kind = kind;
            _offset = offset;
        }

        @Override
        public TypeAnnotationTargetKind getKind() {
            return _kind;
        }

        public int getOffset() {
            return _offset;
        }

        @Override
        public String toString() {
            return _kind.name() + "(offset=" + _offset + ")";
        }
    }

    /** target_info for {@code type_argument_target} (targets 0x47-0x4B). */
    public static final class TypeArgumentTarget extends TypeAnnotationTargetInfo {
        private final TypeAnnotationTargetKind _kind;
        private final int _offset;
        private final int _typeArgumentIndex;

        public TypeArgumentTarget(
            final TypeAnnotationTargetKind kind,
            final int offset,
            final int typeArgumentIndex) {

            _kind = kind;
            _offset = offset;
            _typeArgumentIndex = typeArgumentIndex;
        }

        @Override
        public TypeAnnotationTargetKind getKind() {
            return _kind;
        }

        public int getOffset() {
            return _offset;
        }

        public int getTypeArgumentIndex() {
            return _typeArgumentIndex;
        }

        @Override
        public int getPrimaryIndex() {
            return _typeArgumentIndex;
        }

        @Override
        public String toString() {
            return _kind.name() + "(offset=" + _offset +
                   ", typeArgumentIndex=" + _typeArgumentIndex + ")";
        }
    }
}
