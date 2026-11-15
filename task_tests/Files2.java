import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

/** Small filesystem helpers for generated test artifacts. */
public final class Files2 {
    private Files2() {
    }

    public static void ensureDirectory(final Path dir) throws IOException {
        if (!Files.exists(dir)) {
            Files.createDirectories(dir);
        }
    }
}
