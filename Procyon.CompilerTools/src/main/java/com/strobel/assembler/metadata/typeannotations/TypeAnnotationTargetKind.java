/*
 * TypeAnnotationTargetKind.java
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
 * The {@code target_type} values defined in JVMS 4.7.20, table 4.7.20-A.
 *
 * <p>The numeric codes are the exact bytes stored in the class file; the {@link #isEmittable()}
 * flag distinguishes targets whose annotations can be written back into Java source from
 * targets (receiver and {@code Code} attribute targets) that are recorded but not emitted.</p>
 */
public enum TypeAnnotationTargetKind {
    //
    // Targets that occur outside of the Code attribute (JVMS table 4.7.20-B).
    //
    CLASS_TYPE_PARAMETER(0x00, true),
    METHOD_TYPE_PARAMETER(0x01, true),
    CLASS_EXTENDS(0x10, true),
    CLASS_TYPE_PARAMETER_BOUND(0x11, true),
    METHOD_TYPE_PARAMETER_BOUND(0x12, true),
    FIELD(0x13, true),
    METHOD_RETURN(0x14, true),
    METHOD_RECEIVER(0x15, false),
    FORMAL_PARAMETER(0x16, true),
    THROWS(0x17, true),

    //
    // Targets that occur inside the Code attribute (JVMS table 4.7.20-C).  Their
    // target_info structures are fully read and retained, but the annotations are
    // not written into decompiled source.
    //
    LOCAL_VARIABLE(0x40, false),
    RESOURCE_VARIABLE(0x41, false),
    EXCEPTION_PARAMETER(0x42, false),
    INSTANCEOF(0x43, false),
    NEW(0x44, false),
    CONSTRUCTOR_REFERENCE(0x45, false),
    METHOD_REFERENCE(0x46, false),
    CAST(0x47, false),
    CONSTRUCTOR_INVOCATION_TYPE_ARGUMENT(0x48, false),
    METHOD_INVOCATION_TYPE_ARGUMENT(0x49, false),
    CONSTRUCTOR_REFERENCE_TYPE_ARGUMENT(0x4A, false),
    METHOD_REFERENCE_TYPE_ARGUMENT(0x4B, false);

    private final int _code;
    private final boolean _emittable;

    TypeAnnotationTargetKind(final int code, final boolean emittable) {
        _code = code;
        _emittable = emittable;
    }

    public int getCode() {
        return _code;
    }

    /**
     * @return {@code true} for targets supported in decompiled Java source; {@code false} for
     *         the receiver target and all {@code Code} attribute targets, which are retained
     *         as queryable metadata only.
     */
    public boolean isEmittable() {
        return _emittable;
    }

    public static TypeAnnotationTargetKind fromCode(final int code) {
        for (final TypeAnnotationTargetKind kind : values()) {
            if (kind._code == code) {
                return kind;
            }
        }

        throw new IllegalStateException(
            "Invalid type annotation target_type: 0x" + Integer.toHexString(code)
        );
    }
}
