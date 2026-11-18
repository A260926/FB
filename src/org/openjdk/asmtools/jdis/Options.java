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
import java.util.stream.Collectors;

import static java.lang.String.format;
import static org.openjdk.asmtools.jdis.Indenter.*;
import static org.openjdk.asmtools.jdis.Options.PrintOption.*;

/**
 * A set of jdis print options.
 * <p>
 * Every configured disassembler owns exactly one {@code Options} instance.  The legacy
 * static entry points ({@link #set(PrintOption)}, {@link #contains(PrintOption...)}, ...)
 * are kept for source compatibility and operate on a process-wide
 * {@link #getGlobalOptions() global} instance.  A disassembler never reads from that
 * global instance: it reads only from the instance it captured when it was constructed
 * (or when its {@code disasm(String...)} / {@code decode(String...)} overload was called).
 * This keeps instances fully isolated from each other, including when they run in
 * parallel threads.
 */
public class Options {

    // Initial options correspond to calling the tool without options.
    private static final EnumSet<PrintOption> DEFAULT_PRINT_OPTIONS = EnumSet.of(
            // default option(s)
            LABELS);

    private static final int DEFAULT_TABLE_PADDING = OPERAND_PLACEHOLDER_LENGTH + INSTR_PREFIX_LENGTH + 1;
    private static final int PRINT_BCI_TABLE_PADDING = OPERAND_PLACEHOLDER_LENGTH + INSTR_PREFIX_LENGTH + 3;

    // Legacy process-wide options.  It is the default source for data structures that are
    // built outside of a configured disassembler.
    private static final Options GLOBAL = new Options();

    private final EnumSet<PrintOption> printOptions;
    private int tablePadding;

    /**
     * Creates a new option set initialized exactly like a tool invoked without options.
     */
    public Options() {
        this.printOptions = EnumSet.copyOf(DEFAULT_PRINT_OPTIONS);
        this.tablePadding = DEFAULT_TABLE_PADDING;
    }

    /**
     * Creates an independent copy of another option set.
     */
    public Options(Options other) {
        // The legacy set may be changed concurrently through the static API.
        synchronized (other) {
            this.printOptions = EnumSet.copyOf(other.printOptions);
            this.tablePadding = other.tablePadding;
        }
    }

    /**
     * @return the legacy process-wide option set.
     */
    public static Options getGlobalOptions() {
        return GLOBAL;
    }

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
        LINE_NUMBER_TABLE_All("Line Numbers, Source Lines, attribute as Table"),
        LOCAL_VARIABLE_Vars("Print LocalVariableTable attribute"),
        LOCAL_VARIABLE_Types("Print LocalVariableTypeTable attribute"),
        LOCAL_VARIABLE_All("Print both LocalVariableTable and LocalVariableTypeTable attributes"),
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
        DROP_All("Discard SourceFile, CharacterRangeTable attributes, this_class and super_class pair"),
        DETAILED_Output("Detailed output"),
        EXTRA_DETAILED_Output("Detailed output, this_class and super_class pair"),
        BEST_EFFORT("Print as much as possible despite errors");

        final String descriptor;

        PrintOption(String descriptor) {
            this.descriptor = descriptor;
        }

        /**
         * Applies this option to the legacy process-wide option set.
         */
        public void apply() {
            apply(GLOBAL);
        }

        /**
         * Applies this option (and, for group options, all the group members) to the
         * given option set.
         */
        public void apply(Options options) {
            switch (this) {
                case LOCAL_VARIABLE_All, LINE_NUMBER_TABLE_All, DROP_All -> options.enableGroupOption(this);
                default -> options.enable(this);
            }
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
    // Per-instance API.
    // ---------------------------------------------------------------------

    public void enable(PrintOption val) {
        if (val == PRINT_BCI) {
            tablePadding = PRINT_BCI_TABLE_PADDING;
        }
        printOptions.add(val);
    }

    public void disable(PrintOption val) {
        if (val == PRINT_BCI) {
            tablePadding = DEFAULT_TABLE_PADDING;
        }
        printOptions.remove(val);
    }

    public void enableDetailedOutputOptions() {
        enableAll(DETAILED_OUTPUT);
        disable(PrintOption.LABELS);
    }

    public void enableAll(EnumSet<PrintOption> vals) {
        for (PrintOption val : vals) {
            enable(val);
        }
    }

    public void disableAll(EnumSet<PrintOption> vals) {
        for (PrintOption val : vals) {
            disable(val);
        }
    }

    public void enableGroupOption(PrintOption option) {
        switch (option) {
            case LOCAL_VARIABLE_All -> enableAll(LOCAL_VARIABLE_ALL);
            case LINE_NUMBER_TABLE_All -> enableAll(LINE_NUMBER_TABLE_ALL);
            case DROP_All -> enableAll(DROP_ALL);
            default -> throw new RuntimeException("%s ia not group option".formatted(option.name()));
        }
    }

    public void resetToDefaults() {
        printOptions.clear();
        tablePadding = DEFAULT_TABLE_PADDING;
        enable(PrintOption.LABELS);
    }

    public boolean isSet(PrintOption... vals) {
        for (PrintOption val : vals) {
            if (printOptions.contains(val)) {
                return true;
            }
        }
        return false;
    }

    public String shortDescription() {
        return format("[ %s ]",
                printOptions.stream().map(PrintOption::name).collect(Collectors.joining(", ")));
    }

    /**
     * @return the padding used to align instruction-like records.  It depends on whether
     * the program counter is printed and is therefore a per-instance value.
     */
    public int getTablePadding() {
        return tablePadding;
    }

    /**
     * @return the base comment padding used by the constant pool.  It depends on the table
     * output mode and is therefore a per-instance value.
     */
    public int getCommentPadding() {
        return isSet(TABLE) ? 16 : 20;
    }

    // ---------------------------------------------------------------------
    // Legacy static API.  Operates on the process-wide GLOBAL instance only.
    // ---------------------------------------------------------------------

    public static String getPrintOptions() {
        synchronized (GLOBAL) {
            return GLOBAL.describe();
        }
    }

    public static void set(PrintOption val) {
        synchronized (GLOBAL) {
            GLOBAL.enable(val);
        }
    }

    public static void unset(PrintOption val) {
        synchronized (GLOBAL) {
            GLOBAL.disable(val);
        }
    }

    public static void setDetailedOutputOptions() {
        synchronized (GLOBAL) {
            GLOBAL.enableDetailedOutputOptions();
        }
    }

    public static void set(EnumSet<PrintOption> vals) {
        synchronized (GLOBAL) {
            GLOBAL.enableAll(vals);
        }
    }

    public static void unset(EnumSet<PrintOption> vals) {
        synchronized (GLOBAL) {
            GLOBAL.disableAll(vals);
        }
    }

    public static void setGroupOption(PrintOption option) {
        synchronized (GLOBAL) {
            GLOBAL.enableGroupOption(option);
        }
    }

    public static void setDefaultOutputOptions() {
        synchronized (GLOBAL) {
            GLOBAL.resetToDefaults();
        }
    }

    public static boolean contains(PrintOption... vals) {
        synchronized (GLOBAL) {
            return GLOBAL.isSet(vals);
        }
    }

    public static String asShortString() {
        synchronized (GLOBAL) {
            return GLOBAL.shortDescription();
        }
    }
}
