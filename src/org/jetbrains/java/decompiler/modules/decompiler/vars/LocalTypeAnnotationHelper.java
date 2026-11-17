// Copyright 2000-2024 JetBrains s.r.o. and contributors. Use of this source code is governed by the Apache 2.0 license.
package org.jetbrains.java.decompiler.modules.decompiler.vars;

import org.jetbrains.java.decompiler.modules.decompiler.exps.Exprent;
import org.jetbrains.java.decompiler.modules.decompiler.exps.VarExprent;
import org.jetbrains.java.decompiler.modules.decompiler.sforms.DirectGraph;
import org.jetbrains.java.decompiler.modules.decompiler.stats.RootStatement;
import org.jetbrains.java.decompiler.modules.decompiler.typeann.TypeAnnotation;
import org.jetbrains.java.decompiler.util.StatementIterator;

import java.util.BitSet;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

/**
 * Binds LOCAL_VARIABLE type annotations read from the {@code Code} attribute to the local
 * variable declarations surviving the decompilation pipeline.
 *
 * <p>Annotations are addressed by bytecode slot and half-open live ranges, so the final
 * attachment works even when the LocalVariableTable is missing or when variable merging,
 * splitting and declaration lifting renumbered the SSA versions after the last
 * {@link VarProcessor#setVarVersions} call: each remaining declaration is matched through the
 * bytecode footprint of its own surviving version.</p>
 */
public final class LocalTypeAnnotationHelper {

  private LocalTypeAnnotationHelper() { }

  /**
   * Attaches the Code-level LOCAL_VARIABLE type annotations to the definition expressions of the
   * variables present in the tree. Must run after {@link VarProcessor#setVarDefinitions} and before
   * the method is written to source.
   */
  public static void attachDeclarations(RootStatement root, VarProcessor varProc) {
    if (varProc == null || !varProc.hasLocalTypeAnnotations()) {
      return;
    }

    // Bytecode footprint of every surviving version, including lifted declarations whose
    // defining VarExprent itself carries no offsets.
    Map<VarVersion, BitSet> footprints = new HashMap<>();
    StatementIterator.iterate(root, new DirectGraph.ExprentIterator() {
      @Override
      public int processExprent(Exprent exprent) {
        if (exprent.type == Exprent.EXPRENT_VAR && exprent.bytecode != null) {
          VarExprent var = (VarExprent)exprent;
          footprints.computeIfAbsent(var.getVarVersion(), key -> new BitSet()).or(var.bytecode);
        }
        return 0;
      }
    });

    StatementIterator.iterate(root, new DirectGraph.ExprentIterator() {
      @Override
      public int processExprent(Exprent exprent) {
        if (exprent.type != Exprent.EXPRENT_VAR) {
          return 0;
        }
        VarExprent var = (VarExprent)exprent;
        if (!var.isDefinition()) {
          return 0;
        }
        Integer originalSlot = varProc.getVarOriginalIndex(var.getIndex());
        if (originalSlot == null) {
          return 0;
        }
        BitSet footprint = footprints.get(var.getVarVersion());
        Set<TypeAnnotation> annotations = varProc.annotationsCovering(originalSlot, footprint);
        if (annotations.isEmpty() && footprint != null && !footprint.isEmpty()) {
          // javac starts the local variable range on the instruction after the store.
          BitSet widened = (BitSet)footprint.clone();
          widened.set(footprint.length());
          annotations = varProc.annotationsCovering(originalSlot, widened);
        }
        for (TypeAnnotation annotation : annotations) {
          var.addLocalTypeAnnotation(annotation);
        }
        return 0;
      }
    });
  }
}
