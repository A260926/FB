/*
 * TypeAnnotationTransform.java
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

package com.strobel.decompiler.languages.java.ast.transforms;

import com.strobel.assembler.ir.attributes.SourceAttribute;
import com.strobel.assembler.metadata.FieldDefinition;
import com.strobel.assembler.metadata.MethodDefinition;
import com.strobel.assembler.metadata.ParameterDefinition;
import com.strobel.assembler.metadata.TypeDefinition;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotation;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotationTargetInfo;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotationTargetKind;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotations;
import com.strobel.decompiler.DecompilerContext;
import com.strobel.decompiler.languages.java.ast.AstBuilder;
import com.strobel.decompiler.languages.java.ast.AstNode;
import com.strobel.decompiler.languages.java.ast.AstNodeCollection;
import com.strobel.decompiler.languages.java.ast.AstType;
import com.strobel.decompiler.languages.java.ast.ConstructorDeclaration;
import com.strobel.decompiler.languages.java.ast.FieldDeclaration;
import com.strobel.decompiler.languages.java.ast.Keys;
import com.strobel.decompiler.languages.java.ast.MethodDeclaration;
import com.strobel.decompiler.languages.java.ast.ParameterDeclaration;
import com.strobel.decompiler.languages.java.ast.TypeAnnotationParameterMapper;
import com.strobel.decompiler.languages.java.ast.TypeAnnotationPlacer;
import com.strobel.decompiler.languages.java.ast.TypeDeclaration;
import com.strobel.decompiler.languages.java.ast.TypeParameterDeclaration;

import java.util.ArrayList;
import java.util.List;

/**
 * Reconstructs type-use annotations (JVMS 4.7.20) on the freshly built AST.
 *
 * <p>The transform runs before import collapsing, nested-name simplification and the record
 * rewrite, reading only the type-annotation attributes already decoded from the class file.
 * Receiver and {@code Code} targets are retained on the declaration's queryable metadata but
 * never written into source.  Declaration annotations are untouched.</p>
 */
public final class TypeAnnotationTransform implements IAstTransform {

    private final AstBuilder _builder;

    public TypeAnnotationTransform(final DecompilerContext context) {
        _builder = context.getUserData(Keys.AST_BUILDER);
    }

    /**
     * Runs one placement, routing a structurally-valid-but-unrepresentable type annotation to
     * the declaration's {@link Keys#UNSUPPORTED_TYPE_ANNOTATIONS} record instead of aborting the
     * whole decompilation.  Malformed blobs still fail earlier in the reader.  The annotation is
     * never rebound to an outer type.
     */
    private void safely(final AstNode declaration, final TypeAnnotation annotation, final Runnable action) {
        try {
            action.run();
        }
        catch (final com.strobel.assembler.metadata.typeannotations.TypeAnnotationPlacementException e) {
            recordUnsupported(declaration, annotation);
        }
        catch (final com.strobel.assembler.metadata.typeannotations.TypeAnnotationUnsupportedException e) {
            recordUnsupported(declaration, annotation);
        }
    }

    private static void recordUnsupported(
        final AstNode declaration,
        final TypeAnnotation annotation) {

        List<TypeAnnotation> unsupported = declaration.getUserData(Keys.UNSUPPORTED_TYPE_ANNOTATIONS);

        if (unsupported == null) {
            unsupported = new ArrayList<>();
            declaration.putUserData(Keys.UNSUPPORTED_TYPE_ANNOTATIONS, unsupported);
        }

        unsupported.add(annotation);
    }

    @Override
    public void run(final AstNode compilationUnit) {
        if (_builder == null) {
            return;
        }

        for (final AstNode child : compilationUnit.getChildren()) {
            if (child instanceof TypeDeclaration) {
                processTypeDeclaration((TypeDeclaration) child);
            }
        }
    }

    //
    // Type declarations: type parameters/bounds, superclass and interfaces.
    //
    private void processTypeDeclaration(final TypeDeclaration declaration) {
        final TypeDefinition definition = declaration.getUserData(Keys.TYPE_DEFINITION);

        if (definition != null) {
            final List<TypeAnnotation> all = TypeAnnotations.collect(definition.getSourceAttributes());

            if (!all.isEmpty()) {
                declaration.putUserData(Keys.DECLARATION_TYPE_ANNOTATIONS, new ArrayList<>(all));

                for (final TypeAnnotation annotation : all) {
                    safely(declaration, annotation, new Runnable() {
                        @Override
                        public void run() {
                            applyTypeTarget(declaration, annotation);
                        }
                    });
                }
            }
        }

        for (final AstNode member : declaration.getMembers()) {
            if (member instanceof FieldDeclaration) {
                processField((FieldDeclaration) member);
            }
            else if (member instanceof MethodDeclaration) {
                processMethod((MethodDeclaration) member);
            }
            else if (member instanceof ConstructorDeclaration) {
                processConstructor((ConstructorDeclaration) member);
            }
            else if (member instanceof TypeDeclaration) {
                processTypeDeclaration((TypeDeclaration) member);
            }
        }
    }

    private void applyTypeTarget(final TypeDeclaration declaration, final TypeAnnotation annotation) {
        if (!annotation.isEmittable()) {
            return;
        }

        final AstNodeCollection<TypeParameterDeclaration> typeParameters = declaration.getTypeParameters();
        final AstNodeCollection<AstType> interfaces = declaration.getInterfaces();
        final AstType baseType = declaration.getBaseType();

        final TypeAnnotationTargetInfo info = annotation.getTargetInfo();

        switch (annotation.getTargetKind()) {
            case CLASS_TYPE_PARAMETER: {
                final int index = ((TypeAnnotationTargetInfo.TypeParameterTarget) info).getParameterIndex();
                requireIndex(index, typeParameters.size(), annotation, "class type parameter");
                TypeAnnotationPlacer.placeOnTypeParameter(typeParameters.toArray(new TypeParameterDeclaration[0])[index], annotation, _builder);
                break;
            }

            case CLASS_TYPE_PARAMETER_BOUND: {
                final TypeAnnotationTargetInfo.TypeParameterBoundTarget bound =
                    (TypeAnnotationTargetInfo.TypeParameterBoundTarget) info;
                placeOnTypeParameterBound(typeParameters, bound.getParameterIndex(), bound.getBoundIndex(), annotation);
                break;
            }

            case CLASS_EXTENDS: {
                final TypeAnnotationTargetInfo.SupertypeTarget supertype =
                    (TypeAnnotationTargetInfo.SupertypeTarget) info;

                if (supertype.isSuperclass()) {
                    if (baseType == null || baseType.isNull()) {
                        throw placementFailure(annotation, "superclass target but no superclass type present");
                    }

                    TypeAnnotationPlacer.placeOnType(baseType, singleton(annotation), _builder);
                }
                else {
                    final int index = supertype.getSupertypeIndex();
                    requireIndex(index, interfaces.size(), annotation, "interface");
                    TypeAnnotationPlacer.placeOnType(
                        interfaces.toArray(new AstType[0])[index],
                        singleton(annotation),
                        _builder
                    );
                }
                break;
            }

            default:
                // Other target kinds do not belong to a type declaration attribute.
                break;
        }
    }

    private void placeOnTypeParameterBound(
        final AstNodeCollection<TypeParameterDeclaration> typeParameters,
        final int parameterIndex,
        final int boundIndex,
        final TypeAnnotation annotation) {

        requireIndex(parameterIndex, typeParameters.size(), annotation, "type parameter");

        final TypeParameterDeclaration parameter =
            typeParameters.toArray(new TypeParameterDeclaration[0])[parameterIndex];
        final AstType bound = parameter.getExtendsBound();

        if (bound == null || bound.isNull()) {
            throw placementFailure(annotation, "type parameter has no declared bound");
        }

        //
        // bound_index 0 is the (possibly implicit) class bound; an intersection of additional
        // interface bounds is represented as an IntersectionType whose members follow in order.
        //
        if (boundIndex == 0) {
            TypeAnnotationPlacer.placeOnType(bound, singleton(annotation), _builder);
            return;
        }

        final AstType boundType = selectIntersectionMember(bound, boundIndex, annotation);
        TypeAnnotationPlacer.placeOnType(boundType, singleton(annotation), _builder);
    }

    static AstType selectIntersectionMember(
        final AstType bound,
        final int boundIndex,
        final TypeAnnotation annotation) {

        if (!(bound instanceof com.strobel.decompiler.languages.java.ast.IntersectionType)) {
            throw placementFailure(
                annotation,
                "bound_index " + boundIndex + " requires an intersection bound type"
            );
        }

        final com.strobel.decompiler.languages.java.ast.IntersectionType intersection =
            (com.strobel.decompiler.languages.java.ast.IntersectionType) bound;

        //
        // JVMS counts the class bound as 0 and each interface bound after it.  In the AST the
        // class bound is IntersectionType.getBaseType() and interface bounds are its children.
        //
        if (boundIndex == 0) {
            return intersection.getBaseType();
        }

        final int interfaceIndex = boundIndex - 1;
        final AstNodeCollection<AstType> members = intersection.getInterfaces();

        if (interfaceIndex < 0 || interfaceIndex >= members.size()) {
            throw placementFailure(
                annotation,
                "bound_index " + boundIndex + " out of range (have " + members.size() +
                " additional interface bound(s))"
            );
        }

        return members.toArray(new AstType[0])[interfaceIndex];
    }

    //
    // Fields: only the FIELD (0x13) target is source-visible here.
    //
    private void processField(final FieldDeclaration declaration) {
        final FieldDefinition definition = declaration.getUserData(Keys.FIELD_DEFINITION);

        if (definition == null) {
            return;
        }

        final List<TypeAnnotation> all = TypeAnnotations.collect(definition.getSourceAttributes());

        if (all.isEmpty()) {
            return;
        }

        declaration.putUserData(Keys.DECLARATION_TYPE_ANNOTATIONS, new ArrayList<>(all));

        for (final TypeAnnotation annotation : filter(all, TypeAnnotationTargetKind.FIELD)) {
            safely(declaration, annotation, new Runnable() {
                @Override
                public void run() {
                    TypeAnnotationPlacer.placeOnType(
                        declaration.getReturnType(),
                        singleton(annotation),
                        _builder
                    );
                }
            });
        }
    }

    //
    // Methods and constructors: type parameters/bounds, return, parameters, throws; receiver
    // and Code targets are recorded only.
    //
    private void processMethod(final MethodDeclaration declaration) {
        processMethodLike(declaration, declaration.getUserData(Keys.METHOD_DEFINITION), false);
    }

    private void processConstructor(final ConstructorDeclaration declaration) {
        processMethodLike(declaration, declaration.getUserData(Keys.METHOD_DEFINITION), true);
    }

    private void processMethodLike(
            final com.strobel.decompiler.languages.java.ast.EntityDeclaration declaration,
            final MethodDefinition definition,
            final boolean isConstructor) {

        if (definition == null) {
            return;
        }

        final List<SourceAttribute> sourceAttributes = definition.getSourceAttributes();
        final List<TypeAnnotation> all = TypeAnnotations.collect(sourceAttributes);

        if (all.isEmpty()) {
            return;
        }

        declaration.putUserData(Keys.DECLARATION_TYPE_ANNOTATIONS, new ArrayList<>(all));

        final AstNodeCollection<TypeParameterDeclaration> typeParameters;
        final AstNodeCollection<ParameterDeclaration> parameters;
        final AstNodeCollection<AstType> thrownTypes;
        final AstType returnType;

        if (isConstructor) {
            final ConstructorDeclaration ctor = (ConstructorDeclaration) declaration;
            typeParameters = ctor.getTypeParameters();
            parameters = ctor.getParameters();
            thrownTypes = ctor.getThrownTypes();
            returnType = null;
        }
        else {
            final MethodDeclaration method = (MethodDeclaration) declaration;
            typeParameters = method.getTypeParameters();
            parameters = method.getParameters();
            thrownTypes = method.getThrownTypes();
            returnType = method.getReturnType();
        }

        final TypeAnnotationParameterMapper.Mapping parameterMapping =
            TypeAnnotationParameterMapper.create(definition);

        for (final TypeAnnotation annotation : all) {
            if (!annotation.isEmittable()) {
                // Receiver and Code-attribute targets stay queryable only.
                continue;
            }

            final TypeAnnotationTargetInfo info = annotation.getTargetInfo();

            safely(declaration, annotation, new Runnable() {
                @Override
                public void run() {
                    applyMethodTarget(
                        annotation,
                        info,
                        typeParameters,
                        parameters,
                        thrownTypes,
                        returnType,
                        parameterMapping,
                        definition
                    );
                }
            });
        }
    }

    private void applyMethodTarget(
        final TypeAnnotation annotation,
        final TypeAnnotationTargetInfo info,
        final AstNodeCollection<TypeParameterDeclaration> typeParameters,
        final AstNodeCollection<ParameterDeclaration> parameters,
        final AstNodeCollection<AstType> thrownTypes,
        final AstType returnType,
        final TypeAnnotationParameterMapper.Mapping parameterMapping,
        final MethodDefinition definition) {

        switch (annotation.getTargetKind()) {
            case METHOD_TYPE_PARAMETER: {
                final int index = ((TypeAnnotationTargetInfo.TypeParameterTarget) info).getParameterIndex();
                requireIndex(index, typeParameters.size(), annotation, "method type parameter");
                TypeAnnotationPlacer.placeOnTypeParameter(
                    typeParameters.toArray(new TypeParameterDeclaration[0])[index],
                    annotation,
                    _builder
                );
                break;
            }

            case METHOD_TYPE_PARAMETER_BOUND: {
                final TypeAnnotationTargetInfo.TypeParameterBoundTarget bound =
                    (TypeAnnotationTargetInfo.TypeParameterBoundTarget) info;
                placeOnMethodTypeParameterBound(typeParameters, bound.getParameterIndex(), bound.getBoundIndex(), annotation);
                break;
            }

            case METHOD_RETURN: {
                if (returnType == null || returnType.isNull()) {
                    throw placementFailure(annotation, "return target on a method without a return type");
                }
                TypeAnnotationPlacer.placeOnType(returnType, singleton(annotation), _builder);
                break;
            }

            case FORMAL_PARAMETER: {
                final int formalIndex =
                    ((TypeAnnotationTargetInfo.FormalParameterTarget) info).getFormalParameterIndex();
                final ParameterDefinition parameterDefinition =
                    parameterMapping.resolve(formalIndex, definition);
                final ParameterDeclaration parameter = findParameterDeclaration(parameters, parameterDefinition);

                if (parameter == null) {
                    throw new com.strobel.assembler.metadata.typeannotations.TypeAnnotationUnsupportedException(
                        "formal_parameter_index " + formalIndex + " maps to " +
                        parameterDefinition.getName() + " which has no AST parameter in " +
                        definition.getBriefDescription()
                    );
                }

                TypeAnnotationPlacer.placeOnType(parameter.getType(), singleton(annotation), _builder);
                break;
            }

            case THROWS: {
                final int index = ((TypeAnnotationTargetInfo.ThrowsTarget) info).getThrowsTypeIndex();
                requireIndex(index, thrownTypes.size(), annotation, "throws type");
                TypeAnnotationPlacer.placeOnType(
                    thrownTypes.toArray(new AstType[0])[index],
                    singleton(annotation),
                    _builder
                );
                break;
            }

            case METHOD_RECEIVER:
                // Recorded on the declaration metadata; not emitted into source.
                break;

            default:
                break;
        }
    }

    private void placeOnMethodTypeParameterBound(
        final AstNodeCollection<TypeParameterDeclaration> typeParameters,
        final int parameterIndex,
        final int boundIndex,
        final TypeAnnotation annotation) {

        requireIndex(parameterIndex, typeParameters.size(), annotation, "method type parameter");

        final TypeParameterDeclaration parameter =
            typeParameters.toArray(new TypeParameterDeclaration[0])[parameterIndex];
        final AstType bound = parameter.getExtendsBound();

        if (bound == null || bound.isNull()) {
            throw placementFailure(annotation, "method type parameter has no declared bound");
        }

        final AstType target = boundIndex == 0
            ? bound
            : selectIntersectionMember(bound, boundIndex, annotation);

        TypeAnnotationPlacer.placeOnType(target, singleton(annotation), _builder);
    }

    private static ParameterDeclaration findParameterDeclaration(
        final AstNodeCollection<ParameterDeclaration> parameters,
        final ParameterDefinition definition) {

        for (final ParameterDeclaration parameter : parameters) {
            if (parameter.getUserData(Keys.PARAMETER_DEFINITION) == definition) {
                return parameter;
            }
        }

        return null;
    }

    private static List<TypeAnnotation> filter(
        final List<TypeAnnotation> annotations,
        final TypeAnnotationTargetKind kind) {

        final List<TypeAnnotation> result = new ArrayList<>();

        for (final TypeAnnotation annotation : annotations) {
            if (annotation.getTargetKind() == kind) {
                result.add(annotation);
            }
        }

        return result;
    }

    private static List<TypeAnnotation> singleton(final TypeAnnotation annotation) {
        final List<TypeAnnotation> list = new ArrayList<>(1);
        list.add(annotation);
        return list;
    }

    private static void requireIndex(
        final int index,
        final int size,
        final TypeAnnotation annotation,
        final String what) {

        if (index < 0 || index >= size) {
            throw placementFailure(
                annotation,
                what + " index " + index + " out of range (have " + size + ")"
            );
        }
    }

    private static com.strobel.assembler.metadata.typeannotations.TypeAnnotationPlacementException placementFailure(
        final TypeAnnotation annotation,
        final String reason) {

        return new com.strobel.assembler.metadata.typeannotations.TypeAnnotationPlacementException(
            "Cannot place type annotation " +
            annotation.getAnnotation().getAnnotationType().getFullName() +
            " (" + annotation.getTargetInfo() + ", path " + annotation.getPath() + "): " + reason
        );
    }
}
