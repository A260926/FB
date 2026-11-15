/*
 * TypeAnnotationReader.java
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

import com.strobel.assembler.ir.AnnotationReader;
import com.strobel.assembler.ir.attributes.AttributeNames;
import com.strobel.assembler.ir.attributes.TypeAnnotationsAttribute;
import com.strobel.assembler.metadata.Buffer;
import com.strobel.assembler.metadata.IMetadataScope;
import com.strobel.assembler.metadata.annotations.CustomAnnotation;

import java.nio.BufferUnderflowException;
import java.util.ArrayList;
import java.util.List;

/**
 * Decodes {@code RuntimeVisibleTypeAnnotations} and {@code RuntimeInvisibleTypeAnnotations}
 * attributes according to JVMS 4.7.20.
 *
 * <p>The reader works strictly from class file bytes: it never loads the input class,
 * runs annotation processors, or executes any of the input code.  Every multi-byte read is
 * confined to the attribute blob (a truncated blob surfaces as a
 * {@link TypeAnnotationFormatException}), and all indices and reserved bytes mandated to be
 * zero are validated.  Because each {@code target_info} and {@code type_path} variant is
 * decoded explicitly, a malformed entry cannot cause subsequent entries to be misread.</p>
 */
public final class TypeAnnotationReader {

    private TypeAnnotationReader() {
    }

    public static TypeAnnotationsAttribute read(
        final String attributeName,
        final int attributeLength,
        final IMetadataScope scope,
        final Buffer input) {

        final boolean visible;

        if (AttributeNames.RuntimeVisibleTypeAnnotations.equals(attributeName)) {
            visible = true;
        }
        else if (AttributeNames.RuntimeInvisibleTypeAnnotations.equals(attributeName)) {
            visible = false;
        }
        else {
            throw new TypeAnnotationFormatException(
                "Not a type annotation attribute: " + attributeName
            );
        }

        final TypeAnnotation.Visibility visibility = visible
            ? TypeAnnotation.Visibility.RUNTIME
            : TypeAnnotation.Visibility.CLASS;

        final BoundedBuffer buffer = new BoundedBuffer(input, attributeLength);

        try {
            final int annotationCount = buffer.readUnsignedShort();
            final List<TypeAnnotation> annotations = new ArrayList<>(annotationCount);

            for (int i = 0; i < annotationCount; i++) {
                annotations.add(readOne(scope, buffer, visibility));
            }

            buffer.assertFullyConsumed(attributeName);

            return new TypeAnnotationsAttribute(
                attributeName,
                attributeLength,
                annotations
            );
        }
        catch (final TypeAnnotationFormatException e) {
            throw e;
        }
        catch (final BufferUnderflowException e) {
            throw new TypeAnnotationFormatException(
                "Truncated " + attributeName + " attribute (declared length " +
                attributeLength + ")",
                e
            );
        }
    }

    private static TypeAnnotation readOne(
        final IMetadataScope scope,
        final BoundedBuffer buffer,
        final TypeAnnotation.Visibility visibility) {

        final int targetTypeCode = buffer.readUnsignedByte();
        final TypeAnnotationTargetKind kind;

        try {
            kind = TypeAnnotationTargetKind.fromCode(targetTypeCode);
        }
        catch (final IllegalStateException e) {
            throw new TypeAnnotationFormatException(e.getMessage(), e);
        }

        final TypeAnnotationTargetInfo targetInfo = readTargetInfo(kind, buffer);
        final TypeAnnotationPath path = readPath(buffer);
        final CustomAnnotation annotation = AnnotationReader.read(scope, buffer.rawBuffer());

        //
        // The generic annotation reader works against the underlying Buffer; make sure it
        // could not run past the end of this attribute's blob.
        //
        buffer.assertWithinLimit();

        return new TypeAnnotation(kind, targetInfo, path, visibility, annotation);
    }

    private static TypeAnnotationTargetInfo readTargetInfo(
        final TypeAnnotationTargetKind kind,
        final BoundedBuffer buffer) {

        switch (kind) {
            case CLASS_TYPE_PARAMETER:
            case METHOD_TYPE_PARAMETER: {
                final int parameterIndex = buffer.readUnsignedByte();
                return new TypeAnnotationTargetInfo.TypeParameterTarget(kind, parameterIndex);
            }

            case CLASS_EXTENDS: {
                final int supertypeIndex = buffer.readUnsignedShort();
                return new TypeAnnotationTargetInfo.SupertypeTarget(supertypeIndex);
            }

            case CLASS_TYPE_PARAMETER_BOUND:
            case METHOD_TYPE_PARAMETER_BOUND: {
                final int parameterIndex = buffer.readUnsignedByte();
                final int boundIndex = buffer.readUnsignedByte();
                return new TypeAnnotationTargetInfo.TypeParameterBoundTarget(
                    kind,
                    parameterIndex,
                    boundIndex
                );
            }

            case FIELD:
            case METHOD_RETURN:
            case METHOD_RECEIVER: {
                return new TypeAnnotationTargetInfo.EmptyTarget(kind);
            }

            case FORMAL_PARAMETER: {
                final int formalParameterIndex = buffer.readUnsignedByte();
                return new TypeAnnotationTargetInfo.FormalParameterTarget(formalParameterIndex);
            }

            case THROWS: {
                final int throwsTypeIndex = buffer.readUnsignedShort();
                return new TypeAnnotationTargetInfo.ThrowsTarget(throwsTypeIndex);
            }

            case LOCAL_VARIABLE:
            case RESOURCE_VARIABLE: {
                final int tableLength = buffer.readUnsignedShort();

                //
                // Each localvar table entry is exactly six bytes; reject a count that the
                // remaining attribute blob cannot possibly hold before indexing into it.
                //
                buffer.requireBytes(tableLength * 6, kind.name());

                final List<TypeAnnotationTargetInfo.LocalVariableEntry> entries =
                    new ArrayList<>(tableLength);

                for (int i = 0; i < tableLength; i++) {
                    final int startPc = buffer.readUnsignedShort();
                    final int length = buffer.readUnsignedShort();
                    final int index = buffer.readUnsignedShort();

                    entries.add(
                        new TypeAnnotationTargetInfo.LocalVariableEntry(startPc, length, index)
                    );
                }

                return new TypeAnnotationTargetInfo.LocalVariableTarget(kind, entries);
            }

            case EXCEPTION_PARAMETER: {
                final int exceptionTableIndex = buffer.readUnsignedShort();
                return new TypeAnnotationTargetInfo.CatchTarget(exceptionTableIndex);
            }

            case INSTANCEOF:
            case NEW:
            case CONSTRUCTOR_REFERENCE:
            case METHOD_REFERENCE: {
                final int offset = buffer.readUnsignedShort();
                return new TypeAnnotationTargetInfo.OffsetTarget(kind, offset);
            }

            case CAST:
            case CONSTRUCTOR_INVOCATION_TYPE_ARGUMENT:
            case METHOD_INVOCATION_TYPE_ARGUMENT:
            case CONSTRUCTOR_REFERENCE_TYPE_ARGUMENT:
            case METHOD_REFERENCE_TYPE_ARGUMENT: {
                final int offset = buffer.readUnsignedShort();
                final int typeArgumentIndex = buffer.readUnsignedByte();
                return new TypeAnnotationTargetInfo.TypeArgumentTarget(
                    kind,
                    offset,
                    typeArgumentIndex
                );
            }
        }

        throw new TypeAnnotationFormatException(
            "Unsupported target_type while reading target_info: " + kind
        );
    }

    private static TypeAnnotationPath readPath(final BoundedBuffer buffer) {
        final int pathLength = buffer.readUnsignedByte();

        if (pathLength == 0) {
            return TypeAnnotationPath.EMPTY;
        }

        buffer.requireBytes(pathLength * 2, "type_path");

        final List<TypeAnnotationPath.Entry> entries = new ArrayList<>(pathLength);

        for (int i = 0; i < pathLength; i++) {
            final int kindCode = buffer.readUnsignedByte();
            final int typeArgumentIndex = buffer.readUnsignedByte();

            final TypeAnnotationPath.Kind pathKind;

            try {
                pathKind = TypeAnnotationPath.Kind.fromCode(kindCode);
            }
            catch (final IllegalStateException e) {
                throw new TypeAnnotationFormatException(e.getMessage(), e);
            }

            //
            // JVMS 4.7.20.2: type_argument_index is zero unless the path kind is
            // TYPE_ARGUMENT (3).  A non-zero value in the reserved position is malformed.
            //
            if (pathKind != TypeAnnotationPath.Kind.TYPE_ARGUMENT && typeArgumentIndex != 0) {
                throw new TypeAnnotationFormatException(
                    "Reserved type_argument_index must be zero for path kind " +
                    pathKind + ", but was " + typeArgumentIndex
                );
            }

            entries.add(new TypeAnnotationPath.Entry(pathKind, typeArgumentIndex));
        }

        return new TypeAnnotationPath(entries);
    }

    /**
     * Limits every read to the bytes belonging to one type annotation attribute, so a
     * malformed count can never pull bytes from a following attribute.
     */
    private static final class BoundedBuffer {
        private final Buffer _buffer;
        private final int _start;
        private final int _limit;

        BoundedBuffer(final Buffer buffer, final int attributeLength) {
            _buffer = buffer;
            _start = buffer.position();
            _limit = _start + attributeLength;
        }

        Buffer rawBuffer() {
            return _buffer;
        }

        int position() {
            return _buffer.position();
        }

        void assertWithinLimit() {
            requireBytes(0, "annotation value");
        }

        int readUnsignedByte() {
            requireBytes(1, "u1");
            return _buffer.readUnsignedByte();
        }

        int readUnsignedShort() {
            requireBytes(2, "u2");
            return _buffer.readUnsignedShort();
        }

        void requireBytes(final int count, final String what) {
            if (count < 0 || _buffer.position() + count > _limit) {
                throw new TypeAnnotationFormatException(
                    "Type annotation attribute is too short to read " + what +
                    " (need " + count + " more bytes)"
                );
            }
        }

        void assertFullyConsumed(final String attributeName) {
            if (_buffer.position() != _limit) {
                throw new TypeAnnotationFormatException(
                    attributeName + " has " + (_limit - _buffer.position()) +
                    " trailing byte(s); the attribute blob is malformed"
                );
            }
        }
    }
}
