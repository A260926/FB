/*
 * TypeAnnotationUnsupportedException.java
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
 * Thrown when a structurally valid type annotation cannot be mapped to a unique source
 * construct, rather than because the class file is malformed.
 *
 * <p>Typical cases are a {@code formal_parameter_index} that cannot be reconciled with the
 * method's hidden (synthetic/mandated) parameters using the {@code MethodParameters},
 * {@code Signature} and inner-class information, or an emittable target whose {@code type_path}
 * addresses a type segment the reconstructed tree does not preserve.  The raw record remains
 * available through the declaration's queryable type-annotation metadata, so the entry can be
 * reported as unanalyzed instead of silently dropped or attached to the wrong location.</p>
 */
public final class TypeAnnotationUnsupportedException extends IllegalStateException {
    private static final long serialVersionUID = 1L;

    public TypeAnnotationUnsupportedException(final String message) {
        super(message);
    }
}
