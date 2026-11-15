public final class TaskChecks {
    private long checks;
    private boolean failed;
    public void check(boolean value, String message) {
        ++checks;
        if (!value) { failed=true; throw new AssertionError(message); }
    }
    public interface Case { void run(TaskChecks c) throws Exception; }
    public static void group(String name, Case body) throws Exception {
        TaskChecks c = new TaskChecks();
        body.run(c);
        if(c.failed || c.checks == 0) throw new AssertionError("no successful assertions: "+name);
        System.out.println("GROUP "+name+" checks="+c.checks+" COMPLETE");
    }
}
