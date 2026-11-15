/*
 * TypeAnnotationsAttribute.java
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

package com.strobel.assembler.ir.attributes;

import com.strobel.assembler.metadata.typeannotations.TypeAnnotation;

import java.util.Collections;
import java.util.List;

/**
 * A decoded {@code RuntimeVisibleTypeAnnotations} or {@code RuntimeInvisibleTypeAnnotations}
 * attribute (JVMS 4.7.20).
 *
 * <p>Entries are retained in their original class file order.</p>
 */
public final class TypeAnnotationsAttribute extends SourceAttribute {
    private final List<TypeAnnotation> _annotations;

    public TypeAnnotationsAttribute(final String name, final int length, final List<TypeAnnotation> annotations) {
        super(name, length);
        _annotations = Collections.unmodifiableList(annotations);
    }

    public List<TypeAnnotation> getAnnotations() {
        return _annotations;
    }
}
