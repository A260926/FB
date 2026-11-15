import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * Hand-written, benign class sources compiled by the test harness with
 * {@code javac --release 17 -parameters -proc:none}.  Each public nested class is one fixture;
 * the {@code expectations} lists below enumerate, by hand, the exact JVMS 4.7.20 target/path
 * tuples that javac must produce for that fixture.  The tests verify the production metadata
 * and the recompiled class against these tuples rather than against decompiled text.
 */
public final class TaskFixtures {
    private TaskFixtures() {
    }

    // ----------------------------------------------------------------------------------
    // Nested generic arrays: array dimensions, member segments and generic arguments.
    // ----------------------------------------------------------------------------------
    public static final String NESTED_GENERIC_ARRAYS =
        "import java.lang.annotation.*;\n" +
        "import java.util.List;\n" +
        "import java.util.Map;\n" +
        "public class NestedGenericArrays {\n" +
        "  public static class Outer<A> {\n" +
        "    public static class Inner<B> {}\n" +
        "  }\n" +
        "  public Map.@Sensitive Entry<String, @Trusted Integer>[] matrix;\n" +
        "  public List<@Sensitive String> @Trusted [] @Sensitive [] nested;\n" +
        "  public Outer.Inner<@Sensitive Integer> deep;\n" +
        "}\n";

    //
    // A parameterized non-static owner whose type arguments Procyon does not preserve in the
    // reconstructed type tree.  The owner-level type annotation must be reported as an
    // unsupported (unanalyzed) record rather than attached to the wrong segment; this fixture is
    // used for the unsupported-target test, not the round-trip equivalence test.
    //
    public static final String UNREPRESENTABLE_OWNER =
        "import java.lang.annotation.*;\n" +
        "public class UnrepresentableOwner {\n" +
        "  public static class Outer<A> {\n" +
        "    public class Inner<B> {}\n" +
        "  }\n" +
        "  public Outer<@Trusted String>.Inner<@Sensitive Integer> deep;\n" +
        "}\n";

    // ----------------------------------------------------------------------------------
    // Inner and outer member type segments, including a non-generic enclosing class.
    // ----------------------------------------------------------------------------------
    public static final String INNER_OUTER_MEMBERS =
        "import java.lang.annotation.*;\n" +
        "public class InnerOuterMembers {\n" +
        "  public static class Plain {\n" +
        "    public static class Member<T> {}\n" +
        "  }\n" +
        "  public Plain.@Sensitive Member<@Trusted String> a;\n" +
        "  public Plain.Member<@Sensitive Integer> b;\n" +
        "}\n";

    // ----------------------------------------------------------------------------------
    // Interface bound index and wildcard placements on a type parameter and a method.
    // ----------------------------------------------------------------------------------
    public static final String INTERFACE_BOUND_INDEX =
        "import java.lang.annotation.*;\n" +
        "import java.util.List;\n" +
        "public class InterfaceBoundIndex<T extends @Sensitive Number & @Trusted Comparable<T>> {\n" +
        "  public <X extends Exception> List<@Sensitive ?> wildcard(\n" +
        "      List<? extends @Trusted Number> bound) {\n" +
        "    return null;\n" +
        "  }\n" +
        "}\n";

    // ----------------------------------------------------------------------------------
    // Hidden parameters: a non-static inner constructor gains a mandated enclosing instance,
    // and an enum constructor gains synthetic name/ordinal parameters.  Formal indices must
    // still resolve to the source-visible parameters.
    // ----------------------------------------------------------------------------------
    public static final String HIDDEN_PARAMETERS =
        "import java.lang.annotation.*;\n" +
        "public class HiddenParameters {\n" +
        "  public class Inner {\n" +
        "    Inner(@Sensitive int visible, @Trusted String other) {}\n" +
        "  }\n" +
        "  public enum Color {\n" +
        "    RED(\"r\", 1), GREEN(\"g\", 2);\n" +
        "    Color(@Sensitive String name, @Trusted int code) {}\n" +
        "  }\n" +
        "}\n";

    // ----------------------------------------------------------------------------------
    // Record components: a plain generated accessor must collapse, while an explicit accessor
    // carrying different return-type annotations must be retained.
    // ----------------------------------------------------------------------------------
    public static final String RECORD_ACCESSORS =
        "import java.lang.annotation.*;\n" +
        "import java.util.List;\n" +
        "public class RecordAccessors {\n" +
        "  public record Box(List<@Sensitive String> @Trusted [] items, int plain) {\n" +
        "    @Override\n" +
        "    public @Sensitive List<String> @Sensitive [] items() {\n" +
        "      return items;\n" +
        "    }\n" +
        "  }\n" +
        "}\n";

    // ----------------------------------------------------------------------------------
    // Annotated varargs: the last array dimension becomes the ellipsis and keeps the
    // annotation that belongs to that dimension; earlier dimensions stay brackets.
    // ----------------------------------------------------------------------------------
    public static final String ANNOTATED_VARARGS =
        "import java.lang.annotation.*;\n" +
        "public class AnnotatedVarargs {\n" +
        "  public void varargs(@Trusted int @Sensitive [] @Trusted ... nums) {}\n" +
        "}\n";

    /** Fixture entries in a stable order. */
    public static List<String> fixtureNames() {
        return Arrays.asList(
            "NestedGenericArrays",
            "InnerOuterMembers",
            "InterfaceBoundIndex",
            "HiddenParameters",
            "RecordAccessors",
            "AnnotatedVarargs"
        );
    }

    /** Fixtures that are not part of the round-trip equivalence set. */
    public static List<String> unsupportedFixtureNames() {
        return Collections.singletonList("UnrepresentableOwner");
    }

    public static String sourceFor(final String name) {
        switch (name) {
            case "NestedGenericArrays": return NESTED_GENERIC_ARRAYS;
            case "InnerOuterMembers": return INNER_OUTER_MEMBERS;
            case "InterfaceBoundIndex": return INTERFACE_BOUND_INDEX;
            case "HiddenParameters": return HIDDEN_PARAMETERS;
            case "RecordAccessors": return RECORD_ACCESSORS;
            case "AnnotatedVarargs": return ANNOTATED_VARARGS;
            case "UnrepresentableOwner": return UNREPRESENTABLE_OWNER;
            default: throw new IllegalArgumentException("Unknown fixture: " + name);
        }
    }
}
