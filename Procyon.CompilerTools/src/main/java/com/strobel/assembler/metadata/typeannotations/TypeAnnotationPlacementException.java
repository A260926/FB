/*
 * TypeAnnotationPlacementException.java
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
 * Thrown when an emittable type annotation cannot be attached to the decompiled type tree.
 *
 * <p>This covers out-of-range targets (parameter, throws clause, supertype, type parameter
 * or bound indices) and {@code type_path} steps that do not correspond to any node in the
 * reconstructed AST (for example an array dimension on a non-array type, a wildcard step on
 * a non-wildcard, or an inner-type argument that the type tree does not represent).  The
 * annotation is never silently moved to an outer type.</p>
 */
public final class TypeAnnotationPlacementException extends IllegalStateException {
    private static final long serialVersionUID = 1L;

    public TypeAnnotationPlacementException(final String message) {
        super(message);
    }
}
