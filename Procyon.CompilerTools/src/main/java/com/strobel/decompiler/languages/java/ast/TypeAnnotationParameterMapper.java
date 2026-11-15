/*
 * TypeAnnotationParameterMapper.java
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

import com.strobel.assembler.ir.attributes.AttributeNames;
import com.strobel.assembler.ir.attributes.MethodParametersAttribute;
import com.strobel.assembler.ir.attributes.SourceAttribute;
import com.strobel.assembler.metadata.Flags;
import com.strobel.assembler.metadata.MethodDefinition;
import com.strobel.assembler.metadata.ParameterDefinition;
import com.strobel.assembler.metadata.TypeDefinition;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotationUnsupportedException;

import java.util.ArrayList;
import java.util.List;

/**
 * Maps a JVMS {@code formal_parameter_index} (target 0x16) to the source-visible
 * {@link ParameterDefinition} the decompiler prints.
 *
 * <p>The index is <strong>not</strong> a descriptor slot: JVMS 4.7.20 counts only the
 * parameters that survive into source, while the descriptor includes hidden compiler-introduced
 * parameters (a non-static inner class constructor's enclosing instance, an enum constructor's
 * {@code $enum$name}/{@code $enum$ordinal}, and other synthetic/mandated parameters).  The
 * mapping is reconstructed from the {@code MethodParameters} flags, the method signature and
 * the declaring class's inner-class/enum nature.  When the source parameter cannot be identified
 * uniquely, a {@link TypeAnnotationUnsupportedException} is raised instead of guessing.</p>
 */
public final class TypeAnnotationParameterMapper {

    private TypeAnnotationParameterMapper() {
    }

    public static final class Mapping {
        final List<ParameterDefinition> visibleParameters;

        public Mapping(final List<ParameterDefinition> visibleParameters) {
            this.visibleParameters = visibleParameters;
        }

        public ParameterDefinition resolve(final int formalParameterIndex, final MethodDefinition method) {
            if (formalParameterIndex < 0 || formalParameterIndex >= visibleParameters.size()) {
                throw new TypeAnnotationUnsupportedException(
                    "formal_parameter_index " + formalParameterIndex +
                    " cannot be mapped to a source parameter of " +
                    method.getBriefDescription() + ": only " +
                    visibleParameters.size() + " source-visible parameter(s) exist"
                );
            }

            return visibleParameters.get(formalParameterIndex);
        }
    }

    public static Mapping create(final MethodDefinition method) {
        final List<ParameterDefinition> allParameters = method.getParameters();

        if (allParameters.isEmpty()) {
            return new Mapping(new ArrayList<ParameterDefinition>());
        }

        final boolean[] hidden = new boolean[allParameters.size()];
        final boolean haveMethodParameters =
            SourceAttribute.find(AttributeNames.MethodParameters, method.getSourceAttributes()) != null;

        if (haveMethodParameters) {
            //
            // Procyon already applies the MethodParameters flags to each ParameterDefinition,
            // so synthetic/mandated parameters are directly recognizable.
            //
            for (int i = 0; i < allParameters.size(); i++) {
                final ParameterDefinition parameter = allParameters.get(i);
                hidden[i] = parameter.isSynthetic() || parameter.isMandated();
            }
        }
        else {
            markStructurallyHidden(method, hidden);
        }

        final List<ParameterDefinition> visible = new ArrayList<>();

        for (int i = 0; i < allParameters.size(); i++) {
            if (!hidden[i]) {
                visible.add(allParameters.get(i));
            }
        }

        //
        // A descriptor that still contains more entries than the source signature indicates
        // hidden parameters we could not account for; the formal index would then be ambiguous.
        //
        if (visible.isEmpty() && !allParameters.isEmpty()) {
            throw new TypeAnnotationUnsupportedException(
                "No source-visible parameters could be identified for " +
                method.getBriefDescription()
            );
        }

        return new Mapping(visible);
    }

    /**
     * Fallback for classes without a {@code MethodParameters} attribute: recognize the standard
     * compiler-generated hidden parameters from the declaration shape.
     */
    private static void markStructurallyHidden(final MethodDefinition method, final boolean[] hidden) {
        final TypeDefinition declaringType = method.getDeclaringType();

        if (!method.isConstructor() || declaringType == null) {
            return;
        }

        final List<ParameterDefinition> parameters = method.getParameters();

        if (declaringType.isEnum()) {
            // Enum constructors always lead with synthetic name and ordinal parameters.
            if (parameters.size() >= 2) {
                hidden[0] = true;
                hidden[1] = true;
            }
            return;
        }

        if (!Flags.testAny(declaringType.getFlags(), Flags.STATIC) &&
            declaringType.isNested() &&
            !parameters.isEmpty()) {

            //
            // A non-static nested class constructor leads with the enclosing instance, whose
            // type is the declaring type's enclosing class.
            //
            final ParameterDefinition first = parameters.get(0);

            if (declaringType.getDeclaringType() != null &&
                first.getParameterType() != null &&
                declaringType.getDeclaringType().isEquivalentTo(first.getParameterType())) {

                hidden[0] = true;
            }
        }
    }
}
