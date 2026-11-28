/*
 * Copyright (c) 1996, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
package org.openjdk.asmtools.jdis;

import java.util.EnumSet;
import java.util.function.Consumer;
import java.util.stream.Collectors;

import static java.lang.String.format;
import static org.openjdk.asmtools.jdis.Options.PrintOption.*;

/**
 * A set of jdis printing options.
 * <p>
 * Each {@link Main} instance owns one of these objects, so configuring or running a
 * disassembler never changes the behavior of another one. The historical public static
 * entry points are kept for compatibility; they operate on a separate process-wide set
 * that is not consulted by any {@code Main} instance.
 */
public class Options {

    // Initial options correspond to calling the tool without options.
    static private final EnumSet<PrintOption> DEFAULT_OPTIONS = EnumSet.of(
            // default option(s)
            LABELS);

    // Legacy, process-wide option set kept for source/binary compatibility only.
    static private final Options LEGACY = new Options();

    private final EnumSet<PrintOption> printOptions;

    /**
     * Creates an option set equivalent to invoking the tool without options.
     */
    public Options() {
        this.printOptions = EnumSet.copyOf(DEFAULT_OPTIONS);
    }

    /**
     * Creates a copy of another option set.
     */
    public Options(Options original) {
        this.printOptions = original.printOptions.clone();
    }

    /**
     * @return a copy of the legacy global option set.
     */
    public static Options legacySnapshot() {
        return new Options(LEGACY);
    }

    /**
     * @return the printable description of this option set.
     */
    public String describe() {
        return printOptions.stream().map(op -> format("%-26s: \"%s\"", op.name(), op.descriptor)).
                collect(Collectors.joining("\n"));
    }

    // Print Options
    public enum PrintOption {
        NONE("No options"),
        CONSTANT_POOL("Constant Pool"),
        PRINT_BCI("Program Counter - for all instructions"),
        LABELS("Labels (as identifiers)"),
        CP_INDEX("CP index along with arguments"),
        LINE_NUMBER_TABLE_Numbers("Line Numbers in comments"),
        LINE_NUMBER_TABLE_Lines("Java Source Lines in comments"),
        LINE_NUMBER_TABLE_Table("LineNumberTable attribute as table"),
        LINE_NUMBER_TABLE_All("Line Numbers, Source Lines, attribute as Table", (option) -> setGroupOption(option)),
        LOCAL_VARIABLE_Vars("Print LocalVariableTable attribute"),
        LOCAL_VARIABLE_Types("Print LocalVariableTypeTable attribute"),
        LOCAL_VARIABLE_All("Print both LocalVariableTable and LocalVariableTypeTable attributes", (option) -> setGroupOption(option)),
        HEX("Numbers as hexadecimals"),
        TRACE("Print internal traces, debug information"),
        SYSINFO("system information"),
        NO_COMMENTS("No comments, suppress printing comments"),
        VERBOSE("Verbose information"),
        TABLE("Attributes as table"),
        DROP_Source("Discard SourceFile attribute"),
        DROP_Signatures("Discard Signature attribute"),
        DROP_Classes("Discard this_class and super_class pair"),
        DROP_CharacterRange("Discard CharacterRangeTable attribute"),
        DROP_All("Discard SourceFile, CharacterRangeTable attributes, this_class and super_class pair", (option) -> setGroupOption(option)),
        DETAILED_Output("Detailed output"),
        EXTRA_DETAILED_Output("Detailed output, this_class and super_class pair"),
        BEST_EFFORT("Print as much as possible despite errors");

        final String descriptor;
        final Consumer<PrintOption> action;

        PrintOption(String descriptor) {
            this.descriptor = descriptor;
            this.action = null;
        }

        PrintOption(String descriptor, Consumer<PrintOption> action) {
            this.descriptor = descriptor;
            this.action = action;
        }

        /**
         * Applies the option to the legacy global option set.
         *
         * @deprecated use {@link #apply(Options)} with an option set owned by the tool.
         */
        @Deprecated
        public void apply() {
            apply(LEGACY);
        }

        /**
         * Applies the option (expanding a group option) to the supplied option set.
         */
        public void apply(Options options) {
            if (isGroupOption()) {
                options.enableGroup(this);
            } else {
                options.enable(this);
            }
        }

        private boolean isGroupOption() {
            return this == LOCAL_VARIABLE_All || this == LINE_NUMBER_TABLE_All || this == DROP_All;
        }

        /**
         * @return PR if PR value has format {Prefix}{Any Chars}_{flag} and flag == postfix otherwise null
         */
        public static PrintOption getStringFlag(String prefix, String flag) {
            for (PrintOption item : PrintOption.values()) {
                String name = item.name();
                if (name.startsWith(prefix)) {
                    int ind = item.name().lastIndexOf('_');
                    if (ind > 0 && name.substring(ind + 1).equalsIgnoreCase(flag)) {
                        return item;
                    }
                }
            }
            return null;
        }

        private static final EnumSet<PrintOption> inProgresses = EnumSet.of(
                DROP_CharacterRange);

        public boolean isActive() {
            return !PrintOption.inProgresses.contains(this);
        }
    }

    static private final EnumSet<PrintOption> DETAILED_OUTPUT = EnumSet.of(      // -g:              detailed output format
            CONSTANT_POOL,
            PRINT_BCI,
            CP_INDEX,
            // Print LocalVariables, LocalVariableTypes only if it's specified.
            // LOCAL_VARIABLE_Vars, LOCAL_VARIABLE_Types,
            DETAILED_Output
    );

    static private final EnumSet<PrintOption> DROP_ALL = EnumSet.of(             // -drop || -drop:all
            DROP_Source,
            DROP_Classes,
            DROP_CharacterRange
    );

    static private final EnumSet<PrintOption> LINE_NUMBER_TABLE_ALL = EnumSet.of(
            LINE_NUMBER_TABLE_Numbers,
            LINE_NUMBER_TABLE_Lines,
            LINE_NUMBER_TABLE_Table
    );

    static private final EnumSet<PrintOption> LOCAL_VARIABLE_ALL = EnumSet.of(
            LOCAL_VARIABLE_Vars,
            LOCAL_VARIABLE_Types
    );

    // ---------------------------------------------------------------------
    // Instance API. Every configured Main instance owns its own set.
    // ---------------------------------------------------------------------

    public void enable(PrintOption val) {
        printOptions.add(val);
    }

    public void disable(PrintOption val) {
        printOptions.remove(val);
    }

    public void enable(EnumSet<PrintOption> vals) {
        for (PrintOption val : vals) {
            enable(val);
        }
    }

    public void disable(EnumSet<PrintOption> vals) {
        for (PrintOption val : vals) {
            disable(val);
        }
    }

    public void enableDetailedOutput() {
        enable(DETAILED_OUTPUT);
        disable(PrintOption.LABELS);
    }

    public void enableGroup(PrintOption option) {
        switch (option) {
            case LOCAL_VARIABLE_All -> enable(LOCAL_VARIABLE_ALL);
            case LINE_NUMBER_TABLE_All -> enable(LINE_NUMBER_TABLE_ALL);
            case DROP_All -> enable(DROP_ALL);
            default -> throw new RuntimeException("%s ia not group option".formatted(option.name()));
        }
    }

    public void resetToDefaults() {
        printOptions.clear();
        enable(PrintOption.LABELS);
    }

    public boolean isEnabled(PrintOption... vals) {
        for (PrintOption val : vals) {
            if (printOptions.contains(val)) {
                return true;
            }
        }
        return false;
    }

    public String asShortString() {
        return format("[ %s ]",
                printOptions.stream().map(item -> item.name()).collect(Collectors.joining(", ")));
    }

    // ---------------------------------------------------------------------
    // Legacy static entry points. They operate on the process-wide legacy set
    // that is not used by Main instances configured with their own options.
    // ---------------------------------------------------------------------

    public static String getPrintOptions() {
        return LEGACY.describe();
    }

    public static void set(PrintOption val) {
        LEGACY.enable(val);
    }

    public static void unset(PrintOption val) {
        LEGACY.disable(val);
    }

    public static void setDetailedOutputOptions() {
        LEGACY.enableDetailedOutput();
    }

    public static void set(EnumSet<PrintOption> vals) {
        LEGACY.enable(vals);
    }

    public static void unset(EnumSet<PrintOption> vals) {
        LEGACY.disable(vals);
    }

    public static void setGroupOption(PrintOption option) {
        LEGACY.enableGroup(option);
    }

    public static void setDefaultOutputOptions() {
        LEGACY.resetToDefaults();
    }

    public static boolean contains(PrintOption... vals) {
        return LEGACY.isEnabled(vals);
    }
}
