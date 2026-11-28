import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import org.openjdk.asmtools.common.inputs.ByteInput;
import org.openjdk.asmtools.common.outputs.TextOutput;
import org.openjdk.asmtools.common.outputs.log.StringLog;
import org.openjdk.asmtools.jdis.Main;

/**
 * Production-driven probe for the instance option ownership contract.
 * <p>
 * Each scenario writes its real disassembly text to stdout as one escaped
 * {@code KEY=value} line per result. The C++ contract test compares those
 * bodies against independently produced reference bodies.
 */
public final class InstanceOptionsProbe {
    private static byte[] bytes;

    private static Main make(TextOutput out, String... args) {
        return new Main(out, new StringLog(), new ByteInput(bytes), args);
    }

    /** Runs a configured instance and returns the freshly appended output body. */
    private static String run(Main m, TextOutput out) {
        int before = out.getOutputs().size();
        int rc = m.disasm();
        if (rc != 0) {
            throw new AssertionError("disassembly failed rc=" + rc);
        }
        if (out.getOutputs().size() != before + 1) {
            throw new AssertionError("expected exactly one new class output");
        }
        return out.getOutputs().get(before).getBody();
    }

    private static String esc(String s) {
        StringBuilder sb = new StringBuilder(s.length() + 16);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '\n' -> sb.append("\\n");
                case '\r' -> sb.append("\\r");
                case '\t' -> sb.append("\\t");
                case '\\' -> sb.append("\\\\");
                default -> sb.append(c);
            }
        }
        return sb.toString();
    }

    private static void emit(String key, String body) {
        System.out.println(key + "=" + esc(body));
    }

    public static void main(String[] argv) throws Exception {
        bytes = Files.readAllBytes(Path.of(argv[1]));
        String scenario = argv[0];
        switch (scenario) {
            case "once" -> {
                // once <class> <option>...
                String[] opts = new String[argv.length - 2];
                System.arraycopy(argv, 2, opts, 0, opts.length);
                TextOutput out = new TextOutput();
                emit("OUT", run(make(out, opts), out));
            }
            case "ownership" -> ownership();
            case "reverse" -> reverse();
            case "reargue" -> reargue();
            case "interleave" -> interleave();
            case "parallel" -> parallel();
            case "repeat" -> repeat();
            default -> throw new IllegalArgumentException("unknown scenario " + scenario);
        }
    }

    private static final String[] DETAILED = {"-g", "-table", "-lvt", "-lnt:numbers"};

    // Default instance created first, detailed instance second, then the default one runs.
    private static void ownership() {
        TextOutput aOut = new TextOutput();
        TextOutput bOut = new TextOutput();
        Main a = make(aOut);
        Main b = make(bOut, DETAILED);
        emit("A", run(a, aOut));
        emit("B", run(b, bOut));
    }

    // Detailed instance created first, default instance second.
    private static void reverse() {
        TextOutput dOut = new TextOutput();
        TextOutput eOut = new TextOutput();
        Main d = make(dOut, DETAILED);
        Main e = make(eOut);
        emit("A", run(d, dOut));
        emit("B", run(e, eOut));
    }

    // One instance is reconfigured between runs; another instance is configured in between.
    private static void reargue() {
        TextOutput out = new TextOutput();
        Main m = make(out, "-g");
        emit("G", run(m, out));

        // A different instance is created and configured while m keeps its options.
        TextOutput otherOut = new TextOutput();
        Main other = make(otherOut, "-lvt", "-lnt:numbers");
        emit("OTHER", run(other, otherOut));
        emit("G_AFTER_OTHER", run(m, out));

        // Re-argue the very same instance, then run it again.
        TextOutput tableOut = new TextOutput();
        Main m2 = make(tableOut, "-g");
        m2.disasm("-table");
        emit("TABLE", run(m2, tableOut));

        TextOutput detailedOut = new TextOutput();
        Main m3 = make(detailedOut, "-g");
        m3.disasm("-g", "-table", "-lvt", "-lnt:numbers");
        emit("DETAILED", run(m3, detailedOut));
    }

    // Two instances alternated twice, each on its own output.
    private static void interleave() {
        TextOutput aOut = new TextOutput();
        TextOutput bOut = new TextOutput();
        Main a = make(aOut);
        Main b = make(bOut, DETAILED);
        StringBuilder aBodies = new StringBuilder();
        StringBuilder bBodies = new StringBuilder();
        for (int i = 0; i < 2; i++) {
            aBodies.append(run(a, aOut));
            bBodies.append(run(b, bOut));
        }
        emit("A_ONCE", runOnce());
        emit("B_ONCE", runOnce(DETAILED));
        emit("A_TWICE", aBodies.toString());
        emit("B_TWICE", bBodies.toString());
    }

    // Two threads, each with its own instance, configuration, and output.
    private static void parallel() throws Exception {
        String[] results = new String[2];
        Throwable[] failures = new Throwable[2];
        Thread a = new Thread(() -> {
            try {
                TextOutput out = new TextOutput();
                results[0] = run(make(out), out);
            } catch (Throwable t) {
                failures[0] = t;
            }
        });
        Thread b = new Thread(() -> {
            try {
                TextOutput out = new TextOutput();
                results[1] = run(make(out, DETAILED), out);
            } catch (Throwable t) {
                failures[1] = t;
            }
        });
        a.start();
        b.start();
        a.join();
        b.join();
        if (failures[0] != null) {
            throw new AssertionError("default thread failed", failures[0]);
        }
        if (failures[1] != null) {
            throw new AssertionError("detailed thread failed", failures[1]);
        }
        emit("A", results[0]);
        emit("B", results[1]);
    }

    // The same configured instance runs twice; the second run must be identical.
    private static void repeat() {
        TextOutput out = new TextOutput();
        Main m = make(out, DETAILED);
        String first = run(m, out);
        String second = run(m, out);
        emit("FIRST", first);
        emit("SECOND", second);
        // Count debug information events that must not be duplicated.
        System.out.println("FIRST_LOCAL_STARTS=" + debugMetric(first, DebugMetric.LOCAL_START));
        System.out.println("SECOND_LOCAL_STARTS=" + debugMetric(second, DebugMetric.LOCAL_START));
        System.out.println("FIRST_ENDVARS=" + debugMetric(first, DebugMetric.END_VAR));
        System.out.println("SECOND_ENDVARS=" + debugMetric(second, DebugMetric.END_VAR));
        System.out.println("FIRST_TRAPS=" + debugMetric(first, DebugMetric.TRAP));
        System.out.println("SECOND_TRAPS=" + debugMetric(second, DebugMetric.TRAP));
        System.out.println("FIRST_LINES=" + debugMetric(first, DebugMetric.LINE));
        System.out.println("SECOND_LINES=" + debugMetric(second, DebugMetric.LINE));

        // The non-table debug form prints explicit var/endvar/type/endtype events.
        String[] plain = {"-g", "-lvt", "-lnt:numbers"};
        TextOutput plainOut = new TextOutput();
        Main pm = make(plainOut, plain);
        String plainFirst = run(pm, plainOut);
        String plainSecond = run(pm, plainOut);
        emit("PLAIN_FIRST", plainFirst);
        emit("PLAIN_SECOND", plainSecond);
        System.out.println("PLAIN_FIRST_VARS=" + debugMetric(plainFirst, DebugMetric.VAR_EVENT));
        System.out.println("PLAIN_SECOND_VARS=" + debugMetric(plainSecond, DebugMetric.VAR_EVENT));
        System.out.println("PLAIN_FIRST_ENDVARS=" + debugMetric(plainFirst, DebugMetric.END_VAR));
        System.out.println("PLAIN_SECOND_ENDVARS=" + debugMetric(plainSecond, DebugMetric.END_VAR));
        System.out.println("PLAIN_FIRST_TYPES=" + debugMetric(plainFirst, DebugMetric.TYPE_EVENT));
        System.out.println("PLAIN_SECOND_TYPES=" + debugMetric(plainSecond, DebugMetric.TYPE_EVENT));
        System.out.println("PLAIN_FIRST_STACK=" + debugMetric(plainFirst, DebugMetric.STACK_EVENT));
        System.out.println("PLAIN_SECOND_STACK=" + debugMetric(plainSecond, DebugMetric.STACK_EVENT));
    }

    private enum DebugMetric { LOCAL_START, END_VAR, TRAP, LINE, VAR_EVENT, TYPE_EVENT, STACK_EVENT }

    private static int debugMetric(String body, DebugMetric metric) {
        String[] lines = body.split("\\R");
        switch (metric) {
            case LOCAL_START -> {
                // Data rows following the LocalVariables/LocalVariableTypes headers.
                int n = 0;
                boolean inTable = false;
                for (String line : lines) {
                    String t = line.trim();
                    if (t.startsWith("LocalVariables:") || t.startsWith("LocalVariableTypes:")) {
                        inTable = true;
                        continue;
                    }
                    if (inTable) {
                        if (t.matches("\\d+\\s+\\d+\\s+\\d+\\s+#\\d+\\s+#\\d+.*")) {
                            n++;
                        } else if (!t.startsWith("Start")) {
                            inTable = false;
                        }
                    }
                }
                return n;
            }
            case END_VAR -> {
                return countLines(lines, "endvar");
            }
            case TRAP -> {
                // Instruction-level exception range events: endtry and catch.
                return countLines(lines, "endtry") + countLines(lines, "catch ");
            }
            case VAR_EVENT -> {
                return countLines(lines, " var ");
            }
            case TYPE_EVENT -> {
                return countLines(lines, " type ");
            }
            case STACK_EVENT -> {
                return countLines(lines, "stack_frame_type");
            }
            case LINE -> {
                int n = 0;
                for (String line : lines) {
                    if (line.trim().matches("//\\s+\\d+#")) {
                        n++;
                    }
                }
                return n;
            }
            default -> {
                return 0;
            }
        }
    }

    private static int countLines(String[] lines, String needle) {
        int n = 0;
        for (String line : lines) {
            if (line.contains(needle)) {
                n++;
            }
        }
        return n;
    }

    private static String runOnce(String... args) {
        TextOutput out = new TextOutput();
        return run(make(out, args), out);
    }

}
