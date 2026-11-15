import javax.tools.JavaCompiler;
import javax.tools.JavaFileObject;
import javax.tools.SimpleJavaFileObject;
import javax.tools.StandardJavaFileManager;
import javax.tools.ToolProvider;
import java.io.File;
import java.io.IOException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * Compiles in-memory or on-disk Java sources with {@code javac --release 17 -proc:none},
 * mirroring the class files the task targets.  Only the JDK's {@link JavaCompiler} is used; no
 * input class is loaded or executed.
 */
public final class TaskCompiler {
    private TaskCompiler() {
    }

    /** One source unit handed to the compiler. */
    public static final class Source {
        final String className;
        final String code;

        public Source(final String className, final String code) {
            this.className = className;
            this.code = code;
        }
    }

    private static final class StringSource extends SimpleJavaFileObject {
        private final String code;

        StringSource(final String className, final String code) {
            super(URI.create("string:///" + className.replace('.', '/') + Kind.SOURCE.extension),
                  Kind.SOURCE);
            this.code = code;
        }

        @Override
        public CharSequence getCharContent(final boolean ignoreEncodingErrors) {
            return code;
        }
    }

    /**
     * Compiles source strings into {@code outputDir}.
     *
     * @param classPath extra class path entries (for shared annotations)
     */
    public static void compileStrings(
        final List<Source> sources,
        final Path outputDir,
        final List<Path> classPath) throws IOException {

        Files.createDirectories(outputDir);

        final JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();

        try (final StandardJavaFileManager fileManager =
                 compiler.getStandardFileManager(null, Locale.ROOT, StandardCharsets.UTF_8)) {

            final List<String> options = new ArrayList<>(Arrays.asList(
                "--release", "17",
                "-proc:none",
                "-parameters",
                "-d", outputDir.toString()
            ));

            if (classPath != null && !classPath.isEmpty()) {
                final StringBuilder cp = new StringBuilder();

                for (final Path entry : classPath) {
                    if (cp.length() > 0) {
                        cp.append(File.pathSeparator);
                    }
                    cp.append(entry.toString());
                }

                options.add("-cp");
                options.add(cp.toString());
            }

            final List<JavaFileObject> units = new ArrayList<>();

            for (final Source source : sources) {
                units.add(new StringSource(source.className, source.code));
            }

            final JavaCompiler.CompilationTask task =
                compiler.getTask(null, fileManager, null, options, null, units);

            if (!task.call()) {
                throw new IOException("Compilation failed for: " + names(sources));
            }
        }
    }

    public static void compileFiles(final List<Path> files, final Path outputDir, final List<Path> classPath)
        throws IOException {

        Files.createDirectories(outputDir);

        final JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();

        try (final StandardJavaFileManager fileManager =
                 compiler.getStandardFileManager(null, Locale.ROOT, StandardCharsets.UTF_8)) {

            final List<String> options = new ArrayList<>(Arrays.asList(
                "--release", "17",
                "-proc:none",
                "-parameters",
                "-d", outputDir.toString()
            ));

            if (classPath != null && !classPath.isEmpty()) {
                final StringBuilder cp = new StringBuilder();

                for (final Path entry : classPath) {
                    if (cp.length() > 0) {
                        cp.append(File.pathSeparator);
                    }
                    cp.append(entry.toString());
                }

                options.add("-cp");
                options.add(cp.toString());
            }

            final Iterable<? extends JavaFileObject> units =
                fileManager.getJavaFileObjectsFromPaths(files);

            final JavaCompiler.CompilationTask task =
                compiler.getTask(null, fileManager, null, options, null, units);

            if (!task.call()) {
                throw new IOException("Compilation failed for files: " + files);
            }
        }
    }

    private static String names(final List<Source> sources) {
        final List<String> names = new ArrayList<>();

        for (final Source source : sources) {
            names.add(source.className);
        }

        return names.toString();
    }
}
