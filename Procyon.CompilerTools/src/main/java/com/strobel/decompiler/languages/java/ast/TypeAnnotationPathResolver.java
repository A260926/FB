/*
 * TypeAnnotationPathResolver.java
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

import com.strobel.assembler.metadata.TypeDefinition;
import com.strobel.assembler.metadata.TypeReference;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotationPath;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotationPlacementException;

/**
 * Interprets a JVMS 4.7.20 {@code type_path} against the reconstructed Java type tree.
 *
 * <p>The four path kinds combine as follows:
 * <ul>
 *   <li>{@code ARRAY_ELEMENT} crosses one array dimension of a {@link ComposedType}, landing on
 *       a specific {@link ArraySpecifier} bracket or, once all dimensions are crossed, the
 *       component base type;</li>
 *   <li>{@code INNER_TYPE} advances one dotted nested-type segment.  A signature only contains
 *       a dot where an enclosing class is itself parameterized, so the number of path
 *       {@code INNER_TYPE} steps is the count of parameterized owner types, not the full nested
 *       class chain;</li>
 *   <li>{@code WILDCARD_BOUND} moves from a {@link WildcardType} to its extends/super bound;</li>
 *   <li>{@code TYPE_ARGUMENT} selects one indexed type argument.</li>
 * </ul>
 * A step with no corresponding node raises {@link TypeAnnotationPlacementException}; an
 * annotation is never re-attached to an outer node to make it fit.</p>
 */
public final class TypeAnnotationPathResolver {

    /** A resolved attachment point: an {@link AstType} usage, a member segment, or a bracket. */
    public static final class Location {
        public final AstNode node;
        /**
         * Zero-based index of the dotted identifier part this annotation belongs to when
         * {@code node} is a member {@link SimpleType}; {@code 0} otherwise.
         */
        public final int memberSegment;

        public Location(final AstNode node, final int memberSegment) {
            this.node = node;
            this.memberSegment = memberSegment;
        }
    }

    private TypeAnnotationPathResolver() {
    }

    public static Location resolve(final AstType root, final TypeAnnotationPath path) {
        return place(root, path, 0, 0);
    }

    private static Location place(
        final AstType node,
        final TypeAnnotationPath path,
        final int pathIndex,
        final int innerDepth) {

        if (pathIndex == path.size()) {
            return anchor(node, innerDepth);
        }

        final TypeAnnotationPath.Entry step = path.get(pathIndex);

        switch (step.getKind()) {
            case ARRAY_ELEMENT: {
                if (!(node instanceof ComposedType)) {
                    throw fail(path, pathIndex, "ARRAY_ELEMENT on non-array type " + describe(node));
                }

                final ComposedType composedType = (ComposedType) node;
                final AstNodeCollection<ArraySpecifier> specifiers = composedType.getArraySpecifiers();
                final int dimensionCount = specifiers.size();

                int run = 0;

                while (pathIndex + run < path.size() &&
                       path.get(pathIndex + run).getKind() == TypeAnnotationPath.Kind.ARRAY_ELEMENT) {
                    run++;
                }

                if (run > dimensionCount) {
                    throw fail(
                        path,
                        pathIndex,
                        "ARRAY_ELEMENT depth " + run + " exceeds array dimension count " + dimensionCount
                    );
                }

                final int nextIndex = pathIndex + run;

                if (run == dimensionCount) {
                    // Every bracket crossed: remaining steps address the component type.
                    return place(composedType.getBaseType(), path, nextIndex, innerDepth);
                }

                if (nextIndex == path.size()) {
                    return new Location(
                        specifiers.toArray(new ArraySpecifier[0])[run],
                        memberSegmentOf(node, innerDepth)
                    );
                }

                throw fail(
                    path,
                    nextIndex,
                    "type_path continues inside an array dimension that has no nested type node"
                );
            }

            case INNER_TYPE: {
                if (!(node instanceof SimpleType)) {
                    throw fail(path, pathIndex, "INNER_TYPE on non-member type " + describe(node));
                }

                final SimpleType simpleType = (SimpleType) node;
                final int signatureDots = countSignatureDots(simpleType);
                final int newInnerDepth = innerDepth + 1;

                if (newInnerDepth > signatureDots) {
                    throw fail(
                        path,
                        pathIndex,
                        "INNER_TYPE depth " + newInnerDepth +
                        " exceeds the " + signatureDots + " dotted member segment(s) of " +
                        simpleType.getIdentifier()
                    );
                }

                return place(simpleType, path, pathIndex + 1, newInnerDepth);
            }

            case WILDCARD_BOUND: {
                if (!(node instanceof WildcardType)) {
                    throw fail(path, pathIndex, "WILDCARD_BOUND on non-wildcard type " + describe(node));
                }

                final WildcardType wildcard = (WildcardType) node;
                final AstType bound;

                if (!wildcard.getExtendsBounds().isEmpty()) {
                    bound = wildcard.getExtendsBounds().firstOrNullObject();
                }
                else if (!wildcard.getSuperBounds().isEmpty()) {
                    bound = wildcard.getSuperBounds().firstOrNullObject();
                }
                else {
                    throw fail(
                        path,
                        pathIndex,
                        "WILDCARD_BOUND on unbounded wildcard, which has no represented bound type"
                    );
                }

                return place(bound, path, pathIndex + 1, innerDepth);
            }

            case TYPE_ARGUMENT: {
                if (!(node instanceof SimpleType)) {
                    throw fail(
                        path,
                        pathIndex,
                        "TYPE_ARGUMENT on type without type arguments: " + describe(node)
                    );
                }

                final SimpleType simpleType = (SimpleType) node;

                //
                // The reconstructed SimpleType renders only the leaf segment's type arguments.
                // A TYPE_ARGUMENT is representable only when the path is positioned on that leaf,
                // i.e. after every signature dot (parameterized owner) has been crossed.
                //
                if (innerDepth != countSignatureDots(simpleType)) {
                    throw fail(
                        path,
                        pathIndex,
                        "TYPE_ARGUMENT targets an owner member segment whose type arguments were " +
                        "not preserved in the reconstructed type tree"
                    );
                }

                final int argumentIndex = step.getTypeArgumentIndex();
                final AstNodeCollection<AstType> arguments = simpleType.getTypeArguments();

                if (argumentIndex < 0 || argumentIndex >= arguments.size()) {
                    throw fail(
                        path,
                        pathIndex,
                        "TYPE_ARGUMENT index " + argumentIndex +
                        " out of range (have " + arguments.size() + " argument(s))"
                    );
                }

                // Descending into an argument restarts the path coordinate system.
                return place(
                    arguments.toArray(new AstType[0])[argumentIndex],
                    path,
                    pathIndex + 1,
                    0
                );
            }
        }

        throw fail(path, pathIndex, "unhandled path kind " + step.getKind());
    }

    /** An empty path on an array type names its outermost dimension, per JVMS 4.7.20. */
    private static Location anchor(final AstType node, final int innerDepth) {
        if (node instanceof ComposedType) {
            final ComposedType composedType = (ComposedType) node;

            if (!composedType.getArraySpecifiers().isEmpty()) {
                return new Location(
                    composedType.getArraySpecifiers().firstOrNullObject(),
                    memberSegmentOf(node, innerDepth)
                );
            }
        }

        return new Location(node, memberSegmentOf(node, innerDepth));
    }

    /**
     * Produces the segment marker stored on an annotation.
     *
     * <p>The SimpleType identifier is unqualified at placement time and only gains its
     * package/member prefix later in {@code AddReferenceQualifiersTransform}, so an absolute part
     * index cannot be computed yet.  Instead this returns the number of dotted nested-type
     * segments <em>inward from the leaf</em> that the annotation addresses: {@code 0} for the leaf
     * itself (including an empty {@code type_path}), increasing by one per {@code INNER_TYPE}
     * step.  {@link #resolvePartIndex} turns that marker into an absolute identifier index once
     * the final qualified name exists.</p>
     */
    private static int memberSegmentOf(final AstNode node, final int innerDepth) {
        return innerDepth;
    }

    /**
     * Converts a leaf-relative segment marker to an absolute index into the final, fully
     * qualified identifier.  The marker counts inward from the leaf, so the absolute index is the
     * total part count minus one minus the marker.
     */
    public static int resolvePartIndex(final SimpleType simpleType, final int leafRelative) {
        final int partCount = countIdentifierParts(simpleType);
        final int index = partCount - 1 - leafRelative;
        return index >= 0 ? index : 0;
    }

    private static int countIdentifierParts(final SimpleType simpleType) {
        final String identifier = simpleType.getIdentifier();
        int count = 1;

        for (int i = 0; i < identifier.length(); i++) {
            if (identifier.charAt(i) == '.') {
                count++;
            }
        }

        return count;
    }

    /**
     * @return the number of signature dots above this node.  A generic signature contains a dot
     *         ({@code Owner<...>.Inner}) only where an enclosing class is parameterized.  A static
     *         nested class never captures its owner's type arguments, so a generic static enclosing
     *         class does not introduce a dot and does not count as a path {@code INNER_TYPE} step.
     */
    private static int countSignatureDots(final SimpleType simpleType) {
        final TypeReference leaf = simpleType.getUserData(Keys.TYPE_REFERENCE);

        if (leaf == null) {
            return 0;
        }

        int dots = 0;
        TypeReference current = leaf;

        // Walk from the leaf upward; the leaf itself is the first parameterized segment.
        while (current != null && current.isNested()) {
            final TypeReference owner = current.getDeclaringType();

            if (owner == null) {
                break;
            }

            final TypeDefinition resolvedOwner = owner.resolve();
            final TypeDefinition resolvedCurrent = current.resolve();

            final boolean genericOwner =
                resolvedOwner != null && !resolvedOwner.getGenericParameters().isEmpty();

            final boolean currentIsStatic =
                resolvedCurrent != null && resolvedCurrent.isStatic();

            //
            // A non-static nested type carries its (possibly parameterized) owner in the
            // signature, producing a dot; a static nested type never does.
            //
            if (genericOwner && !currentIsStatic) {
                dots++;
            }

            current = owner;
        }

        return dots;
    }

    private static String describe(final AstType node) {
        final TypeReference reference = node.getUserData(Keys.TYPE_REFERENCE);
        return reference != null ? reference.getBriefDescription() : node.getClass().getSimpleName();
    }

    private static TypeAnnotationPlacementException fail(
        final TypeAnnotationPath path,
        final int pathIndex,
        final String reason) {

        return new TypeAnnotationPlacementException(
            "Type annotation path " + path + " cannot be placed at step " + pathIndex + ": " + reason
        );
    }
}
