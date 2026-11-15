/*
 * TypeAnnotationPlacer.java
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

package com.strobel.decompiler.languages.java.ast;

import com.strobel.assembler.metadata.typeannotations.TypeAnnotation;

import java.util.List;

/**
 * Attaches decoded type-use annotations (JVMS 4.7.20) to a reconstructed type tree.
 *
 * <p>Each renderable type-use {@link Annotation} is linked 1:1 to its raw
 * {@link TypeAnnotation} record through {@link Keys#SOURCE_TYPE_ANNOTATION} (and, for dotted
 * member types, {@link Keys#TYPE_ANNOTATION_SEGMENT}).  Because that link is ordinary user
 * data, it survives AST cloning, import collapsing and nested-name simplification, so two
 * usages of the same declared type keep distinct annotations.  Nodes are attached with
 * {@link Roles#TYPE_ANNOTATION}, keeping type-use annotations separate from declaration
 * annotations; nothing is written onto a shared metadata {@code TypeReference}.</p>
 */
public final class TypeAnnotationPlacer {

    private TypeAnnotationPlacer() {
    }

    /**
     * Place every emittable annotation addressed to one root type (field type, return type,
     * parameter type, throws type, supertype or a type-parameter bound).
     */
    public static void placeOnType(
        final AstType root,
        final List<TypeAnnotation> annotations,
        final AstBuilder builder) {

        if (root == null || root.isNull() || annotations.isEmpty()) {
            return;
        }

        for (final TypeAnnotation annotation : annotations) {
            if (!annotation.isEmittable()) {
                continue;
            }

            final TypeAnnotationPathResolver.Location location =
                TypeAnnotationPathResolver.resolve(root, annotation.getPath());

            attach(location, annotation, builder);
        }
    }

    /** Place an empty-path annotation directly on a type parameter declaration. */
    public static void placeOnTypeParameter(
        final TypeParameterDeclaration declaration,
        final TypeAnnotation annotation,
        final AstBuilder builder) {

        if (!annotation.getPath().isEmpty()) {
            throw new com.strobel.assembler.metadata.typeannotations.TypeAnnotationPlacementException(
                "Type parameter target carries a non-empty type_path " + annotation.getPath()
            );
        }

        addAnnotationNode(declaration, annotation, builder, 0);
    }

    private static void attach(
        final TypeAnnotationPathResolver.Location location,
        final TypeAnnotation annotation,
        final AstBuilder builder) {

        addAnnotationNode(location.node, annotation, builder, location.memberSegment);
    }

    private static void addAnnotationNode(
        final AstNode node,
        final TypeAnnotation record,
        final AstBuilder builder,
        final int memberSegment) {

        for (final Annotation existing : node.getChildrenByRole(Roles.TYPE_ANNOTATION)) {
            if (existing.getUserData(Keys.SOURCE_TYPE_ANNOTATION) == record) {
                return;
            }
        }

        final Annotation expression = builder.createAnnotation(record.getAnnotation());

        expression.putUserData(Keys.SOURCE_TYPE_ANNOTATION, record);
        expression.putUserData(Keys.TYPE_ANNOTATION_SEGMENT, memberSegment);
        node.addChild(expression, Roles.TYPE_ANNOTATION);
    }
}
