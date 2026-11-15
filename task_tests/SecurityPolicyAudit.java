import com.strobel.assembler.InputTypeLoader;
import com.strobel.assembler.ir.attributes.AttributeNames;
import com.strobel.assembler.ir.attributes.RecordAttribute;
import com.strobel.assembler.ir.attributes.RecordComponentInfo;
import com.strobel.assembler.ir.attributes.SourceAttribute;
import com.strobel.assembler.metadata.FieldDefinition;
import com.strobel.assembler.metadata.MetadataSystem;
import com.strobel.assembler.metadata.MethodDefinition;
import com.strobel.assembler.metadata.TypeDefinition;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotation;
import com.strobel.assembler.metadata.typeannotations.TypeAnnotations;

import java.util.ArrayList;
import java.util.List;

/**
 * Offline integrity-audit example built entirely on the production type-annotation metadata.
 *
 * <p>It models a release-package policy that requires custom security markers
 * ({@code Sensitive} = RUNTIME-visible, {@code Trusted} = CLASS-visible) at precise type
 * positions.  For every policy rule it reports one of:
 * <ul>
 *   <li><b>present</b> — a matching marker was found at the exact target and type_path;</li>
 *   <li><b>missing</b> — no marker exists at the required position;</li>
 *   <li><b>wrong position</b> — a marker of the required kind exists elsewhere but not at the
 *       required exact type_path;</li>
 *   <li><b>unanalyzable</b> — the target/path cannot be represented in source (for example an
 *       outer member-segment argument the type tree drops), so it must be reviewed manually.</li>
 * </ul>
 * The audit reads class metadata only; it never loads, initializes or executes the input class,
 * and it never searches decompiled text or annotation simple names.</p>
 *
 * <p>This class is an executable example (it has a {@code main} method); it is also exercised by
 * the Rewrite test group through its public methods.</p>
 */
public final class SecurityPolicyAudit {

    /** One required marker at a precise type position. */
    public static final class PolicyRule {
        public final String declaration;
        public final String target;
        public final String typePath;
        public final String markerSimpleName;
        public final TypeAnnotation.Visibility visibility;

        public PolicyRule(
            final String declaration,
            final String target,
            final String typePath,
            final String markerSimpleName,
            final TypeAnnotation.Visibility visibility) {

            this.declaration = declaration;
            this.target = target;
            this.typePath = typePath;
            this.markerSimpleName = markerSimpleName;
            this.visibility = visibility;
        }

        @Override
        public String toString() {
            return declaration + " target=" + target + " path=" + typePath +
                   " requires " + visibility + " @" + markerSimpleName;
        }
    }

    /** The audit verdict for one policy rule. */
    public enum Verdict { PRESENT, MISSING, WRONG_POSITION, UNANALYZABLE }

    /** One audit finding, including the class, declaration signature, target and path. */
    public static final class Finding {
        public final String className;
        public final String declaration;
        public final String target;
        public final String typePath;
        public final Verdict verdict;
        public final String detail;

        Finding(
            final String className,
            final String declaration,
            final String target,
            final String typePath,
            final Verdict verdict,
            final String detail) {

            this.className = className;
            this.declaration = declaration;
            this.target = declaration != null ? target : null;
            this.typePath = typePath;
            this.verdict = verdict;
            this.detail = detail;
        }

        @Override
        public String toString() {
            return verdict + " [" + className + "] " + declaration +
                   " target=" + target + " path=" + typePath + " — " + detail;
        }
    }

    /** Paths the decompiler cannot represent in source; they must be reviewed manually. */
    private static boolean isUnanalyzablePath(final String typePath) {
        // An owner-segment type argument (a TYPE_ARGUMENT with no enclosing INNER_TYPE at the
        // root) targets a parameterized enclosing class whose arguments the type tree erases.
        return typePath.startsWith("[TYPE_ARGUMENT");
    }

    private static String simpleName(final String fullName) {
        final int dot = fullName.lastIndexOf('.');
        final int dollar = fullName.lastIndexOf('$');
        final int cut = Math.max(dot, dollar);
        return cut >= 0 ? fullName.substring(cut + 1) : fullName;
    }

    private static String pathOf(final TypeAnnotation annotation) {
        return TaskAudit.Record.renderPathPublic(annotation.getPath());
    }

    private static boolean markerMatches(final TypeAnnotation annotation, final PolicyRule rule) {
        return annotation.getVisibility() == rule.visibility &&
               simpleName(annotation.getAnnotation().getAnnotationType().getFullName())
                   .equals(rule.markerSimpleName);
    }

    /**
     * Audits one type definition against the policy, descending into nested types.
     */
    public List<Finding> audit(final TypeDefinition type, final List<PolicyRule> rules) {
        final List<Finding> findings = new ArrayList<>();
        auditType(type, rules, findings);
        return findings;
    }

    private void auditType(
        final TypeDefinition type,
        final List<PolicyRule> rules,
        final List<Finding> findings) {

        for (final PolicyRule rule : rules) {
            final List<TypeAnnotation> candidates = candidatesFor(type, rule.declaration);

            if (candidates == null) {
                continue;
            }

            findings.add(evaluate(type.getFullName(), rule, candidates));
        }

        for (final TypeDefinition nested : type.getDeclaredTypes()) {
            auditType(nested, rules, findings);
        }
    }

    private Finding evaluate(
        final String className,
        final PolicyRule rule,
        final List<TypeAnnotation> candidates) {

        boolean sameMarkerElsewhere = false;
        boolean unanalyzable = false;

        for (final TypeAnnotation annotation : candidates) {
            if (!markerMatches(annotation, rule)) {
                continue;
            }

            final String actualPath = pathOf(annotation);

            if (actualPath.equals(rule.typePath) &&
                annotation.getTargetInfo().toString().contains(rule.target)) {
                return new Finding(
                    className, rule.declaration, rule.target, rule.typePath,
                    Verdict.PRESENT, "required marker found at the exact type position");
            }

            if (isUnanalyzablePath(actualPath)) {
                unanalyzable = true;
            }

            sameMarkerElsewhere = true;
        }

        if (unanalyzable) {
            return new Finding(
                className, rule.declaration, rule.target, rule.typePath,
                Verdict.UNANALYZABLE,
                "marker targets a type segment not preserved in the reconstructed type tree; " +
                "manual review required");
        }

        if (sameMarkerElsewhere) {
            return new Finding(
                className, rule.declaration, rule.target, rule.typePath,
                Verdict.WRONG_POSITION,
                "a matching marker exists but at a different type_path");
        }

        return new Finding(
            className, rule.declaration, rule.target, rule.typePath,
            Verdict.MISSING, "no matching marker at the required position");
    }

    /**
     * Resolves the type-use annotations on the declaration named by a policy rule.
     * Supported declaration names: {@code type}, {@code field:<name>},
     * {@code method:<name>}, {@code record-component:<name>}.
     */
    private List<TypeAnnotation> candidatesFor(final TypeDefinition type, final String declaration) {
        if ("type".equals(declaration)) {
            return TypeAnnotations.collect(type.getSourceAttributes());
        }

        if (declaration.startsWith("field:")) {
            final String name = declaration.substring("field:".length());

            for (final FieldDefinition field : type.getDeclaredFields()) {
                if (field.getName().equals(name)) {
                    return TypeAnnotations.collect(field.getSourceAttributes());
                }
            }

            return null;
        }

        if (declaration.startsWith("method:")) {
            final String name = declaration.substring("method:".length());

            for (final MethodDefinition method : type.getDeclaredMethods()) {
                if (method.getName().equals(name)) {
                    return TypeAnnotations.collect(method.getSourceAttributes());
                }
            }

            return null;
        }

        if (declaration.startsWith("record-component:")) {
            final String name = declaration.substring("record-component:".length());
            final RecordAttribute record = SourceAttribute.find(
                AttributeNames.Record, type.getSourceAttributes());

            if (record == null) {
                return null;
            }

            for (final RecordComponentInfo component : record.getComponents()) {
                if (component.getName().equals(name)) {
                    return TypeAnnotations.collect(component.getAttributes());
                }
            }

            return null;
        }

        return null;
    }

    /**
     * Executable demonstration.  Builds a small class with the marker annotations and runs the
     * policy audit, printing every verdict.  The report states only whether the custom markers
     * satisfy the agreed policy; it never claims the annotations themselves enforce anything.
     */
    public static void main(final String[] args) throws Exception {
        final java.nio.file.Path workRoot = Paths.getWorkDir();
        final java.nio.file.Path dir = workRoot.resolve("audit-example");
        Files2.ensureDirectory(dir);

        final String source =
            "import java.lang.annotation.*;\n" +
            "import java.util.List;\n" +
            "public class AuditTarget {\n" +
            "  List<@Sensitive String> labeled;\n" +
            "  String @Trusted [] onlyOuter;\n" +
            "  List<@Trusted String> second;\n" +
            "}\n";

        TaskCompiler.compileStrings(
            java.util.Collections.singletonList(
                new TaskCompiler.Source("AuditTarget", source)),
            dir,
            java.util.Collections.<java.nio.file.Path>emptyList());

        final MetadataSystem metadata = new MetadataSystem(new InputTypeLoader());
        final TypeDefinition type =
            metadata.lookupType(dir.resolve("AuditTarget.class").toString()).resolve();

        final List<PolicyRule> rules = java.util.Arrays.asList(
            // Satisfied: Sensitive on the List type argument of 'labeled'.
            new PolicyRule("field:labeled", "FIELD", "[TYPE_ARGUMENT(0)]",
                           "Sensitive", TypeAnnotation.Visibility.RUNTIME),

            // Missing: no Trusted marker on the List argument of 'labeled'.
            new PolicyRule("field:labeled", "FIELD", "[TYPE_ARGUMENT(0)]",
                           "Trusted", TypeAnnotation.Visibility.CLASS),

            // Wrong position: Trusted exists on 'onlyOuter' but at the array root ([]),
            // not on an element type argument.
            new PolicyRule("field:onlyOuter", "FIELD", "[ARRAY_ELEMENT,TYPE_ARGUMENT(0)]",
                           "Trusted", TypeAnnotation.Visibility.CLASS),

            // Same declared type (List<String>) as 'labeled' but a different marker; the audit
            // distinguishes the two usages by declaration rather than by annotation name.
            new PolicyRule("field:second", "FIELD", "[TYPE_ARGUMENT(0)]",
                           "Trusted", TypeAnnotation.Visibility.CLASS)
        );

        final SecurityPolicyAudit auditor = new SecurityPolicyAudit();
        final List<Finding> findings = auditor.audit(type, rules);

        System.out.println("Custom data-security marker policy audit");
        System.out.println("(Reports marker/policy conformance only; annotations do not enforce policy.)");

        for (final Finding finding : findings) {
            System.out.println(" - " + finding);
        }
    }
}
