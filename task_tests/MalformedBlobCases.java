import com.strobel.assembler.metadata.Buffer;
import com.strobel.assembler.metadata.DynamicCallSite;
import com.strobel.assembler.metadata.FieldReference;
import com.strobel.assembler.metadata.IMetadataScope;
import com.strobel.assembler.metadata.IMethodSignature;
import com.strobel.assembler.metadata.MethodHandle;
import com.strobel.assembler.metadata.MethodReference;
import com.strobel.assembler.metadata.TypeReference;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotationFormatException;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotationReader;

/**
 * Feeds deliberately malformed {@code RuntimeVisibleTypeAnnotations} blobs to the production
 * reader and asserts that each violation of JVMS 4.7.20 raises a
 * {@link TypeAnnotationFormatException} rather than being silently accepted or causing later
 * entries to be misread.
 */
final class MalformedBlobCases {
    private MalformedBlobCases() {
    }

    /** A minimal scope; constant-pool lookups are not reached for the malformed prefixes. */
    private static final IMetadataScope SCOPE = new IMetadataScope() {
        @Override
        public TypeReference lookupType(final int token) {
            return null;
        }

        @Override
        public FieldReference lookupField(final int token) {
            return null;
        }

        @Override
        public MethodReference lookupMethod(final int token) {
            return null;
        }

        @Override
        public MethodHandle lookupMethodHandle(final int token) {
            return null;
        }

        @Override
        public IMethodSignature lookupMethodType(final int token) {
            return null;
        }

        @Override
        public DynamicCallSite lookupDynamicCallSite(final int token) {
            return null;
        }

        @Override
        public FieldReference lookupField(final int typeToken, final int nameAndTypeToken) {
            return null;
        }

        @Override
        public MethodReference lookupMethod(final int typeToken, final int nameAndTypeToken) {
            return null;
        }

        @Override
        @SuppressWarnings("unchecked")
        public <T> T lookupConstant(final int token) {
            return (T) "X";
        }

        @Override
        public Object lookup(final int token) {
            return null;
        }
    };

    static void runAll() {
        final String visible = "RuntimeVisibleTypeAnnotations";

        // Unknown target_type.
        expectReject("unknown target_type", () ->
            TypeAnnotationReader.read(visible, 3, SCOPE, buffer(0, 1, 0x7F, 0, 0)));

        // Invalid type_path_kind.
        expectReject("invalid path kind", () ->
            TypeAnnotationReader.read(visible, 8, SCOPE, buffer(0, 1, 0x13, 1, 9, 0, 0, 0)));

        // Non-zero reserved type_argument_index on an ARRAY_ELEMENT path step.
        expectReject("non-zero reserved path byte", () ->
            TypeAnnotationReader.read(visible, 8, SCOPE, buffer(0, 1, 0x13, 1, 0, 5, 0, 0)));

        // Truncated blob: claims five entries but provides none.
        expectReject("truncated blob", () ->
            TypeAnnotationReader.read(visible, 2, SCOPE, buffer(0, 5)));

        // Local variable table count larger than the remaining bytes.
        expectReject("oversized localvar table", () ->
            TypeAnnotationReader.read(visible, 4, SCOPE, buffer(0, 1, 0x40, 1, 0)));

        // Wrong attribute name is rejected.
        expectReject("wrong attribute name", () ->
            TypeAnnotationReader.read("RuntimeVisibleAnnotations", 1, SCOPE, buffer(0)));
    }

    private static void expectReject(final String name, final Runnable caseBody) {
        try {
            caseBody.run();
            throw new AssertionError("malformed blob accepted: " + name);
        }
        catch (final TypeAnnotationFormatException expected) {
            // Expected: the reader identified the JVMS violation.
            TaskCheck.check(true, "rejected malformed blob: " + name);
        }
    }

    private static Buffer buffer(final int... bytes) {
        final Buffer buffer = new Buffer(bytes.length);

        for (final int b : bytes) {
            buffer.writeByte((byte) b);
        }

        buffer.position(0);
        return buffer;
    }
}
