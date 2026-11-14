import java.nio.file.*;
import java.util.*;
import com.strobel.decompiler.*;
import com.strobel.assembler.InputTypeLoader;
import com.strobel.assembler.metadata.*;
import com.strobel.assembler.ir.attributes.*;
public class Baseline {
    public static void main(String[] args) throws Exception {
        Path file = Paths.get(".task-build/fixtures/Fixture.class").toAbsolutePath();
        MetadataSystem metadata = new MetadataSystem(new InputTypeLoader());
        TypeDefinition type = metadata.lookupType(file.toString()).resolve();
        if (type == null || type.getDeclaredFields().size() != 1) throw new AssertionError("field metadata");
        FieldDefinition field = type.getDeclaredFields().get(0);
        SourceAttribute attribute = SourceAttribute.find("RuntimeVisibleTypeAnnotations", field.getSourceAttributes());
        if (attribute == null) throw new AssertionError("type attribute missing from source metadata");
        PlainTextOutput output = new PlainTextOutput();
        Decompiler.decompile(file.toString(), output, DecompilerSettings.javaDefaults());
        String text = output.toString();
        if (!text.contains("@Deprecated") || !text.contains("List<String[]>") || !text.contains("record Plain")) {
            throw new AssertionError("existing declaration/generic/record behavior: " + text);
        }
        Files.write(Paths.get(".task-build/baseline-output.java.txt"), text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        int annotations = text.split("@TUse", -1).length - 1;
        System.out.println("BASELINE_DECOMPILER_OK declaration=1 generic_array=1 record=1");
        System.out.println("TYPE_USE_OBSERVED attribute=" + attribute.getClass().getSimpleName() + " source_annotations=" + annotations);
    }
}
