import com.strobel.assembler.InputTypeLoader;
import com.strobel.assembler.ir.attributes.SourceAttribute;
import com.strobel.assembler.metadata.FieldDefinition;
import com.strobel.assembler.metadata.MethodDefinition;
import com.strobel.assembler.metadata.MetadataSystem;
import com.strobel.assembler.metadata.TypeDefinition;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotation;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotationPath;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotations;
import com.strobel.decompiler.Decompiler;
import com.strobel.decompiler.DecompilerSettings;
import com.strobel.decompiler.PlainTextOutput;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Drives the production decompiler purely from class file bytes and exposes the decoded
 * type-annotation metadata for assertions.  No input class is initialized or executed.
 */
public final class TaskAudit {
    private TaskAudit() {
    }

    /** One normalized type-annotation observation, independent of source formatting. */
    public static final class Record {
        public final String owner;
        public final String target;
        public final String path;
        public final String visibility;
        public final String annotationType;

        Record(final String owner, final TypeAnnotation ta) {
            this.owner = owner;
            this.target = ta.getTargetInfo().toString();
            this.path = renderPath(ta.getPath());
            this.visibility = ta.getVisibility().name();
            this.annotationType = ta.getAnnotation().getAnnotationType().getFullName();
        }

        static String renderPath(final TypeAnnotationPath path) {            final StringBuilder sb = new StringBuilder("[");

            for (int i = 0; i < path.size(); i++) {
                if (i > 0) {
                    sb.append(',');
                }

                final TypeAnnotationPath.Entry entry = path.get(i);
                sb.append(entry.getKind().name());

                if (entry.getKind() == TypeAnnotationPath.Kind.TYPE_ARGUMENT) {
                    sb.append('(').append(entry.getTypeArgumentIndex()).append(')');
                }
            }

            return sb.append(']').toString();
        }

        public String stableKey() {
            return owner + "|" + target + "|" + path + "|" + visibility + "|" + annotationType;
        }

        @Override
        public String toString() {
            return stableKey();
        }

        public static String renderPathPublic(final TypeAnnotationPath path) {
            return renderPath(path);
        }
    }

    public static MetadataSystem newMetadataSystem() {
        return new MetadataSystem(new InputTypeLoader());
    }

    public static TypeDefinition loadType(final MetadataSystem metadata, final Path classFile) {
        return metadata.lookupType(classFile.toString()).resolve();
    }

    public static String decompile(final Path classFile) {
        final PlainTextOutput output = new PlainTextOutput();
        final DecompilerSettings settings = DecompilerSettings.javaDefaults();
        settings.setTypeLoader(new InputTypeLoader());
        Decompiler.decompile(classFile.toString(), output, settings);
        return output.toString();
    }

    public static void writeSource(final Path file, final String source) throws IOException {
        Files.createDirectories(file.getParent());
        Files.write(file, source.getBytes(StandardCharsets.UTF_8));
    }

    /** Collects every type-use record (all targets, all visibilities) on a type and its members. */
    public static List<Record> collectAll(final TypeDefinition type) {
        final List<Record> records = new ArrayList<>();

        addRecords(records, "type:" + type.getFullName(), type.getSourceAttributes());

        for (final FieldDefinition field : type.getDeclaredFields()) {
            addRecords(records, "field:" + type.getFullName() + "." + field.getName(),
                       field.getSourceAttributes());
        }

        for (final MethodDefinition method : type.getDeclaredMethods()) {
            addRecords(records, "method:" + type.getFullName() + "." + method.getName() +
                                method.getErasedSignature(),
                       method.getSourceAttributes());
        }

        return records;
    }

    private static void addRecords(
        final List<Record> out,
        final String owner,
        final List<SourceAttribute> attributes) {

        for (final TypeAnnotation ta : TypeAnnotations.collect(attributes)) {
            out.add(new Record(owner, ta));
        }
    }

    public static List<String> stableKeys(final List<Record> records) {
        final List<String> keys = new ArrayList<>();

        for (final Record record : records) {
            keys.add(record.stableKey());
        }

        Collections.sort(keys);
        return keys;
    }
}
