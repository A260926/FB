/*
 * Keys.java
 *
 * Copyright (c) 2013 Mike Strobel
 *
 * This source code is based on Mono.Cecil from Jb Evain, Copyright (c) Jb Evain;
 * and ILSpy/ICSharpCode from SharpDevelop, Copyright (c) AlphaSierraPapa.
 *
 * This source code is subject to terms and conditions of the Apache License, Version 2.0.
 * A copy of the license can be found in the License.html file at the root of this distribution.
 * By using this source code in any fashion, you are agreeing to be bound by the terms of the
 * Apache License, Version 2.0.
 *
 * You must not remove this notice, or any other, from this software.
 */

package com.strobel.decompiler.languages.java.ast;

import com.strobel.assembler.ir.attributes.ModuleAttribute;
import com.strobel.assembler.metadata.*;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotation;
import com.strobel.componentmodel.Key;
import com.strobel.core.ArrayUtilities;
import com.strobel.core.ExceptionUtilities;
import com.strobel.decompiler.ast.Variable;

import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;

public final class Keys {
    public final static Key<Variable> VARIABLE = Key.create("Variable");
    public final static Key<VariableDefinition> VARIABLE_DEFINITION = Key.create("VariableDefinition");
    public final static Key<ParameterDefinition> PARAMETER_DEFINITION = Key.create("ParameterDefinition");
    public final static Key<MemberReference> MEMBER_REFERENCE = Key.create("MemberReference");
    public final static Key<PackageReference> PACKAGE_REFERENCE = Key.create("PackageReference");
    public final static Key<FieldDefinition> FIELD_DEFINITION = Key.create("FieldDefinition");
    public final static Key<MethodDefinition> METHOD_DEFINITION = Key.create("MethodDefinition");
    public final static Key<TypeDefinition> TYPE_DEFINITION = Key.create("TypeDefinition");
    public final static Key<ModuleReference> MODULE_REFERENCE = Key.create("ModuleReference");
    public final static Key<TypeReference> TYPE_REFERENCE = Key.create("TypeReference");
    public final static Key<TypeReference> ANONYMOUS_BASE_TYPE_REFERENCE = Key.create("AnonymousBaseTypeReference");
    public final static Key<DynamicCallSite> DYNAMIC_CALL_SITE = Key.create("DynamicCallSite");
    public final static Key<AstBuilder> AST_BUILDER = Key.create("AstBuilder");
    public final static Key<Object> CONSTANT_VALUE = Key.create("ConstantValue");
    public final static Key<NameVariables> NAME_VARIABLES = Key.create("NameVariables");

    /**
     * For a type-use {@link Annotation} attached to a dotted member {@link SimpleType}, the
     * nested-class segment it applies to ({@code 0} = outermost/root segment).  Absent for
     * annotations on a non-dotted type.
     */
    public final static Key<Integer> TYPE_ANNOTATION_SEGMENT =
        Key.create("TypeAnnotationSegment");

    /**
     * The exact raw JVMS 4.7.20 record that a renderable type-use {@link Annotation} was
     * generated from.  The 1:1 link (plus {@link #TYPE_ANNOTATION_SEGMENT}) keeps each type
     * usage distinguishable after import cleanup, nested-name simplification and AST cloning.
     */
    public final static Key<TypeAnnotation> SOURCE_TYPE_ANNOTATION =
        Key.create("SourceTypeAnnotation");

    /**
     * Every raw type-use record (including receiver and {@code Code} targets that are not
     * written into source) attached to a declaration node.  Provides a queryable, ordered
     * record of the full JVMS 4.7.20 attribute contents for that declaration.
     */
    public final static Key<java.util.List<TypeAnnotation>> DECLARATION_TYPE_ANNOTATIONS =
        Key.create("DeclarationTypeAnnotations");

    /**
     * Type-use records that were read and validated but whose {@code type_path} could not be
     * represented on the reconstructed type tree (for example an outer member-segment type
     * argument that the AST does not retain).  They are never attached to a wrong node; they
     * are retained here so callers can report them as unanalyzed.
     */
    public final static Key<java.util.List<TypeAnnotation>> UNSUPPORTED_TYPE_ANNOTATIONS =
        Key.create("UnsupportedTypeAnnotations");
    public final static List<Key<?>> ALL_KEYS;

    static {
        final ArrayList<Key<?>> keys = new ArrayList<>();

        try {
            for (final Field field : Keys.class.getDeclaredFields()) {
                if (field.getType() == Key.class) {
                    keys.add((Key<?>) field.get(null));
                }
            }

            ALL_KEYS = ArrayUtilities.<Key<?>>asUnmodifiableList(keys.toArray(new Key[keys.size()]));
        }
        catch (final Throwable t) {
            throw ExceptionUtilities.asRuntimeException(t);
        }
    }
}
