/*
 * TypeAnnotationFormatException.java
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
 * Thrown when a {@code RuntimeVisibleTypeAnnotations} or
 * {@code RuntimeInvisibleTypeAnnotations} attribute violates JVMS 4.7.20:
 * an unknown {@code target_type} or {@code type_path_kind}, a non-zero reserved
 * byte, an index outside the attribute bounds, or a truncated blob.
 */
public final class TypeAnnotationFormatException extends IllegalStateException {
    private static final long serialVersionUID = 1L;

    public TypeAnnotationFormatException(final String message) {
        super(message);
    }

    public TypeAnnotationFormatException(final String message, final Throwable cause) {
        super(message, cause);
    }
}
