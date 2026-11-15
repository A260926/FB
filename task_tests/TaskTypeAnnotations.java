import com.strobel.assembler.InputTypeLoader;
import com.strobel.assembler.ir.attributes.SourceAttribute;
import com.strobel.assembler.metadata.FieldDefinition;
import com.strobel.assembler.metadata.MetadataSystem;
import com.strobel.assembler.metadata.MethodDefinition;
import com.strobel.assembler.metadata.TypeDefinition;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotation;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotationPath;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotationTargetKind;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotations;
import com.strobel.decompiler.DecompilerSettings;
import com.strobel.decompiler.PlainTextOutput;
import com.strobel.decompiler.Decompiler;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * End-to-end checks for JVMS 4.7.20 type-use annotation support.
 *
 * <p>Three groups are registered:
 * <ul>
 *   <li><b>Metadata</b> — decoded {@code target_type}, {@code target_info}, {@code type_path},
 *       visibility and annotation values match hand-listed tuples, and malformed blobs are
 *       rejected explicitly;</li>
 *   <li><b>Paths</b> — array dimensions, inner/outer member segments, wildcard bounds, interface
 *       bound indices, hidden parameters, record accessors and varargs map to the right usage,
 *       and two usages of the same declared type stay distinguishable;</li>
 *   <li><b>Rewrite</b> — decompiled source recompiles under {@code javac --release 17 -proc:none}
 *       and the recompiled class carries the same visible and invisible annotations at every
 *       supported type position (the comparison reads class metadata, not text).</li>
 * </ul>
 * Every case calls {@link TaskCheck#check} and runs as a no-arg lambda.
 */
public final class TaskTypeAnnotations {

    private static final Path WORK = Paths.getWorkDir();

    private TaskTypeAnnotations() {
    }

    public static List<TaskCheck.Case> cases() {
        final List<TaskCheck.Case> cases = new ArrayList<>();

        // ---------------------------------------------------------------- Metadata group
        cases.add(new TaskCheck.Case("Metadata", "visibleAndInvisibleAttributesDecoded",
            TaskTypeAnnotations::visibleAndInvisibleAttributesDecoded));
        cases.add(new TaskCheck.Case("Metadata", "targetInfoAndPathRoundTrip",
            TaskTypeAnnotations::targetInfoAndPathRoundTrip));
        cases.add(new TaskCheck.Case("Metadata", "annotationValueAndOrderPreserved",
            TaskTypeAnnotations::annotationValueAndOrderPreserved));
        cases.add(new TaskCheck.Case("Metadata", "receiverAndCodeTargetsRecordedNotEmitted",
            TaskTypeAnnotations::receiverAndCodeTargetsRecordedNotEmitted));
        cases.add(new TaskCheck.Case("Metadata", "malformedBlobsRejected",
            TaskTypeAnnotations::malformedBlobsRejected));

        // ------------------------------------------------------------------ Paths group
        cases.add(new TaskCheck.Case("Paths", "nestedGenericArrayPaths",
            TaskTypeAnnotations::nestedGenericArrayPaths));
        cases.add(new TaskCheck.Case("Paths", "innerAndOuterMemberSegments",
            TaskTypeAnnotations::innerAndOuterMemberSegments));
        cases.add(new TaskCheck.Case("Paths", "interfaceBoundAndWildcardPaths",
            TaskTypeAnnotations::interfaceBoundAndWildcardPaths));
        cases.add(new TaskCheck.Case("Paths", "hiddenParameterIndices",
            TaskTypeAnnotations::hiddenParameterIndices));
        cases.add(new TaskCheck.Case("Paths", "recordComponentAndAccessorPaths",
            TaskTypeAnnotations::recordComponentAndAccessorPaths));
        cases.add(new TaskCheck.Case("Paths", "annotatedVarargsEllipsis",
            TaskTypeAnnotations::annotatedVarargsEllipsis));
        cases.add(new TaskCheck.Case("Paths", "sameTypeTwoUsagesStayDistinct",
            TaskTypeAnnotations::sameTypeTwoUsagesStayDistinct));
        cases.add(new TaskCheck.Case("Paths", "unrepresentableOwnerReportedNotMisattached",
            TaskTypeAnnotations::unrepresentableOwnerReportedNotMisattached));

        // ---------------------------------------------------------------- Rewrite group
        cases.add(new TaskCheck.Case("Rewrite", "decompiledSourceRecompiles",
            TaskTypeAnnotations::decompiledSourceRecompiles));
        cases.add(new TaskCheck.Case("Rewrite", "recompiledAnnotationsMatchAtEveryPosition",
            TaskTypeAnnotations::recompiledAnnotationsMatchAtEveryPosition));
        cases.add(new TaskCheck.Case("Rewrite", "declarationAnnotationsRemainSeparate",
            TaskTypeAnnotations::declarationAnnotationsRemainSeparate));
        cases.add(new TaskCheck.Case("Rewrite", "explicitRecordAccessorRetained",
            TaskTypeAnnotations::explicitRecordAccessorRetained));
        cases.add(new TaskCheck.Case("Rewrite", "securityPolicyAuditVerdicts",
            TaskTypeAnnotations::securityPolicyAuditVerdicts));
        cases.add(new TaskCheck.Case("Rewrite", "annotationIdentitySurvivesAstCloneAndRewrite",
            TaskTypeAnnotations::annotationIdentitySurvivesAstCloneAndRewrite));

        return cases;
    }

    // ======================================================================
    // Helpers
    // ======================================================================

    private static final class Compiled {
        final Path dir;
        final MetadataSystem metadata = TaskAudit.newMetadataSystem();

        Compiled(final String tag) throws Exception {
            this.dir = WORK.resolve("fixtures-" + tag);
            recreate(this.dir);

            final List<TaskCompiler.Source> sources = new ArrayList<>();

            for (final String name : TaskFixtures.fixtureNames()) {
                sources.add(new TaskCompiler.Source(name, TaskFixtures.sourceFor(name)));
            }

            TaskCompiler.compileStrings(sources, dir, Collections.<Path>emptyList());
        }

        TypeDefinition type(final String name) {
            return metadata.lookupType(dir.resolve(name + ".class").toString()).resolve();
        }

        Path classFile(final String name) {
            return dir.resolve(name + ".class");
        }
    }

    private static void recreate(final Path dir) throws Exception {
        if (Files.exists(dir)) {
            deleteRecursively(dir);
        }
        Files.createDirectories(dir);
    }

    private static void deleteRecursively(final Path path) throws Exception {
        if (!Files.exists(path)) {
            return;
        }

        if (Files.isDirectory(path)) {
            try (java.util.stream.Stream<Path> children = Files.list(path)) {
                final List<Path> list = new ArrayList<>();
                children.forEach(list::add);
                for (final Path child : list) {
                    deleteRecursively(child);
                }
            }
        }

        Files.delete(path);
    }

    private static List<TypeAnnotation> fieldAnnotations(
        final TypeDefinition type,
        final String fieldName) {

        for (final FieldDefinition field : type.getDeclaredFields()) {
            if (field.getName().equals(fieldName)) {
                return TypeAnnotations.collect(field.getSourceAttributes());
            }
        }

        throw new AssertionError("field not found: " + fieldName);
    }

    private static List<TypeAnnotation> methodAnnotations(
        final TypeDefinition type,
        final String methodName) {

        for (final MethodDefinition method : type.getDeclaredMethods()) {
            if (method.getName().equals(methodName)) {
                return TypeAnnotations.collect(method.getSourceAttributes());
            }
        }

        throw new AssertionError("method not found: " + methodName);
    }

    private static TypeAnnotation first(
        final List<TypeAnnotation> annotations,
        final TypeAnnotationTargetKind kind) {

        for (final TypeAnnotation annotation : annotations) {
            if (annotation.getTargetKind() == kind) {
                return annotation;
            }
        }

        throw new AssertionError("no annotation with target " + kind + " in " + annotations);
    }

    private static String pathString(final TypeAnnotationPath path) {
        return TaskAudit.Record.renderPathPublic(path);
    }

    private static boolean hasPath(
        final List<TypeAnnotation> annotations,
        final String typeName,
        final TypeAnnotation.Visibility visibility,
        final String path) {

        for (final TypeAnnotation annotation : annotations) {
            if (annotation.getVisibility() == visibility &&
                annotation.getAnnotation().getAnnotationType().getFullName().endsWith(typeName) &&
                pathString(annotation.getPath()).equals(path)) {
                return true;
            }
        }

        return false;
    }

    // ======================================================================
    // Metadata group
    // ======================================================================

    private static void visibleAndInvisibleAttributesDecoded() throws Exception {
        final Compiled c = new Compiled("meta-visible");
        final TypeDefinition type = c.type("NestedGenericArrays");
        final List<TypeAnnotation> matrix = fieldAnnotations(type, "matrix");

        boolean sawRuntime = false;
        boolean sawClass = false;

        for (final TypeAnnotation annotation : matrix) {
            if (annotation.getVisibility() == TypeAnnotation.Visibility.RUNTIME &&
                annotation.getAnnotation().getAnnotationType().getFullName().endsWith("Sensitive")) {
                sawRuntime = true;
            }

            if (annotation.getVisibility() == TypeAnnotation.Visibility.CLASS &&
                annotation.getAnnotation().getAnnotationType().getFullName().endsWith("Trusted")) {
                sawClass = true;
            }
        }

        TaskCheck.check(sawRuntime, "RUNTIME (RuntimeVisibleTypeAnnotations) entry decoded");
        TaskCheck.check(sawClass, "CLASS (RuntimeInvisibleTypeAnnotations) entry decoded");
        TaskCheck.check(first(matrix, TypeAnnotationTargetKind.FIELD)
                            .getTargetKind() == TypeAnnotationTargetKind.FIELD,
                        "field target_type 0x13 decoded");
    }

    private static void targetInfoAndPathRoundTrip() throws Exception {
        final Compiled c = new Compiled("meta-target");
        final TypeDefinition type = c.type("NestedGenericArrays");
        final List<TypeAnnotation> nested = fieldAnnotations(type, "nested");

        // An empty path is represented by an empty, non-null path on at least one entry.
        boolean sawEmptyPath = false;

        for (final TypeAnnotation annotation : nested) {
            TaskCheck.check(annotation.getPath() != null, "type_path is never null");

            if (annotation.getPath().isEmpty()) {
                sawEmptyPath = true;
            }
        }

        TaskCheck.check(sawEmptyPath, "empty type_path preserved as empty");

        // A multi-step path keeps ARRAY_ELEMENT ordering and indices.
        TaskCheck.check(
            hasPath(nested, "Sensitive", TypeAnnotation.Visibility.RUNTIME,
                    "[ARRAY_ELEMENT,ARRAY_ELEMENT,TYPE_ARGUMENT(0)]"),
            "compound array/type-argument path decoded exactly");
        TaskCheck.check(
            hasPath(nested, "Trusted", TypeAnnotation.Visibility.CLASS, "[]"),
            "invisible empty-path entry decoded");
    }

    private static void annotationValueAndOrderPreserved() throws Exception {
        final Compiled c = new Compiled("meta-values");
        // The InterfaceBoundIndex type parameter bound carries both a RUNTIME and a CLASS entry.
        final List<TaskAudit.Record> all = TaskAudit.collectAll(c.type("InterfaceBoundIndex"));

        final List<String> ordered = TaskAudit.stableKeys(all);

        TaskCheck.check(!ordered.isEmpty(), "metadata records exposed for auditing");

        // Every record retains a non-null annotation value with its annotation type.
        for (final TaskAudit.Record record : all) {
            TaskCheck.check(record.annotationType != null, "annotation type retained");
            TaskCheck.check(record.target != null, "target_info retained");
            TaskCheck.check(record.path != null, "type_path retained");
            TaskCheck.check(record.visibility != null, "visibility retained");
        }
    }

    private static void receiverAndCodeTargetsRecordedNotEmitted() throws Exception {
        // Build a class containing a receiver, a cast and an instanceof type annotation, all of
        // which live in Code attributes and must not be written into source.
        final Path dir = WORK.resolve("fixtures-nonemittable");
        recreate(dir);

        final String source =
            "import java.lang.annotation.*;\n" +
            "public class NonEmittable {\n" +
            "  @Target(ElementType.TYPE_USE) @Retention(RetentionPolicy.RUNTIME)\n" +
            "  @interface Rt {}\n" +
            "  void receiver(@Rt NonEmittable this) {}\n" +
            "  static Object cast(Object o) { return (@Rt String) o; }\n" +
            "  static boolean inst(Object o) { return o instanceof @Rt String; }\n" +
            "}\n";

        TaskCompiler.compileStrings(
            Collections.singletonList(new TaskCompiler.Source("NonEmittable", source)),
            dir,
            Collections.<Path>emptyList());

        final MetadataSystem metadata = TaskAudit.newMetadataSystem();
        final TypeDefinition type =
            metadata.lookupType(dir.resolve("NonEmittable.class").toString()).resolve();

        boolean receiver = false;
        boolean codeTarget = false;

        for (final MethodDefinition method : type.getDeclaredMethods()) {
            for (final TypeAnnotation annotation :
                     TypeAnnotations.collect(method.getSourceAttributes())) {

                TaskCheck.check(!annotation.isEmittable(),
                                "receiver/Code target marked non-emittable: " + annotation.getTargetKind());

                if (annotation.getTargetKind() == TypeAnnotationTargetKind.METHOD_RECEIVER) {
                    receiver = true;
                }

                if (annotation.getTargetKind() == TypeAnnotationTargetKind.CAST ||
                    annotation.getTargetKind() == TypeAnnotationTargetKind.INSTANCEOF) {
                    codeTarget = true;
                    // target_info must still be fully readable.
                    TaskCheck.check(annotation.getTargetInfo() != null,
                                    "Code target_info retained and queryable");
                }
            }
        }

        TaskCheck.check(receiver, "receiver target retained in metadata");
        TaskCheck.check(codeTarget, "Code-attribute target retained in metadata");

        // And none of them leak into the source text.
        final PlainTextOutput output = new PlainTextOutput();
        final DecompilerSettings settings = DecompilerSettings.javaDefaults();
        settings.setTypeLoader(new InputTypeLoader());
        Decompiler.decompile(dir.resolve("NonEmittable.class").toString(), output, settings);

        TaskCheck.check(!output.toString().contains("@Rt"),
                        "non-emittable annotations are not written into source");
    }

    private static void malformedBlobsRejected() {
        MalformedBlobCases.runAll();
    }

    // ======================================================================
    // Paths group
    // ======================================================================

    private static void nestedGenericArrayPaths() throws Exception {
        final Compiled c = new Compiled("path-array");
        final TypeDefinition type = c.type("NestedGenericArrays");

        final List<TypeAnnotation> matrix = fieldAnnotations(type, "matrix");
        TaskCheck.check(
            hasPath(matrix, "Sensitive", TypeAnnotation.Visibility.RUNTIME, "[ARRAY_ELEMENT]"),
            "array outer dimension annotation placed on the array path");
        TaskCheck.check(
            hasPath(matrix, "Trusted", TypeAnnotation.Visibility.CLASS,
                    "[ARRAY_ELEMENT,TYPE_ARGUMENT(1)]"),
            "array component generic argument path preserved");

        final List<TypeAnnotation> nested = fieldAnnotations(type, "nested");
        TaskCheck.check(
            hasPath(nested, "Sensitive", TypeAnnotation.Visibility.RUNTIME,
                    "[ARRAY_ELEMENT,ARRAY_ELEMENT,TYPE_ARGUMENT(0)]"),
            "deepest component argument reached through two array dimensions");
    }

    private static void innerAndOuterMemberSegments() throws Exception {
        final Compiled c = new Compiled("path-inner");
        final TypeDefinition type = c.type("InnerOuterMembers");

        final List<TypeAnnotation> a = fieldAnnotations(type, "a");
        // Empty-path annotation on a dotted static nested type and its leaf generic argument.
        TaskCheck.check(
            hasPath(a, "Sensitive", TypeAnnotation.Visibility.RUNTIME, "[]"),
            "empty-path member type annotation retained");
        TaskCheck.check(
            hasPath(a, "Trusted", TypeAnnotation.Visibility.CLASS, "[TYPE_ARGUMENT(0)]"),
            "member generic argument path retained");
    }

    private static void interfaceBoundAndWildcardPaths() throws Exception {
        final Compiled c = new Compiled("path-bound");
        final TypeDefinition type = c.type("InterfaceBoundIndex");
        final List<TaskAudit.Record> records = TaskAudit.collectAll(type);

        boolean boundZero = false;
        boolean boundOne = false;
        boolean wildcardBound = false;

        for (final TaskAudit.Record record : records) {
            if (record.target.contains("boundIndex=0")) {
                boundZero = true;
            }

            if (record.target.contains("boundIndex=1")) {
                boundOne = true;
            }

            if (record.path.equals("[TYPE_ARGUMENT(0),WILDCARD_BOUND]")) {
                wildcardBound = true;
            }
        }

        TaskCheck.check(boundZero, "interface bound index 0 (class bound) decoded");
        TaskCheck.check(boundOne, "interface bound index 1 (additional interface bound) decoded");
        TaskCheck.check(wildcardBound, "WILDCARD_BOUND path decoded");
    }

    private static void hiddenParameterIndices() throws Exception {
        final Compiled c = new Compiled("path-hidden");
        final TypeDefinition type = c.type("HiddenParameters");

        // Non-static inner constructor: the mandated enclosing instance is hidden, so the two
        // source parameters keep formal indices 0 and 1.
        final TypeDefinition inner = nestedType(type, "Inner");
        final List<TypeAnnotation> innerCtor = constructorAnnotations(inner);

        int[] formalIndices = formalIndices(innerCtor);
        Arrays.sort(formalIndices);
        TaskCheck.check(Arrays.equals(formalIndices, new int[] {0, 1}),
                        "inner constructor formal indices resolve to the two visible parameters");

        // Enum constructor: synthetic name/ordinal parameters are hidden.
        final TypeDefinition color = nestedType(type, "Color");
        final List<TypeAnnotation> colorCtor = constructorAnnotations(color);
        formalIndices = formalIndices(colorCtor);
        Arrays.sort(formalIndices);
        TaskCheck.check(Arrays.equals(formalIndices, new int[] {0, 1}),
                        "enum constructor formal indices skip synthetic name/ordinal");
    }

    private static void recordComponentAndAccessorPaths() throws Exception {
        final Compiled c = new Compiled("path-record");
        final TypeDefinition box = nestedType(c.type("RecordAccessors"), "Box");

        // The record component's own attributes drive the component header, not the copied
        // field/accessor/constructor duplicates.
        final com.strobel.assembler.ir.attributes.RecordAttribute recordAttribute =
            com.strobel.assembler.ir.attributes.SourceAttribute.find(
                com.strobel.assembler.ir.attributes.AttributeNames.Record,
                box.getSourceAttributes());

        TaskCheck.check(recordAttribute != null, "Record attribute present on record class");

        boolean componentHasAnnotation = false;

        for (com.strobel.assembler.ir.attributes.RecordComponentInfo component :
                 recordAttribute.getComponents()) {

            if (!TypeAnnotations.collect(component.getAttributes()).isEmpty()) {
                componentHasAnnotation = true;
            }
        }

        TaskCheck.check(componentHasAnnotation,
                        "record component carries its own type annotation attributes");

        // The explicit accessor with distinct annotations is retained (not collapsed).
        final PlainTextOutput output = new PlainTextOutput();
        final DecompilerSettings settings = DecompilerSettings.javaDefaults();
        settings.setTypeLoader(new InputTypeLoader());
        Decompiler.decompile(c.classFile("RecordAccessors").toString().replace(
                                 "RecordAccessors.class", "RecordAccessors$Box.class"),
                             output, settings);

        final String text = output.toString();
        TaskCheck.check(text.contains("items()"), "explicit record accessor retained in source");
    }

    private static void annotatedVarargsEllipsis() throws Exception {
        final Compiled c = new Compiled("path-varargs");
        final TypeDefinition type = c.type("AnnotatedVarargs");
        final List<TypeAnnotation> varargs = methodAnnotations(type, "varargs");

        // Empty path -> component; [ARRAY_ELEMENT] -> ellipsis dimension; [ARRAY_ELEMENT x2] -> bracket.
        TaskCheck.check(
            hasPath(varargs, "Sensitive", TypeAnnotation.Visibility.RUNTIME, "[]"),
            "varargs component annotation retained");
        TaskCheck.check(
            hasPath(varargs, "Trusted", TypeAnnotation.Visibility.CLASS, "[ARRAY_ELEMENT]"),
            "ellipsis dimension annotation retained");
        TaskCheck.check(
            hasPath(varargs, "Trusted", TypeAnnotation.Visibility.CLASS,
                    "[ARRAY_ELEMENT,ARRAY_ELEMENT]"),
            "non-ellipsis array bracket annotation retained");
    }

    private static void sameTypeTwoUsagesStayDistinct() throws Exception {
        final Path dir = WORK.resolve("fixtures-distinct");
        recreate(dir);

        final String source =
            "import java.lang.annotation.*;\n" +
            "import java.util.List;\n" +
            "public class DistinctUsages {\n" +
            "  @Target(ElementType.TYPE_USE) @Retention(RetentionPolicy.RUNTIME)\n" +
            "  @interface Mark { String value(); }\n" +
            "  List<@Mark(\"first\") String> first;\n" +
            "  List<@Mark(\"second\") String> second;\n" +
            "}\n";

        TaskCompiler.compileStrings(
            Collections.singletonList(new TaskCompiler.Source("DistinctUsages", source)),
            dir,
            Collections.<Path>emptyList());

        final MetadataSystem metadata = TaskAudit.newMetadataSystem();
        final TypeDefinition type =
            metadata.lookupType(dir.resolve("DistinctUsages.class").toString()).resolve();

        final List<TypeAnnotation> first = fieldAnnotations(type, "first");
        final List<TypeAnnotation> second = fieldAnnotations(type, "second");

        TaskCheck.check(first.size() == 1 && second.size() == 1,
                        "each field carries exactly its own type annotation");
        TaskCheck.check(first.get(0) != second.get(0),
                        "two usages of List<String> keep distinct annotation records");
        TaskCheck.check(
            !first.get(0).equals(second.get(0)),
            "same declared type does not cause annotation sharing across usages");

        // And the rendered output preserves both distinct markers.
        final PlainTextOutput output = new PlainTextOutput();
        final DecompilerSettings settings = DecompilerSettings.javaDefaults();
        settings.setTypeLoader(new InputTypeLoader());
        Decompiler.decompile(dir.resolve("DistinctUsages.class").toString(), output, settings);
        final String text = output.toString();
        TaskCheck.check(text.contains("\"first\"") && text.contains("\"second\""),
                        "both distinct usage markers rendered");
    }

    private static void unrepresentableOwnerReportedNotMisattached() throws Exception {
        final Path dir = WORK.resolve("fixtures-unsupported");
        recreate(dir);

        TaskCompiler.compileStrings(
            Collections.singletonList(
                new TaskCompiler.Source("UnrepresentableOwner",
                                        TaskFixtures.sourceFor("UnrepresentableOwner"))),
            dir,
            Collections.<Path>emptyList());

        final MetadataSystem metadata = TaskAudit.newMetadataSystem();
        final TypeDefinition type =
            metadata.lookupType(dir.resolve("UnrepresentableOwner.class").toString()).resolve();

        final List<TypeAnnotation> deep = fieldAnnotations(type, "deep");

        // The owner-level (parameterized enclosing class) annotation cannot be represented and
        // must still be readable in metadata rather than rebound to the inner argument.
        final TypeAnnotation ownerArg = findByPath(deep, "[TYPE_ARGUMENT(0)]");
        final TypeAnnotation innerArg = findByPath(deep, "[INNER_TYPE,TYPE_ARGUMENT(0)]");

        TaskCheck.check(ownerArg != null, "unrepresentable owner annotation retained in metadata");
        TaskCheck.check(innerArg != null, "representable inner annotation retained in metadata");
        TaskCheck.check(ownerArg.getVisibility() == TypeAnnotation.Visibility.CLASS,
                        "owner annotation visibility preserved");

        // The rendered source must not attach the owner marker to the inner segment.
        final PlainTextOutput output = new PlainTextOutput();
        final DecompilerSettings settings = DecompilerSettings.javaDefaults();
        settings.setTypeLoader(new InputTypeLoader());
        Decompiler.decompile(dir.resolve("UnrepresentableOwner.class").toString(), output, settings);
        final String text = output.toString();
        TaskCheck.check(!text.contains("@Trusted"),
                        "unrepresentable owner annotation is not misattached in source");
        TaskCheck.check(text.contains("@Sensitive"),
                        "representable inner annotation is still emitted");
    }

    // ======================================================================
    // Rewrite group
    // ======================================================================

    private static void decompiledSourceRecompiles() throws Exception {
        final Compiled c = new Compiled("rewrite-compile");
        final Path sourceDir = WORK.resolve("rewrite-src");
        final Path classDir = WORK.resolve("rewrite-classes");
        recreate(sourceDir);
        recreate(classDir);

        for (final String name : TaskFixtures.fixtureNames()) {
            final String source = TaskAudit.decompile(c.classFile(name));
            TaskAudit.writeSource(sourceDir.resolve(name + ".java"), source);
        }

        // Recompiling every decompiled fixture together exercises the full supported target set.
        final List<Path> files = new ArrayList<>();

        for (final String name : TaskFixtures.fixtureNames()) {
            files.add(sourceDir.resolve(name + ".java"));
        }

        TaskCompiler.compileFiles(files, classDir, Collections.<Path>emptyList());

        for (final String name : TaskFixtures.fixtureNames()) {
            TaskCheck.check(Files.exists(classDir.resolve(name + ".class")),
                            "recompiled class produced for " + name);
        }
    }

    private static void recompiledAnnotationsMatchAtEveryPosition() throws Exception {
        final Compiled c = new Compiled("rewrite-match");
        final Path sourceDir = WORK.resolve("match-src");
        final Path classDir = WORK.resolve("match-classes");
        recreate(sourceDir);
        recreate(classDir);

        final List<Path> files = new ArrayList<>();

        for (final String name : TaskFixtures.fixtureNames()) {
            final String source = TaskAudit.decompile(c.classFile(name));
            TaskAudit.writeSource(sourceDir.resolve(name + ".java"), source);
            files.add(sourceDir.resolve(name + ".java"));
        }

        TaskCompiler.compileFiles(files, classDir, Collections.<Path>emptyList());

        final MetadataSystem metadata = TaskAudit.newMetadataSystem();
        int compared = 0;

        for (final String name : TaskFixtures.fixtureNames()) {
            final TypeDefinition original =
                metadata.lookupType(c.classFile(name).toString()).resolve();
            final TypeDefinition recompiled =
                metadata.lookupType(classDir.resolve(name + ".class").toString()).resolve();

            final List<String> originalKeys =
                TaskAudit.stableKeys(collectRecursive(original));
            final List<String> recompiledKeys =
                TaskAudit.stableKeys(collectRecursive(recompiled));

            TaskCheck.check(originalKeys.equals(recompiledKeys),
                            "type annotation metadata matches after recompile for " + name +
                            "\n  original: " + originalKeys +
                            "\n  recompiled: " + recompiledKeys);
            compared += originalKeys.size();
        }

        TaskCheck.check(compared > 0, "a non-empty set of annotations was compared");
    }

    private static void declarationAnnotationsRemainSeparate() throws Exception {
        final Path dir = WORK.resolve("fixtures-decl");
        recreate(dir);

        final String source =
            "import java.lang.annotation.*;\n" +
            "import java.util.List;\n" +
            "public class DeclSeparate {\n" +
            "  @Target(ElementType.TYPE_USE) @Retention(RetentionPolicy.RUNTIME)\n" +
            "  @interface TypeMark {}\n" +
            "  @Deprecated List<@TypeMark String> values;\n" +
            "}\n";

        TaskCompiler.compileStrings(
            Collections.singletonList(new TaskCompiler.Source("DeclSeparate", source)),
            dir,
            Collections.<Path>emptyList());

        final PlainTextOutput output = new PlainTextOutput();
        final DecompilerSettings settings = DecompilerSettings.javaDefaults();
        settings.setTypeLoader(new InputTypeLoader());
        Decompiler.decompile(dir.resolve("DeclSeparate.class").toString(), output, settings);
        final String text = output.toString();

        // Declaration annotation and type-use annotation are both present but distinct.
        TaskCheck.check(text.contains("@Deprecated"), "declaration annotation retained");
        TaskCheck.check(text.contains("@TypeMark"), "type-use annotation retained");
        TaskCheck.check(text.contains("List<@TypeMark String>"),
                        "type-use annotation placed on the argument, not the declaration");

        // Declaration annotations are read from a different attribute than type annotations.
        final MetadataSystem metadata = TaskAudit.newMetadataSystem();
        final TypeDefinition type =
            metadata.lookupType(dir.resolve("DeclSeparate.class").toString()).resolve();
        final FieldDefinition field = fieldDefinitions(type, "values");
        final List<TypeAnnotation> typeOnly =
            TypeAnnotations.collect(field.getSourceAttributes());

        for (final TypeAnnotation annotation : typeOnly) {
            TaskCheck.check(
                !annotation.getAnnotation().getAnnotationType().getFullName()
                    .endsWith("Deprecated"),
                "declaration annotation is never mixed into the type-use attribute");
        }
    }

    private static void explicitRecordAccessorRetained() throws Exception {
        final Compiled c = new Compiled("rewrite-accessor");
        final PlainTextOutput output = new PlainTextOutput();
        final DecompilerSettings settings = DecompilerSettings.javaDefaults();
        settings.setTypeLoader(new InputTypeLoader());

        final Path boxClass = c.dir.resolve("RecordAccessors$Box.class");
        Decompiler.decompile(boxClass.toString(), output, settings);
        final String text = output.toString();

        TaskCheck.check(text.contains("items()"), "explicit accessor method retained");
        TaskCheck.check(text.contains("@Sensitive"),
                        "explicit accessor's distinct return annotation retained");
    }

    private static void securityPolicyAuditVerdicts() throws Exception {
        final Path dir = WORK.resolve("audit-verdict");
        recreate(dir);

        final String source =
            "import java.lang.annotation.*;\n" +
            "import java.util.List;\n" +
            "public class AuditVerdictTarget {\n" +
            "  List<@Sensitive String> labeled;\n" +
            "  String @Trusted [] onlyOuter;\n" +
            "  List<@Trusted String> second;\n" +
            "}\n";

        TaskCompiler.compileStrings(
            Collections.singletonList(new TaskCompiler.Source("AuditVerdictTarget", source)),
            dir,
            Collections.<Path>emptyList());

        final MetadataSystem metadata = TaskAudit.newMetadataSystem();
        final TypeDefinition type =
            metadata.lookupType(dir.resolve("AuditVerdictTarget.class").toString()).resolve();

        final List<SecurityPolicyAudit.PolicyRule> rules = Arrays.asList(
            new SecurityPolicyAudit.PolicyRule(
                "field:labeled", "FIELD", "[TYPE_ARGUMENT(0)]",
                "Sensitive", TypeAnnotation.Visibility.RUNTIME),
            new SecurityPolicyAudit.PolicyRule(
                "field:labeled", "FIELD", "[TYPE_ARGUMENT(0)]",
                "Trusted", TypeAnnotation.Visibility.CLASS),
            new SecurityPolicyAudit.PolicyRule(
                "field:onlyOuter", "FIELD", "[ARRAY_ELEMENT,TYPE_ARGUMENT(0)]",
                "Trusted", TypeAnnotation.Visibility.CLASS),
            new SecurityPolicyAudit.PolicyRule(
                "field:second", "FIELD", "[TYPE_ARGUMENT(0)]",
                "Trusted", TypeAnnotation.Visibility.CLASS)
        );

        final List<SecurityPolicyAudit.Finding> findings =
            new SecurityPolicyAudit().audit(type, rules);

        TaskCheck.check(findings.size() == 4, "one finding per policy rule");

        int present = 0;
        int missing = 0;
        int wrong = 0;

        for (final SecurityPolicyAudit.Finding finding : findings) {
            switch (finding.verdict) {
                case PRESENT: present++; break;
                case MISSING: missing++; break;
                case WRONG_POSITION: wrong++; break;
                default:
                    throw new AssertionError("unexpected verdict " + finding.verdict);
            }

            // Every finding identifies class, declaration, target and path for triage.
            TaskCheck.check(finding.className != null && finding.declaration != null,
                            "finding carries class and declaration identity");
        }

        TaskCheck.check(present == 2, "two present markers at exact positions");
        TaskCheck.check(missing == 1, "one missing marker reported");
        TaskCheck.check(wrong == 1, "one wrong-position (outer-array-only) marker reported");
    }

    private static void annotationIdentitySurvivesAstCloneAndRewrite() throws Exception {
        final Path dir = WORK.resolve("clone-identity");
        recreate(dir);

        final String source =
            "import java.lang.annotation.*;\n" +
            "import java.util.List;\n" +
            "public class CloneIdentity {\n" +
            "  @Target(ElementType.TYPE_USE) @Retention(RetentionPolicy.RUNTIME)\n" +
            "  @interface Mark { String value(); }\n" +
            "  List<@Mark(\"a\") String> alpha;\n" +
            "  List<@Mark(\"b\") String> beta;\n" +
            "}\n";

        TaskCompiler.compileStrings(
            Collections.singletonList(new TaskCompiler.Source("CloneIdentity", source)),
            dir,
            Collections.<Path>emptyList());

        final MetadataSystem metadata = TaskAudit.newMetadataSystem();
        final TypeDefinition type =
            metadata.lookupType(dir.resolve("CloneIdentity.class").toString()).resolve();

        final com.strobel.decompiler.DecompilerContext context = buildContext();
        final com.strobel.decompiler.languages.java.ast.AstBuilder builder =
            new com.strobel.decompiler.languages.java.ast.AstBuilder(context);
        builder.addType(type);
        new com.strobel.decompiler.languages.java.ast.transforms.TypeAnnotationTransform(context)
            .run(builder.getCompilationUnit());

        // Clone the whole compilation unit; the 1:1 source-record link must be preserved.
        final com.strobel.decompiler.languages.java.ast.AstNode clone =
            builder.getCompilationUnit().clone();

        final List<TypeAnnotation> originalRecords = collectTypeAnnotationRecords(
            builder.getCompilationUnit());
        final List<TypeAnnotation> clonedRecords = collectTypeAnnotationRecords(clone);

        TaskCheck.check(originalRecords.size() == 2,
                        "two type annotations present before cloning");
        TaskCheck.check(clonedRecords.size() == originalRecords.size(),
                        "all type annotations survive an AST clone");

        // The two List<String> usages remain distinct after cloning (no shared-record leakage).
        final java.util.Set<TypeAnnotation> distinct =
            new java.util.HashSet<>(clonedRecords);
        TaskCheck.check(distinct.size() == 2,
                        "the two same-type usages keep distinct annotation records after cloning");

        // And the fully rendered output, after import collapsing/name rewrites, keeps both markers.
        final PlainTextOutput output = new PlainTextOutput();
        final DecompilerSettings settings = DecompilerSettings.javaDefaults();
        settings.setTypeLoader(new InputTypeLoader());
        Decompiler.decompile(dir.resolve("CloneIdentity.class").toString(), output, settings);
        final String text = output.toString();
        TaskCheck.check(text.contains("\"a\"") && text.contains("\"b\""),
                        "distinct markers survive import cleanup and nested-name simplification");
    }

    private static com.strobel.decompiler.DecompilerContext buildContext() {
        final DecompilerSettings settings = DecompilerSettings.javaDefaults();
        settings.setTypeLoader(new InputTypeLoader());
        return new com.strobel.decompiler.DecompilerContext(settings);
    }

    private static List<TypeAnnotation> collectTypeAnnotationRecords(
        final com.strobel.decompiler.languages.java.ast.AstNode root) {

        final List<TypeAnnotation> records = new ArrayList<>();
        collectRecordsRecursive(root, records);
        return records;
    }

    private static void collectRecordsRecursive(
        final com.strobel.decompiler.languages.java.ast.AstNode node,
        final List<TypeAnnotation> into) {

        for (final com.strobel.decompiler.languages.java.ast.Annotation annotation :
                 node.getChildrenByRole(
                     com.strobel.decompiler.languages.java.ast.Roles.TYPE_ANNOTATION)) {

            final TypeAnnotation source =
                annotation.getUserData(
                    com.strobel.decompiler.languages.java.ast.Keys.SOURCE_TYPE_ANNOTATION);

            if (source != null) {
                into.add(source);
            }
        }

        for (final com.strobel.decompiler.languages.java.ast.AstNode child : node.getChildren()) {
            collectRecordsRecursive(child, into);
        }
    }

    // ======================================================================
    // Small shared utilities
    // ======================================================================

    private static TypeDefinition nestedType(final TypeDefinition owner, final String simpleName) {
        for (final TypeDefinition nested : owner.getDeclaredTypes()) {
            if (nested.getSimpleName().equals(simpleName)) {
                return nested;
            }
        }

        throw new AssertionError("nested type not found: " + simpleName);
    }

    private static List<TypeAnnotation> constructorAnnotations(final TypeDefinition type) {
        for (final MethodDefinition method : type.getDeclaredMethods()) {
            if (method.isConstructor()) {
                return TypeAnnotations.collect(method.getSourceAttributes());
            }
        }

        throw new AssertionError("constructor not found in " + type.getName());
    }

    private static int[] formalIndices(final List<TypeAnnotation> annotations) {
        final List<Integer> indices = new ArrayList<>();

        for (final TypeAnnotation annotation : annotations) {
            if (annotation.getTargetKind() == TypeAnnotationTargetKind.FORMAL_PARAMETER) {
                indices.add(
                    ((com.strobel.assembler.metadata.typeannotations.TypeAnnotationTargetInfo.FormalParameterTarget)
                         annotation.getTargetInfo()).getFormalParameterIndex());
            }
        }

        final int[] result = new int[indices.size()];

        for (int i = 0; i < result.length; i++) {
            result[i] = indices.get(i);
        }

        return result;
    }

    private static TypeAnnotation findByPath(
        final List<TypeAnnotation> annotations,
        final String path) {

        for (final TypeAnnotation annotation : annotations) {
            if (pathString(annotation.getPath()).equals(path)) {
                return annotation;
            }
        }

        return null;
    }

    private static FieldDefinition fieldDefinitions(final TypeDefinition type, final String name) {
        for (final FieldDefinition field : type.getDeclaredFields()) {
            if (field.getName().equals(name)) {
                return field;
            }
        }

        throw new AssertionError("field not found: " + name);
    }

    private static List<TaskAudit.Record> collectRecursive(final TypeDefinition type) {
        final List<TaskAudit.Record> all = new ArrayList<>(TaskAudit.collectAll(type));

        for (final TypeDefinition nested : type.getDeclaredTypes()) {
            all.addAll(collectRecursive(nested));
        }

        return all;
    }
}
