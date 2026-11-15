/*
 * TypeAnnotation.java
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

import com.strobel.assembler.metadata.annotations.CustomAnnotation;
import com.strobel.core.VerifyArgument;

/**
 * One entry of a {@code RuntimeVisibleTypeAnnotations} or
 * {@code RuntimeInvisibleTypeAnnotations} attribute (JVMS 4.7.20).
 *
 * <p>The record preserves everything needed to distinguish different usages of the
 * same declared type: the owning declaration is held by the enclosing
 * {@link TypeAnnotationsAttribute}, while each annotation carries its {@code target_type},
 * full {@code target_info}, the raw {@code type_path}, its visibility, and the complete
 * annotation value (including member order).</p>
 */
public final class TypeAnnotation {

    /** Which of the two type annotation attributes this entry was read from. */
    public enum Visibility {
        /** From {@code RuntimeVisibleTypeAnnotations} ({@code RetentionPolicy.RUNTIME}). */
        RUNTIME,
        /** From {@code RuntimeInvisibleTypeAnnotations} ({@code RetentionPolicy.CLASS}). */
        CLASS
    }

    private final TypeAnnotationTargetKind _targetKind;
    private final TypeAnnotationTargetInfo _targetInfo;
    private final TypeAnnotationPath _path;
    private final Visibility _visibility;
    private final CustomAnnotation _annotation;

    public TypeAnnotation(
        final TypeAnnotationTargetKind targetKind,
        final TypeAnnotationTargetInfo targetInfo,
        final TypeAnnotationPath path,
        final Visibility visibility,
        final CustomAnnotation annotation) {

        _targetKind = VerifyArgument.notNull(targetKind, "targetKind");
        _targetInfo = VerifyArgument.notNull(targetInfo, "targetInfo");
        _path = VerifyArgument.notNull(path, "path");
        _visibility = VerifyArgument.notNull(visibility, "visibility");
        _annotation = VerifyArgument.notNull(annotation, "annotation");
    }

    /** The {@code target_type} byte, as a typed enum. */
    public TypeAnnotationTargetKind getTargetKind() {
        return _targetKind;
    }

    /** The decoded {@code target_info} union; never {@code null}. */
    public TypeAnnotationTargetInfo getTargetInfo() {
        return _targetInfo;
    }

    /** The raw {@code type_path}; an empty path is represented by {@link TypeAnnotationPath#EMPTY}. */
    public TypeAnnotationPath getPath() {
        return _path;
    }

    public Visibility getVisibility() {
        return _visibility;
    }

    /** The annotation value, with all members and their original order retained. */
    public CustomAnnotation getAnnotation() {
        return _annotation;
    }

    /**
     * @return {@code true} when this annotation can be written back into Java source;
     *         receiver and {@code Code} attribute targets are retained for queries only.
     */
    public boolean isEmittable() {
        return _targetKind.isEmittable();
    }

    @Override
    public String toString() {
        return "TypeAnnotation{" +
               "target=" + _targetInfo +
               ", path=" + _path +
               ", visibility=" + _visibility +
               ", annotation=" + _annotation.getAnnotationType().getFullName() +
               '}';
    }
}
