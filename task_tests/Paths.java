import java.nio.file.Path;

/**
 * Resolves a writable working directory for generated test sources and classes.  Uses the
 * current working directory (the repository root when run through {@code task_support/run.sh}),
 * keeping all generated artifacts under {@code .task-build}.
 */
public final class Paths {
    private Paths() {
    }

    public static Path getWorkDir() {
        return java.nio.file.Paths.get(".task-build", "type-annotation-tests")
                                .toAbsolutePath()
                                .normalize();
    }
}
