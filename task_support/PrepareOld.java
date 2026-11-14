import java.nio.file.*;
import java.nio.charset.StandardCharsets;
public class PrepareOld {
    public static void main(String[] args) throws Exception {
        Path source = Paths.get("Procyon.CompilerTools/src/test/java/com/strobel/decompiler/AnnotationTests.java");
        String text = new String(Files.readAllBytes(source), StandardCharsets.UTF_8);
        // Adapt only JUnit discovery and the assertion-provider superclass.
        // All three upstream fixture definitions, test bodies and expected outputs remain verbatim.
        if (!text.contains("extends DecompilerTest") || !text.contains("import org.junit.Test;"))
            throw new AssertionError("upstream test shape changed");
        text = text.replace("import org.junit.Test;", "")
                   .replace("extends DecompilerTest", "extends OldAnnotationHarness")
                   .replace("@Test", "");
        Path output = Paths.get(".task-build/old-src/com/strobel/decompiler/AnnotationTests.java");
        Files.createDirectories(output.getParent());
        Files.write(output, text.getBytes(StandardCharsets.UTF_8));
    }
}
