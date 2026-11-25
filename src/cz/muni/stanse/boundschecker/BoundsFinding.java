package cz.muni.stanse.boundschecker;

import cz.muni.stanse.codestructures.CFGNode;
import cz.muni.stanse.utils.Pair;
import cz.muni.stanse.codestructures.CFGHandle;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

/**
 * One stable {@code bounds-proof} result for a single syntactic
 * {@code a[i]} / {@code p[i]} access.
 *
 * <p>The status is {@link #PROVED} only when every reachable path proves
 * {@code 0 <= i < length} for every object the access may denote.  Otherwise
 * the status is {@link #UNPROVED}; this only means the proof was not completed
 * and must not be read as a claim that the code is vulnerable.</p>
 *
 * <p>Instances expose the enclosing function, source location, the possible
 * underlying objects with their individual status and, for unproved results,
 * one real condition/rewrite witness trace.</p>
 */
public final class BoundsFinding implements Comparable<BoundsFinding> {

    public static final String PROVED = "proved";
    public static final String UNPROVED = "unproved";

    /** One source location event inside a witness trace. */
    public static final class WitnessStep {
        private final int line;
        private final int column;
        private final String kind;
        private final String description;
        private final CFGNode node;

        public WitnessStep(final int line, final int column,
                           final String kind, final String description,
                           final CFGNode node) {
            this.line = line;
            this.column = column;
            this.kind = kind == null ? "step" : kind;
            this.description = description == null ? "" : description;
            this.node = node;
        }

        public int getLine() { return line; }
        /** 1-based source column. */
        public int getColumn() { return column; }
        public String getKind() { return kind; }
        public String getDescription() { return description; }
        public CFGNode getNode() { return node; }

        public String getLocation() {
            return line + ":" + column;
        }

        @Override
        public String toString() {
            return getLocation() + " " + kind + " " + description;
        }
    }

    private final String status;
    private final String checkerName;
    private final String kind;
    private final String functionName;
    private final File file;
    private final int line;
    private final int column;
    private final int rawColumn;
    private final String accessText;
    private final CFGNode accessNode;
    private final Set<Pair<CFGHandle, String>> origins;
    private final Map<String, String> objectStatuses;
    private final Map<String, Integer> objectLengths;
    private final List<WitnessStep> witness;
    private final List<CFGNode> traceNodes;
    private final String message;

    BoundsFinding(final String status,
                  final String checkerName,
                  final String kind,
                  final String functionName,
                  final File file,
                  final int line,
                  final int column,
                  final int rawColumn,
                  final String accessText,
                  final CFGNode accessNode,
                  final Set<Pair<CFGHandle, String>> origins,
                  final Map<String, String> objectStatuses,
                  final Map<String, Integer> objectLengths,
                  final List<WitnessStep> witness,
                  final List<CFGNode> traceNodes,
                  final String message) {
        this.status = status;
        this.checkerName = checkerName;
        this.kind = kind;
        this.functionName = functionName;
        this.file = file;
        this.line = line;
        this.column = column;
        this.rawColumn = rawColumn;
        this.accessText = accessText;
        this.accessNode = accessNode;
        this.origins = origins == null
                ? Collections.<Pair<CFGHandle, String>>emptySet()
                : Collections.unmodifiableSet(origins);
        this.objectStatuses = objectStatuses == null
                ? Collections.<String, String>emptyMap()
                : Collections.unmodifiableMap(
                        new TreeMap<String, String>(objectStatuses));
        this.objectLengths = objectLengths == null
                ? Collections.<String, Integer>emptyMap()
                : Collections.unmodifiableMap(
                        new TreeMap<String, Integer>(objectLengths));
        this.witness = witness == null
                ? Collections.<WitnessStep>emptyList()
                : Collections.unmodifiableList(new ArrayList<WitnessStep>(witness));
        this.traceNodes = traceNodes == null
                ? Collections.<CFGNode>emptyList()
                : Collections.unmodifiableList(new ArrayList<CFGNode>(traceNodes));
        this.message = message;
    }

    /** @return {@link #PROVED} or {@link #UNPROVED}. */
    public String getStatus() { return status; }

    public boolean isProved() { return PROVED.equals(status); }
    public boolean isUnproved() { return UNPROVED.equals(status); }

    public String getCheckerName() { return checkerName; }
    public String getKind() { return kind; }
    public String getFunctionName() { return functionName; }
    public String getFunction() { return functionName; }

    public File getFile() { return file; }
    public String getFileName() {
        return file == null ? "<unknown>" : file.getPath();
    }

    public int getLine() { return line; }
    /** Alias for {@link #getLine()}, 1-based source line. */
    public int getLineNumber() { return line; }
    public int getStartLine() { return line; }
    /** 1-based source column (ANTLR {@code bc} attributes are 0-based). */
    public int getColumn() { return column; }
    /** Alias for {@link #getColumn()}. */
    public int getColumnNumber() { return column; }
    public int getStartColumn() { return column; }
    /** Raw 0-based column offset as carried by the CFG XML. */
    public int getRawColumn() { return rawColumn; }

    public String getAccessText() { return accessText; }
    public CFGNode getAccessNode() { return accessNode; }

    /** Possible {@code (CFGHandle, array identifier)} underlying objects. */
    public Set<Pair<CFGHandle, String>> getOrigins() { return origins; }

    /** Sorted object identifier -> {@link #PROVED}/{@link #UNPROVED}. */
    public Map<String, String> getObjectStatuses() { return objectStatuses; }

    public Map<String, Integer> getObjectLengths() { return objectLengths; }

    public List<String> getObjects() {
        return new ArrayList<String>(objectStatuses.keySet());
    }

    public String getObjectStatus(final String object) {
        return objectStatuses.get(object);
    }

    /** One real, branch-coherent condition/rewrite location sequence. */
    public List<WitnessStep> getWitness() { return witness; }

    public List<String> getWitnessLocations() {
        final List<String> result = new ArrayList<String>();
        for (final WitnessStep step : witness)
            result.add(step.getLocation());
        return result;
    }

    public List<Integer> getWitnessLines() {
        final List<Integer> result = new ArrayList<Integer>();
        for (final WitnessStep step : witness)
            result.add(Integer.valueOf(step.getLine()));
        return result;
    }

    public List<String> getWitnessDescriptions() {
        final List<String> result = new ArrayList<String>();
        for (final WitnessStep step : witness)
            result.add(step.toString());
        return result;
    }

    /** CFG nodes backing {@link #getWitness()}, usable for checker traces. */
    public List<CFGNode> getTraceNodes() { return traceNodes; }

    public String getMessage() { return message; }

    /** Alias for {@link #getMessage()}. */
    public String getDescription() { return message; }

    /** Alias for {@link #getWitness()}. */
    public List<WitnessStep> getWitnessSteps() { return getWitness(); }

    public String getLocationString() {
        return getFileName() + ":" + line + ":" + column;
    }

    @Override
    public int compareTo(final BoundsFinding other) {
        int c = getFileName().compareTo(other.getFileName());
        if (c != 0) return c;
        if (line != other.line) return line - other.line;
        if (rawColumn != other.rawColumn)
            return rawColumn - other.rawColumn;
        if (column != other.column) return column - other.column;
        final String a = accessText == null ? "" : accessText;
        final String b = other.accessText == null ? "" : other.accessText;
        return a.compareTo(b);
    }

    @Override
    public boolean equals(final Object o) {
        if (this == o) return true;
        if (!(o instanceof BoundsFinding)) return false;
        return compareTo((BoundsFinding) o) == 0;
    }

    @Override
    public int hashCode() {
        int h = getFileName().hashCode();
        h = 31 * h + line;
        h = 31 * h + rawColumn;
        return h;
    }

    @Override
    public String toString() {
        final StringBuilder sb = new StringBuilder();
        sb.append('[').append(status).append("] ")
          .append(getLocationString())
          .append(" in ").append(functionName)
          .append(' ').append(accessText == null ? "arrayAccess" : accessText);
        if (!objectStatuses.isEmpty()) {
            sb.append(" objects=").append(objectStatuses);
        }
        if (message != null)
            sb.append(" -- ").append(message);
        if (isUnproved() && !witness.isEmpty()) {
            sb.append(" witness=[");
            for (int i = 0; i < witness.size(); i++) {
                if (i > 0) sb.append(" <- ");
                sb.append(witness.get(i));
            }
            sb.append(']');
        }
        return sb.toString();
    }

    /** Mutable accumulator used internally while the analysis runs. */
    static final class Accumulator {
        final String functionName;
        final File file;
        final int line;
        final int column;
        final int rawColumn;
        final String accessText;
        final CFGNode accessNode;
        final Map<Pair<CFGHandle, String>, Obj> objects =
                new LinkedHashMap<Pair<CFGHandle, String>, Obj>();
        boolean unknownObject = false;
        WitnessChoice unknownWitness = null;

        Accumulator(final String functionName, final File file,
                    final int line, final int column, final int rawColumn,
                    final String accessText, final CFGNode accessNode) {
            this.functionName = functionName;
            this.file = file;
            this.line = line;
            this.column = column;
            this.rawColumn = rawColumn;
            this.accessText = accessText;
            this.accessNode = accessNode;
        }

        static final class Obj {
            boolean evaluated = false;
            boolean proved = true;
            WitnessChoice badWitness = null;
            String reason = null;
        }
    }

    /** Lexicographically deterministic witness choice. */
    static final class WitnessChoice implements Comparable<WitnessChoice> {
        final List<WitnessStep> steps;
        final List<CFGNode> nodes;

        WitnessChoice(final List<WitnessStep> steps,
                      final List<CFGNode> nodes) {
            this.steps = steps;
            this.nodes = nodes;
        }

        public int compareTo(final WitnessChoice o) {
            final int n = Math.min(steps.size(), o.steps.size());
            for (int i = 0; i < n; i++) {
                final WitnessStep a = steps.get(i);
                final WitnessStep b = o.steps.get(i);
                if (a.getLine() != b.getLine())
                    return a.getLine() - b.getLine();
                if (a.getColumn() != b.getColumn())
                    return a.getColumn() - b.getColumn();
                final int k = a.getKind().compareTo(b.getKind());
                if (k != 0) return k;
                final int d = a.getDescription().compareTo(b.getDescription());
                if (d != 0) return d;
            }
            return steps.size() - o.steps.size();
        }

        static WitnessChoice min(final WitnessChoice a,
                                 final WitnessChoice b) {
            if (a == null) return b;
            if (b == null) return a;
            return a.compareTo(b) <= 0 ? a : b;
        }

        /**
         * Picks the more informative of two witnesses for a joined/widened
         * state: prefer the longer real path history (more rewrites/guards
         * traversed), breaking ties with deterministic lexicographic order.
         * Never concatenates the two.
         */
        static WitnessChoice forJoin(final WitnessChoice a,
                                     final WitnessChoice b) {
            if (a == null) return b;
            if (b == null) return a;
            if (a.steps.size() != b.steps.size())
                return a.steps.size() > b.steps.size() ? a : b;
            return min(a, b);
        }
    }
}
