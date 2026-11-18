import org.openjdk.asmtools.common.inputs.ByteInput;
import org.openjdk.asmtools.common.outputs.TextOutput;
import org.openjdk.asmtools.common.outputs.log.StringLog;
import org.openjdk.asmtools.jdis.Main;
import org.openjdk.asmtools.jdis.Options;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Probe used by instance_options_contract.cpp.
 * <p>
 * The probe drives the production {@link Main} only through its public API.  Fixture
 * class files are read as raw bytes and are never loaded or executed.
 * <p>
 * Every produced disassembly is printed between section markers so that the C++ side can
 * compare real bodies byte for byte.  Each command also emits a {@code REF*} section
 * produced by a fresh, isolated instance with the same arguments, inside the same JVM,
 * so that comparing a section with its reference never depends on anything but the
 * disassembler's own state.
 */
public final class InstanceOptionsProbe {

    private static final String DETAILED_ARGS = "-g,-table,-lvt,-lnt:numbers";

    private InstanceOptionsProbe() {
    }

    public static void main(String[] args) throws Exception {
        String command = args[0];
        Path fixture = Path.of(args[1]);
        byte[] bytes = Files.readAllBytes(fixture);
        switch (command) {
            case "body" -> body(bytes, args);
            case "default-then-detailed" -> defaultThenDetailed(bytes);
            case "detailed-then-default" -> detailedThenDefault(bytes);
            case "interleaved" -> interleaved(bytes);
            case "reargs" -> reargs(bytes);
            case "parallel" -> parallel(bytes);
            case "repeat" -> repeat(bytes, args);
            case "format-pair" -> formatPair(bytes, args);
            case "legacy-static" -> legacyStatic(bytes);
            default -> throw new IllegalArgumentException(command);
        }
    }

    // ------------------------------------------------------------------ helpers

    private static String[] split(String spec) {
        if (spec.isEmpty()) {
            return new String[0];
        }
        return spec.split(",");
    }

    private static Main main(TextOutput out, byte[] bytes, String... argv) {
        return new Main(out, new StringLog(), new ByteInput(bytes), argv);
    }

    private static Main mainDefault(TextOutput out, byte[] bytes) {
        return new Main(out, new StringLog(), new ByteInput(bytes));
    }

    private static String lastBody(TextOutput out) {
        List<TextOutput.NamedSource> outputs = out.getOutputs();
        if (outputs.isEmpty()) {
            throw new IllegalStateException("disassembly produced no output");
        }
        return outputs.get(outputs.size() - 1).getBody();
    }

    private static String run(Main main, TextOutput out) {
        int rc = main.disasm();
        if (rc != 0) {
            throw new IllegalStateException("disassembly failed: " + rc);
        }
        return lastBody(out);
    }

    private static void emit(String name, String body) {
        System.out.print("@@BEGIN " + name + "\n");
        System.out.print(body);
        System.out.print("@@END " + name + "\n");
    }

    /** A fresh isolated instance with the caller-provided arguments. */
    private static String isolated(byte[] bytes, String... argv) {
        TextOutput out = new TextOutput();
        return run(main(out, bytes, argv), out);
    }

    /**
     * A fresh isolated instance with the default (no option) configuration.  An explicit
     * empty argument list is used so that the reference is computed from the documented
     * default rather than from whatever legacy process-wide state happens to exist.
     */
    private static String isolatedDefault(byte[] bytes) {
        return isolated(bytes);
    }

    // ---------------------------------------------------------------- commands

    private static void body(byte[] bytes, String[] args) {
        String[] argv = split(args.length > 2 ? args[2] : "");
        emit("B", isolated(bytes, argv));
    }

    /** Default instance created first, configured instance second; run in creation order. */
    private static void defaultThenDetailed(byte[] bytes) {
        TextOutput aOut = new TextOutput();
        Main a = mainDefault(aOut, bytes);
        TextOutput bOut = new TextOutput();
        Main b = main(bOut, bytes, split(DETAILED_ARGS));
        emit("A", run(a, aOut));
        emit("B", run(b, bOut));
        emit("REF_A", isolatedDefault(bytes));
        emit("REF_B", isolated(bytes, split(DETAILED_ARGS)));
    }

    /** Configured instance created first, default instance second; run in creation order. */
    private static void detailedThenDefault(byte[] bytes) {
        TextOutput bOut = new TextOutput();
        Main b = main(bOut, bytes, split(DETAILED_ARGS));
        TextOutput aOut = new TextOutput();
        Main a = mainDefault(aOut, bytes);
        emit("B", run(b, bOut));
        emit("A", run(a, aOut));
        emit("REF_A", isolatedDefault(bytes));
        emit("REF_B", isolated(bytes, split(DETAILED_ARGS)));
    }

    /** Alternating executions of two live instances. */
    private static void interleaved(byte[] bytes) {
        TextOutput aOut = new TextOutput();
        Main a = mainDefault(aOut, bytes);
        TextOutput bOut = new TextOutput();
        Main b = main(bOut, bytes, split(DETAILED_ARGS));
        emit("A1", run(a, aOut));
        emit("B1", run(b, bOut));
        emit("A2", run(a, aOut));
        emit("B2", run(b, bOut));
        emit("REF_A", isolatedDefault(bytes));
        emit("REF_B", isolated(bytes, split(DETAILED_ARGS)));
    }

    /** The same instance is given a new argument list several times. */
    private static void reargs(byte[] bytes) {
        TextOutput out = new TextOutput();
        Main m = main(out, bytes);
        emit("R1", run(m, out));
        emit("R2", run(m, out));
        boolean ok = m.disasm(split(DETAILED_ARGS));
        if (!ok) {
            throw new IllegalStateException("detailed disassembly failed");
        }
        emit("D", lastBody(out));
        emit("R3", run(m, out));
        emit("REF_A", isolatedDefault(bytes));
        emit("REF_D", isolated(bytes, split(DETAILED_ARGS)));
    }

    /** Two instances, two threads, two output objects, running at the same time. */
    private static void parallel(byte[] bytes) throws Exception {
        TextOutput aOut = new TextOutput();
        TextOutput bOut = new TextOutput();
        Main a = mainDefault(aOut, bytes);
        Main b = main(bOut, bytes, split(DETAILED_ARGS));
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch start = new CountDownLatch(1);
        Thread ta = new Thread(() -> {
            try {
                start.await();
                run(a, aOut);
            } catch (Throwable t) {
                failure.compareAndSet(null, t);
            }
        });
        Thread tb = new Thread(() -> {
            try {
                start.await();
                run(b, bOut);
            } catch (Throwable t) {
                failure.compareAndSet(null, t);
            }
        });
        ta.start();
        tb.start();
        start.countDown();
        ta.join();
        tb.join();
        if (failure.get() != null) {
            throw new IllegalStateException(failure.get());
        }
        emit("A", lastBody(aOut));
        emit("B", lastBody(bOut));
        emit("REF_A", isolatedDefault(bytes));
        emit("REF_B", isolated(bytes, split(DETAILED_ARGS)));
    }

    /** Repeated execution of one instance with the same options. */
    private static void repeat(byte[] bytes, String[] args) {
        String[] argv = split(args.length > 2 ? args[2] : DETAILED_ARGS);
        TextOutput out = new TextOutput();
        Main m = main(out, bytes, argv);
        int first = out.getOutputs().size();
        emit("FIRST", run(m, out));
        emit("SECOND", run(m, out));
        emit("THIRD", run(m, out));
        if (out.getOutputs().size() != first + 3) {
            throw new IllegalStateException("unexpected output count");
        }
        emit("REF", isolated(bytes, argv));
    }

    /** Two live instances with two different option sets, plus isolated references. */
    private static void formatPair(byte[] bytes, String[] args) {
        String spec = args[2];
        int sep = spec.indexOf(';');
        String[] first = split(spec.substring(0, sep));
        String[] second = split(spec.substring(sep + 1));
        TextOutput aOut = new TextOutput();
        TextOutput bOut = new TextOutput();
        Main a = main(aOut, bytes, first);
        Main b = main(bOut, bytes, second);
        emit("A", run(a, aOut));
        emit("B", run(b, bOut));
        emit("REF_A", isolated(bytes, first));
        emit("REF_B", isolated(bytes, second));
    }

    /**
     * Checks that a configured instance neither reads nor writes the legacy process-wide
     * option set, while the no-argument entry keeps its historical default behaviour.
     */
    private static void legacyStatic(byte[] bytes) {
        // Start from the documented default.
        Options.setDefaultOutputOptions();
        emit("GLOBAL_BEFORE", Options.asShortString() + "\n");

        TextOutput detailedOut = new TextOutput();
        Main detailed = main(detailedOut, bytes, split(DETAILED_ARGS));
        emit("A", run(detailed, detailedOut));
        emit("REF_A", isolated(bytes, split(DETAILED_ARGS)));

        // Configuring and running another instance must not touch the legacy state.
        emit("GLOBAL_AFTER", Options.asShortString() + "\n");
        emit("GLOBAL_CONTAINS_CP", String.valueOf(Options.contains(Options.PrintOption.CONSTANT_POOL)) + "\n");

        // The no-argument constructor form keeps following the legacy default.
        TextOutput defaultOut = new TextOutput();
        Main plain = mainDefault(defaultOut, bytes);
        emit("D", run(plain, defaultOut));
        emit("REF_D", isolatedDefault(bytes));

        // A later legacy change is visible only to instances that were never configured.
        Options.setDetailedOutputOptions();
        emit("D2", run(plain, defaultOut));
        emit("REF_D2", isolated(bytes, "-g"));
        Options.setDefaultOutputOptions();
    }
}
