import java.util.*;
public final class TaskCheck {
    @FunctionalInterface public interface Body { void run() throws Exception; }
    public static final class Case {
        final String group, name; final Body body;
        public Case(String group, String name, Body body) {
            this.group = Objects.requireNonNull(group); this.name = Objects.requireNonNull(name);
            this.body = Objects.requireNonNull(body);
        }
    }
    private static long assertions;
    public static void check(boolean condition, String message) {
        assertions++;
        if (!condition) throw new AssertionError(message);
    }
    public static void skip(String message) { throw new AssertionError("SKIP is not a completed case: " + message); }
    public static void main(String[] args) throws Exception {
        Object value = Class.forName("TaskTypeAnnotations").getMethod("cases").invoke(null);
        if (!(value instanceof List) || ((List<?>) value).isEmpty()) throw new AssertionError("zero cases");
        List<?> list = (List<?>) value;
        Set<String> names = new HashSet<>();
        int total = 0;
        for (String group : Arrays.asList("Metadata", "Paths", "Rewrite")) {
            int count = 0; long start = assertions;
            for (Object entry : list) {
                if (!(entry instanceof Case)) throw new AssertionError("invalid case");
                Case test = (Case) entry;
                if (!Arrays.asList("Metadata", "Paths", "Rewrite").contains(test.group)) throw new AssertionError("unknown group");
                if (!group.equals(test.group)) continue;
                if (test.name.trim().isEmpty() || !names.add(group + ":" + test.name)) throw new AssertionError("duplicate/empty name");
                long before = assertions;
                test.body.run();
                if (assertions <= before) throw new AssertionError("no assertions: " + test.name);
                System.out.println("TASK_CASE_DONE " + group + " " + test.name + " checks=" + (assertions-before));
                count++;
            }
            if (count == 0 || assertions <= start) throw new AssertionError("empty group: " + group);
            total += count;
            System.out.println("TASK_GROUP_DONE " + group + " cases=" + count + " checks=" + (assertions-start));
        }
        if (total != list.size()) throw new AssertionError("unexecuted case");
        System.out.println("TASK_TARGET_DONE cases=" + total + " checks=" + assertions);
    }
}
