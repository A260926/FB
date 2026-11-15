/*
 * TypeAnnotations.java
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

import com.strobel.assembler.ir.attributes.AttributeNames;
import com.strobel.assembler.ir.attributes.SourceAttribute;
import com.strobel.assembler.ir.attributes.TypeAnnotationsAttribute;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Helpers for reading the type-use annotation attributes attached to a declaration.
 *
 * <p>Both {@code RuntimeVisibleTypeAnnotations} and {@code RuntimeInvisibleTypeAnnotations}
 * are merged, preserving each attribute's internal order (visible entries first only when the
 * visible attribute appears first in the source).  These helpers never inspect declaration
 * ({@code RuntimeVisibleAnnotations}) attributes, so ordinary declaration annotations remain
 * entirely separate.</p>
 */
public final class TypeAnnotations {

    private TypeAnnotations() {
    }

    /**
     * @return all type-use annotations carried by the given source attributes, in attribute
     *         and entry order; an empty list when there are none.
     */
    public static List<TypeAnnotation> collect(final List<SourceAttribute> attributes) {
        if (attributes == null || attributes.isEmpty()) {
            return Collections.emptyList();
        }

        List<TypeAnnotation> result = null;

        for (final SourceAttribute attribute : attributes) {
            if (attribute instanceof TypeAnnotationsAttribute) {
                final List<TypeAnnotation> entries =
                    ((TypeAnnotationsAttribute) attribute).getAnnotations();

                if (!entries.isEmpty()) {
                    if (result == null) {
                        result = new ArrayList<>();
                    }
                    result.addAll(entries);
                }
            }
        }

        return result == null ? Collections.<TypeAnnotation>emptyList() : result;
    }

    public static boolean hasTypeAnnotations(final List<SourceAttribute> attributes) {
        if (attributes == null) {
            return false;
        }

        for (final SourceAttribute attribute : attributes) {
            if (attribute instanceof TypeAnnotationsAttribute &&
                !((TypeAnnotationsAttribute) attribute).getAnnotations().isEmpty()) {

                return true;
            }
        }

        return false;
    }

    public static boolean isTypeAnnotationAttributeName(final String name) {
        return AttributeNames.RuntimeVisibleTypeAnnotations.equals(name) ||
               AttributeNames.RuntimeInvisibleTypeAnnotations.equals(name);
    }
}
