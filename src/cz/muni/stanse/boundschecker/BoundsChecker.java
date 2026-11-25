package cz.muni.stanse.boundschecker;

import cz.muni.stanse.checker.Checker;
import cz.muni.stanse.checker.CheckerError;
import cz.muni.stanse.checker.CheckerErrorReceiver;
import cz.muni.stanse.checker.CheckerException;
import cz.muni.stanse.checker.CheckerProgressMonitor;
import cz.muni.stanse.checker.CheckingResult;
import cz.muni.stanse.checker.CheckingSuccess;
import cz.muni.stanse.codestructures.CFGHandle;
import cz.muni.stanse.codestructures.CFGNode;
import cz.muni.stanse.codestructures.LazyInternalStructures;
import cz.muni.stanse.pointeranalyzer.PointsToAnalyzer;
import cz.muni.stanse.pointeranalyzer.shapirohorwitz.ShapiroHorwitzAnalyzer;
import cz.muni.stanse.pointeranalyzer.shapirohorwitz.SteensgaardCategorizationProvider;
import cz.muni.stanse.utils.Pair;

import org.dom4j.Element;

import java.io.File;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Queue;
import java.util.Set;
import java.util.TreeSet;

/**
 * Intraprocedural static regression checker proving one-dimensional fixed
 * size integer array accesses in a small, headless C subset.
 *
 * <p>Every syntactic {@code a[i]} / {@code p[i]} access produces one stable
 * {@link BoundsFinding}.  The checker combines:</p>
 * <ul>
 *   <li>the production flow-sensitive points-to query
 *       {@link PointsToAnalyzer#getPointsToSetAt} for possible underlying
 *       objects at the exact CFG program point, and</li>
 *   <li>a disjunctive interval/pointer abstract execution over the real
 *       CFG which keeps (object, index constraint, condition witness)
 *       correlations across branches and converges loops with widening.</li>
 * </ul>
 *
 * <p>Unsupported forms (casts in indices, pointer arithmetic, unknown call
 * writes and anything outside the supported subset) are reported
 * {@link BoundsFinding#UNPROVED}; unrelated known relations are preserved.
 * Unproved never means that a vulnerability exists, only that the proof could
 * not be completed.</p>
 */
public class BoundsChecker extends Checker {

    public static final String CHECKER_NAME = "BoundsChecker";
    public static final String KIND = "bounds-proof";

    /** Max precise disjuncts accumulated at a loop header before widening. */
    private static final int WIDEN_THRESHOLD = 4;
    /** Hard cap on disjuncts kept at a non-loop merge. */
    private static final int DISJ_CAP = 48;
    /** Global work item safety cap for pathological CFGs. */
    private static final int WORK_CAP = 60000;

    public BoundsChecker() {
        super();
    }

    @Override
    public String getName() {
        return CHECKER_NAME;
    }

    /**
     * Analyze one function CFG and return all its bounds-proof findings,
     * sorted by file, line and column.  Each invocation is fully isolated.
     */
    public static List<BoundsFinding> analyze(final CFGHandle cfg) {
        if (cfg == null)
            return Collections.emptyList();
        return new Engine(cfg).run();
    }

    @Override
    public CheckingResult check(final LazyInternalStructures internals,
                                final CheckerErrorReceiver errReceiver,
                                final CheckerProgressMonitor monitor)
            throws CheckerException {
        if (monitor != null)
            monitor.write("Starting Bounds Checker");
        for (final CFGHandle cfg : internals.getCFGHandles()) {
            final List<BoundsFinding> findings = analyze(cfg);
            for (final BoundsFinding f : findings) {
                if (!f.isUnproved())
                    continue;
                final List<CFGNode> trace = new ArrayList<CFGNode>();
                final CFGNode startNode = cfg.getStartNode();
                if (startNode != null)
                    trace.add(startNode);
                trace.addAll(f.getTraceNodes());
                if (f.getAccessNode() != null)
                    trace.add(f.getAccessNode());
                // CheckerErrorTrace expects distinct first/last entries.
                if (trace.isEmpty() && f.getAccessNode() != null)
                    trace.add(f.getAccessNode());
                if (trace.size() == 1)
                    trace.add(trace.get(0));
                if (trace.isEmpty())
                    continue;
                final CheckerError error = new CheckerError(
                        "Array access bounds proof incomplete",
                        f.toString(),
                        2,
                        CHECKER_NAME,
                        trace,
                        "Path leading to the unchecked access",
                        "Conditions and rewrites along one real path",
                        "Bounds proof could not be completed: "
                                + f.getMessage(),
                        internals);
                errReceiver.receive(error);
            }
        }
        if (monitor != null)
            monitor.write("Bounds Checker finished");
        return new CheckingSuccess();
    }

    // ======================================================================
    // Interval domain
    // ======================================================================

    static final class Range {
        static final long BOUND = 1L << 40;
        static final Range TOP = new Range(-BOUND, BOUND);

        final long lo;
        final long hi;

        Range(final long lo, final long hi) {
            this.lo = clamp(lo);
            this.hi = clamp(hi);
        }

        static long clamp(final long v) {
            if (v > BOUND) return BOUND;
            if (v < -BOUND) return -BOUND;
            return v;
        }

        boolean isBottom() { return lo > hi; }

        boolean covers(final Range other) {
            return lo <= other.lo && other.hi <= hi;
        }

        Range intersect(final Range o) {
            return new Range(Math.max(lo, o.lo), Math.min(hi, o.hi));
        }

        Range plus(final Range o) {
            if (isBottom() || o.isBottom())
                return new Range(1, 0);
            return new Range(lo + o.lo, hi + o.hi);
        }

        Range minus(final Range o) {
            if (isBottom() || o.isBottom())
                return new Range(1, 0);
            return new Range(lo - o.hi, hi - o.lo);
        }

        Range negate() {
            return new Range(-hi, -lo);
        }

        Range shift(final long k) {
            return new Range(lo + k, hi + k);
        }

        @Override
        public String toString() {
            return "[" + (lo <= -BOUND ? "-inf" : Long.toString(lo))
                    + "," + (hi >= BOUND ? "+inf" : Long.toString(hi)) + "]";
        }
    }

    static final class Eval {
        final Range range;
        final boolean unsupported;

        Eval(final Range range, final boolean unsupported) {
            this.range = range;
            this.unsupported = unsupported;
        }

        static Eval top() { return new Eval(Range.TOP, false); }
        static Eval unsupported() { return new Eval(Range.TOP, true); }
    }

    // ======================================================================
    // Pointer domain value
    // ======================================================================

    static final class PVal {
        final boolean top;
        final Set<String> objs;
        final boolean loose; // derived through arithmetic/cast

        private PVal(final boolean top, final Set<String> objs,
                     final boolean loose) {
            this.top = top;
            this.objs = objs;
            this.loose = loose;
        }

        static PVal top() { return new PVal(true, null, true); }

        static PVal exact(final Set<String> objs) {
            return new PVal(false,
                    new HashSet<String>(objs == null
                            ? Collections.<String>emptySet() : objs),
                    false);
        }

        static PVal loose(final Set<String> objs) {
            return new PVal(false,
                    new HashSet<String>(objs == null
                            ? Collections.<String>emptySet() : objs),
                    true);
        }

        boolean covers(final PVal o) {
            if (top)
                return true;
            if (o == null || o.top)
                return false;
            if (!objs.containsAll(o.objs))
                return false;
            return loose || !o.loose;
        }

        PVal merge(final PVal o) {
            if (top || o.top)
                return PVal.top();
            final Set<String> all = new HashSet<String>(objs);
            all.addAll(o.objs);
            return new PVal(false, all, loose || o.loose);
        }
    }

    // ======================================================================
    // Abstract state: correlated int ranges + pointer bindings + witness
    // ======================================================================

    static final class State {
        final Map<String, Range> ints = new HashMap<String, Range>();
        final Map<String, PVal> ptrs = new HashMap<String, PVal>();
        final Set<String> escaped = new HashSet<String>();
        final List<BoundsFinding.WitnessStep> path =
                new ArrayList<BoundsFinding.WitnessStep>();
        final List<CFGNode> trace = new ArrayList<CFGNode>();
        boolean feasible = true;
        boolean widened = false;

        State copy() {
            final State s = new State();
            for (final Map.Entry<String, Range> e : ints.entrySet())
                s.ints.put(e.getKey(), e.getValue());
            for (final Map.Entry<String, PVal> e : ptrs.entrySet())
                s.ptrs.put(e.getKey(), e.getValue());
            s.escaped.addAll(escaped);
            s.path.addAll(path);
            s.trace.addAll(trace);
            s.feasible = feasible;
            s.widened = widened;
            return s;
        }

        boolean covers(final State o) {
            for (final Map.Entry<String, Range> e : o.ints.entrySet()) {
                final Range mine = ints.get(e.getKey());
                if (mine == null)
                    continue; // TOP covers
                if (!mine.covers(e.getValue()))
                    return false;
            }
            for (final Map.Entry<String, PVal> e : o.ptrs.entrySet()) {
                final PVal mine = ptrs.get(e.getKey());
                if (mine == null)
                    continue; // TOP covers
                if (!mine.covers(e.getValue()))
                    return false;
            }
            return escaped.containsAll(o.escaped);
        }

        BoundsFinding.WitnessChoice witnessChoice(
                final List<BoundsFinding.WitnessStep> extraSteps,
                final List<CFGNode> extraNodes) {
            final List<BoundsFinding.WitnessStep> steps =
                    new ArrayList<BoundsFinding.WitnessStep>(path);
            if (extraSteps != null)
                steps.addAll(extraSteps);
            final List<CFGNode> nodes = new ArrayList<CFGNode>(trace);
            if (extraNodes != null)
                nodes.addAll(extraNodes);
            return new BoundsFinding.WitnessChoice(steps, nodes);
        }
    }

    // ======================================================================
    // Linear form m*x + c used by condition refinement
    // ======================================================================

    static final class Linear {
        String name; // null for a pure constant
        long mul = 1;
        long add = 0;
        long constant = 0;

        Range range(final State s) {
            if (name == null)
                return new Range(constant, constant);
            Range x = s.ints.get(name);
            if (x == null)
                x = Range.TOP;
            if (mul == 1)
                return x.shift(add);
            return x.negate().shift(add);
        }
    }

    // ======================================================================
    // Engine
    // ======================================================================

    private static final class Engine {
        private final CFGHandle cfg;
        private final String functionName;
        private final Map<String, Integer> arrayLengths =
                new HashMap<String, Integer>();
        private final Set<String> arrayNames = new HashSet<String>();
        private final Set<String> pointerNames = new HashSet<String>();
        /** Location key -> init declarators of a C99 for-init declaration. */
        private final Map<String, List<Element>> forInitDecls =
                new HashMap<String, List<Element>>();
        private final PointsToAnalyzer pointsTo;

        private final Map<LocKey, BoundsFinding.Accumulator> records =
                new LinkedHashMap<LocKey, BoundsFinding.Accumulator>();
        private final Map<CFGNode, List<State>> accepted =
                new HashMap<CFGNode, List<State>>();
        private final Set<CFGNode> headers = new HashSet<CFGNode>();

        private int workItems = 0;
        private boolean truncated = false;

        Engine(final CFGHandle cfg) {
            this.cfg = cfg;
            this.functionName = cfg.getFunctionName();
            collectArrays();
            PointsToAnalyzer pta = null;
            try {
                final ShapiroHorwitzAnalyzer sh =
                        new ShapiroHorwitzAnalyzer(
                                new SteensgaardCategorizationProvider());
                try {
                    sh.analyze(Collections.singletonList(cfg));
                } catch (final Throwable t) {
                    // The independent program-point snapshot still works;
                    // a failure of the global unification analysis must not
                    // take down bounds checking.
                }
                pta = sh;
            } catch (final Throwable t) {
                pta = null;
            }
            this.pointsTo = pta;
        }

        // ------------------------------------------------------------------
        // Setup / XML helpers
        // ------------------------------------------------------------------

        private void collectArrays() {
            final Element root = cfg.getElement();
            if (root == null)
                return;
            @SuppressWarnings("unchecked")
            final List<Element> declarators =
                    root.selectNodes(".//declarator[arrayDecl]");
            for (final Element declarator : declarators) {
                final Element id = firstChildNamed(declarator, "id");
                if (id == null)
                    continue;
                final String name = id.getTextTrim();
                arrayNames.add(name);
                Integer length = null;
                final Element sizeConst = (Element)
                        declarator.selectSingleNode("arrayDecl/intConst");
                if (sizeConst != null) {
                    try {
                        length = Integer.valueOf(
                                Integer.decode(sizeConst.getTextTrim().trim()));
                    } catch (final Exception ex) {
                        length = null;
                    }
                }
                arrayLengths.put(name, length);
            }

            // Pointer-typed declarators keep their kind for the whole
            // function (C variables never change type).
            @SuppressWarnings("unchecked")
            final List<Element> ptrDeclarators =
                    root.selectNodes(".//declarator[pointer]");
            for (final Element declarator : ptrDeclarators) {
                final String name = declaratorId(declarator);
                if (name != null)
                    pointerNames.add(name);
            }

            // C99 for-init declarations are lowered by the CFG emitter to
            // an emptyStatement located at the forStatement position; keep
            // their initializers so the loop counter starts at its value.
            @SuppressWarnings("unchecked")
            final List<Element> fors = root.selectNodes(".//forStatement");
            for (final Element forStmt : fors) {
                @SuppressWarnings("unchecked")
                final List<Element> inits =
                        forStmt.selectNodes("declaration/initDeclarator");
                if (inits.isEmpty())
                    continue;
                final String key = forStmt.attributeValue("bl") + ":"
                        + forStmt.attributeValue("bc");
                forInitDecls.put(key, inits);
            }
        }

        // ------------------------------------------------------------------
        // Main run
        // ------------------------------------------------------------------

        List<BoundsFinding> run() {
            findHeaders();

            final CFGNode start = cfg.getStartNode();
            final State initial = new State();
            runLoop(start, initial);

            // Make sure every syntactic access has a record even when its
            // CFG node was never reached by the abstract execution.
            scanAllAccesses();

            final List<BoundsFinding> findings =
                    new ArrayList<BoundsFinding>();
            for (final BoundsFinding.Accumulator acc : records.values())
                findings.add(finalize(acc));

            Collections.sort(findings);
            return findings;
        }

        private final class WorkItem {
            final CFGNode node;
            final State state;
            WorkItem(final CFGNode node, final State state) {
                this.node = node;
                this.state = state;
            }
        }

        private final Queue<WorkItem> queue = new ArrayDeque<WorkItem>();
        private CFGNode activeNode = null;

        private void runLoop(final CFGNode start, final State initial) {
            if (start != null) {
                acceptedCompute(start).add(initial);
                queue.add(new WorkItem(start, initial));
            }

            while (!queue.isEmpty()) {
                final WorkItem item = queue.poll();
                if (++workItems > WORK_CAP) {
                    truncated = true;
                    break;
                }
                process(item.node, item.state);
            }
        }

        private List<State> acceptedCompute(final CFGNode n) {
            List<State> l = accepted.get(n);
            if (l == null) {
                l = new ArrayList<State>();
                accepted.put(n, l);
            }
            return l;
        }

        private void offer(final CFGNode node, final State s) {
            if (node == null || s == null || !s.feasible)
                return;
            final List<State> list = acceptedCompute(node);
            for (final State a : list) {
                if (a.covers(s))
                    return;
            }

            if (headers.contains(node)) {
                State wide = null;
                for (final State a : list) {
                    if (a.widened) {
                        wide = a;
                        break;
                    }
                }
                if (wide != null) {
                    final State w = widen(wide, s);
                    w.widened = true;
                    list.remove(wide);
                    list.add(w);
                    queue.add(new WorkItem(node, w));
                    return;
                }
                if (list.size() >= WIDEN_THRESHOLD) {
                    final State base = hull(list);
                    final State w = widen(base, s);
                    w.widened = true;
                    list.clear();
                    list.add(w);
                    queue.add(new WorkItem(node, w));
                    return;
                }
            }

            if (list.size() >= DISJ_CAP) {
                final List<State> all = new ArrayList<State>(list);
                all.add(s);
                final State h = hull(all);
                list.clear();
                list.add(h);
                queue.add(new WorkItem(node, h));
                return;
            }

            list.add(s);
            queue.add(new WorkItem(node, s));
        }

        // ------------------------------------------------------------------
        // CFG traversal processing
        // ------------------------------------------------------------------

        private void process(final CFGNode node, final State incoming) {
            activeNode = node;
            final Element el = node.getElement();
            State outgoing;

            if (el != null && "assert".equals(el.getName())) {
                final State constrained = incoming.copy();
                applyAssert(el, constrained);
                if (!constrained.feasible)
                    return;
                evaluateAccesses(node, el, constrained);
                outgoing = constrained;
            } else {
                evaluateAccesses(node, el, incoming);
                outgoing = incoming.copy();
                if (el != null)
                    exec(el, outgoing);
            }

            if (!outgoing.feasible)
                return;
            for (final CFGNode succ : node.getSuccessors())
                offer(succ, outgoing);
        }

        // ------------------------------------------------------------------
        // Statement transfer
        // ------------------------------------------------------------------

        private void exec(final Element e, final State s) {
            if (e == null)
                return;
            final String name = e.getName();

            if ("assignExpression".equals(name) && e.elements().size() >= 2) {
                // Side effects inside the operands happen first.
                exec((Element) e.elements().get(0), s);
                exec((Element) e.elements().get(1), s);
                applyAssignment(e, s);
                return;
            }

            if ("initDeclarator".equals(name)) {
                final Element init = (Element)
                        e.selectSingleNode("initializer/*[1]");
                if (init != null)
                    exec(init, s);
                applyInitDeclarator(e, s);
                return;
            }

            if ("emptyStatement".equals(name)) {
                // Recover a C99 for-init declaration lowered to emptyStatement.
                final String key = e.attributeValue("bl") + ":"
                        + e.attributeValue("bc");
                final List<Element> inits = forInitDecls.get(key);
                if (inits != null) {
                    for (final Element id : inits)
                        applyInitDeclarator(id, s);
                }
                return;
            }

            if (("postfixExpression".equals(name)
                    || "prefixExpression".equals(name))
                    && ("++".equals(e.attributeValue("op"))
                        || "--".equals(e.attributeValue("op")))) {
                applyIncDec(e, s);
                return;
            }

            if ("functionCall".equals(name)) {
                applyCall(e, s);
                return;
            }

            // Default: evaluate children so nested side effects are handled.
            for (final Object child : e.elements()) {
                if (child instanceof Element)
                    exec((Element) child, s);
            }
        }

        private void applyInitDeclarator(final Element initDecl,
                                         final State s) {
            final Element declarator =
                    firstChildNamed(initDecl, "declarator");
            final String lhs = declarator != null
                    ? declaratorId(declarator) : null;
            if (lhs == null)
                return;
            final Element rhs = (Element)
                    initDecl.selectSingleNode("initializer/*[1]");
            assignFromRhs(lhs, rhs, initDecl, s, "initialized");
        }

        private void applyAssignment(final Element assign, final State s) {
            final Element lhs = (Element) assign.elements().get(0);
            final Element rhs = (Element) assign.elements().get(1);
            final String op = assign.attributeValue("op");

            if ("id".equals(lhs.getName())) {
                final String name = lhs.getTextTrim();
                if (op != null) {
                    applyCompound(name, op, rhs, assign, s);
                } else {
                    assignFromRhs(name, rhs, assign, s, "assigned");
                }
                return;
            }

            if ("derefExpression".equals(lhs.getName())
                    || "arrayAccess".equals(lhs.getName())) {
                // Writes through pointers/cells do not rebind the base
                // variable.  Side effects in the index were already executed
                // by exec(); nothing scalar to update.
                appendRewrite(s, assign, "wrote through "
                        + exprText(lhs) + " (binding unchanged)");
                return;
            }

            final String root = rootId(lhs);
            if (root != null) {
                s.ints.put(root, Range.TOP);
                s.ptrs.put(root, PVal.top());
                appendRewrite(s, assign,
                        "unknown LHS shape overwrites " + root);
            }
        }

        private void applyCompound(final String name, final String op,
                                   final Element rhs, final Element node,
                                   final State s) {
            final boolean pointerish = pointerNames.contains(name);
            if (pointerish) {
                final PVal cur = s.ptrs.get(name);
                if (cur == null || cur.top) {
                    s.ptrs.put(name, PVal.top());
                } else {
                    s.ptrs.put(name, PVal.loose(cur.objs));
                }
                appendRewrite(s, node,
                        name + " " + op + "= ... is pointer arithmetic");
                return;
            }
            final Range cur = s.ints.containsKey(name)
                    ? s.ints.get(name) : Range.TOP;
            final Eval rv = evalInt(rhs, s);
            Range next;
            if ("+".equals(op))
                next = cur.plus(rv.range);
            else if ("-".equals(op))
                next = cur.minus(rv.range);
            else
                next = Range.TOP;
            s.ints.put(name, next);
            appendRewrite(s, node, name + " " + op + "= "
                    + exprText(rhs) + " giving " + next);
        }

        private void assignFromRhs(final String lhs, final Element rhs,
                                   final Element locationEl, final State s,
                                   final String verb) {
            if (arrayNames.contains(lhs)) {
                // Assigning to a whole array is invalid C; ignore.
                return;
            }

            // Pointer binding?  Variable kinds are fixed by declarations.
            if (pointerNames.contains(lhs)) {
                final PVal pv = evalPointer(rhs, s);
                s.ptrs.put(lhs, pv);
                // A pointer overwrite must not leave a stale int range around.
                s.ints.remove(lhs);
                appendRewrite(s, locationEl, verb + " pointer " + lhs
                        + " -> " + (pv.top ? "<unknown>" : pv.objs)
                        + (pv.loose ? " (loose)" : ""));
                return;
            }

            final Eval rv = evalInt(rhs, s);
            s.ints.put(lhs, rv.range);
            // An integer overwrite invalidates any prior pointer binding and
            // all old range constraints (range is replaced wholesale).
            s.ptrs.remove(lhs);
            appendRewrite(s, locationEl, verb + " " + lhs + " = "
                    + exprText(rhs) + " giving " + rv.range);
        }

        private void applyIncDec(final Element e, final State s) {
            final Element child = e.elements().isEmpty()
                    ? null : (Element) e.elements().get(0);
            if (child == null || !"id".equals(child.getName()))
                return;
            final String name = child.getTextTrim();
            final boolean inc = "++".equals(e.attributeValue("op"));
            if (pointerNames.contains(name)) {
                final PVal cur = s.ptrs.get(name);
                if (cur != null && !cur.top)
                    s.ptrs.put(name, PVal.loose(cur.objs));
                appendRewrite(s, e,
                        "pointer " + name + (inc ? "++" : "--")
                                + " makes offset unknown");
                return;
            }
            final Range cur = s.ints.containsKey(name)
                    ? s.ints.get(name) : new Range(0, 0);
            final Range next = inc ? cur.shift(1) : cur.shift(-1);
            s.ints.put(name, next);
            appendRewrite(s, e, name + (inc ? "++" : "--")
                    + " giving " + next);
        }

        private void applyCall(final Element call, final State s) {
            // Record address-taken roots.
            @SuppressWarnings("unchecked")
            final List<Element> addrs =
                    call.selectNodes(".//addrExpression/id");
            for (final Element id : addrs)
                s.escaped.add(id.getTextTrim());
            @SuppressWarnings("unchecked")
            final List<Element> bareAddrs =
                    call.selectNodes(".//addrExpression/arrayAccess/id");
            for (final Element id : bareAddrs)
                s.escaped.add(id.getTextTrim());

            // Unknown callee may rewrite every address-escaped symbol.
            for (final String esc : new HashSet<String>(s.escaped)) {
                s.ints.put(esc, Range.TOP);
                s.ptrs.put(esc, PVal.top());
            }
            final Element callee = call.elements().isEmpty()
                    ? null : (Element) call.elements().get(0);
            appendRewrite(s, call, "unknown call "
                    + (callee == null ? "" : exprText(callee))
                    + " may write escaped symbols " + s.escaped);
        }

        // ------------------------------------------------------------------
        // Pointer evaluation inside BoundsChecker states
        // ------------------------------------------------------------------

        private PVal evalPointer(final Element e, final State s) {
            if (e == null)
                return PVal.top();
            final String name = e.getName();

            if ("id".equals(name)) {
                final String id = e.getTextTrim();
                if (arrayNames.contains(id))
                    return PVal.exact(Collections.singleton(id));
                final PVal cur = s.ptrs.get(id);
                return cur == null ? PVal.top() : cur;
            }
            if ("addrExpression".equals(name)) {
                final Element addressed = e.elements().isEmpty()
                        ? null : (Element) e.elements().get(0);
                if (addressed == null)
                    return PVal.top();
                if ("id".equals(addressed.getName()))
                    return PVal.exact(
                            Collections.singleton(addressed.getTextTrim()));
                final String root = rootId(addressed);
                if (root != null)
                    return PVal.exact(Collections.singleton(root));
                return PVal.top();
            }
            if ("castExpression".equals(name)) {
                final Element inner = e.elements().size() >= 2
                        ? (Element) e.elements().get(1)
                        : (e.elements().isEmpty() ? null
                                : (Element) e.elements().get(0));
                final PVal v = evalPointer(inner, s);
                if (!v.top)
                    return PVal.loose(v.objs);
                return PVal.top();
            }
            if ("binaryExpression".equals(name)) {
                // Pointer arithmetic: origins may remain the pointer
                // operands' objects but offsets become loose.
                PVal merged = null;
                for (final Object child : e.elements()) {
                    if (!(child instanceof Element))
                        continue;
                    final PVal v = evalPointer((Element) child, s);
                    if (v != null && !v.top && !v.objs.isEmpty())
                        merged = merged == null ? v : merged.merge(v);
                }
                if (merged == null)
                    return PVal.top();
                return PVal.loose(merged.objs);
            }
            if ("conditionalExpression".equals(name)) {
                PVal merged = null;
                for (final Object child : e.elements()) {
                    if (!(child instanceof Element))
                        continue;
                    final PVal v = evalPointer((Element) child, s);
                    merged = merged == null ? v : merged.merge(v);
                }
                return merged == null ? PVal.top() : merged;
            }
            if ("functionCall".equals(name))
                return PVal.top();
            if ("parenExpression".equals(name) && !e.elements().isEmpty())
                return evalPointer((Element) e.elements().get(0), s);
            return PVal.top();
        }

        // ------------------------------------------------------------------
        // Integer interval evaluation
        // ------------------------------------------------------------------

        private Eval evalInt(final Element e, final State s) {
            if (e == null)
                return Eval.top();
            final String name = e.getName();

            if ("intConst".equals(name)) {
                try {
                    final long v = Long.decode(e.getTextTrim().trim());
                    return new Eval(new Range(v, v), false);
                } catch (final Exception ex) {
                    return Eval.unsupported();
                }
            }
            if ("id".equals(name)) {
                final String id = e.getTextTrim();
                final Range r = s.ints.get(id);
                return new Eval(r == null ? Range.TOP : r, false);
            }
            if ("castExpression".equals(name))
                return Eval.unsupported();

            if ("binaryExpression".equals(name)) {
                final String op = e.attributeValue("op");
                final List<Element> kids = elementChildren(e);
                if (kids.size() == 2) {
                    final Eval l = evalInt(kids.get(0), s);
                    final Eval r = evalInt(kids.get(1), s);
                    final boolean bad = l.unsupported || r.unsupported;
                    Range rr;
                    if ("+".equals(op))
                        rr = l.range.plus(r.range);
                    else if ("-".equals(op))
                        rr = l.range.minus(r.range);
                    else if ("*".equals(op))
                        rr = mulRange(l.range, r.range);
                    else
                        return Eval.unsupported();
                    return new Eval(rr, bad);
                }
                return Eval.unsupported();
            }

            if ("prefixExpression".equals(name)
                    || "postfixExpression".equals(name)) {
                final String op = e.attributeValue("op");
                final Element child = e.elements().isEmpty()
                        ? null : (Element) e.elements().get(0);
                Eval base = evalInt(child, s);
                if ("++".equals(op)) {
                    // prefix ++ yields incremented value; postfix ++ yields
                    // old value, both safe approximations use shifted range.
                    Range rr = "prefixExpression".equals(name)
                            ? base.range.shift(1) : base.range;
                    return new Eval(rr, base.unsupported);
                }
                if ("--".equals(op)) {
                    Range rr = "prefixExpression".equals(name)
                            ? base.range.shift(-1) : base.range;
                    return new Eval(rr, base.unsupported);
                }
                if ("-".equals(op))
                    return new Eval(base.range.negate(), base.unsupported);
                if ("+".equals(op))
                    return base;
                return Eval.unsupported();
            }

            if ("parenExpression".equals(name) && !e.elements().isEmpty())
                return evalInt((Element) e.elements().get(0), s);

            if ("conditionalExpression".equals(name)) {
                final List<Element> kids = elementChildren(e);
                // GNU extension allows x ? : y with two children.
                if (kids.size() >= 2) {
                    final Eval a = evalInt(kids.get(kids.size() - 2), s);
                    final Eval b = evalInt(kids.get(kids.size() - 1), s);
                    Range r = new Range(Math.min(a.range.lo, b.range.lo),
                            Math.max(a.range.hi, b.range.hi));
                    return new Eval(r, a.unsupported || b.unsupported);
                }
                return Eval.unsupported();
            }

            // sizeofExpression etc. could be constant but is outside the
            // guaranteed subset; stay conservative.
            return Eval.unsupported();
        }

        private Range mulRange(final Range a, final Range b) {
            if (a.isBottom() || b.isBottom())
                return new Range(1, 0);
            // Only handle constant multiplication precisely; otherwise top.
            if (a.lo == a.hi) {
                final long k = a.lo;
                if (k >= 0)
                    return new Range(b.lo * k, b.hi * k);
                return new Range(b.hi * k, b.lo * k);
            }
            if (b.lo == b.hi)
                return mulRange(b, a);
            return Range.TOP;
        }

        // ------------------------------------------------------------------
        // Branch condition refinement
        // ------------------------------------------------------------------

        private void applyAssert(final Element assertEl, final State s) {
            Element cond = null;
            boolean polarity = true;
            final List<Element> kids = elementChildren(assertEl);
            if (!kids.isEmpty())
                cond = kids.get(0);

            if (cond != null && "prefixExpression".equals(cond.getName())
                    && "!".equals(cond.attributeValue("op"))) {
                polarity = false;
                final List<Element> ck = elementChildren(cond);
                cond = ck.isEmpty() ? null : ck.get(0);
            }
            if (cond == null)
                return;

            refine(cond, polarity, s);
        }

        private void refine(final Element cond, final boolean truth,
                            final State s) {
            if (cond == null)
                return;
            final String name = cond.getName();

            if ("prefixExpression".equals(name)
                    && "!".equals(cond.attributeValue("op"))) {
                final List<Element> kids = elementChildren(cond);
                if (!kids.isEmpty())
                    refine(kids.get(0), !truth, s);
                return;
            }

            if ("binaryExpression".equals(name)) {
                final String op = cond.attributeValue("op");
                if ("&&".equals(op)) {
                    final List<Element> kids = elementChildren(cond);
                    if (truth && kids.size() == 2) {
                        refine(kids.get(0), true, s);
                        if (s.feasible)
                            refine(kids.get(1), true, s);
                    } else if (!truth && kids.size() == 2) {
                        // !(A && B) cannot be intersected as a whole.
                        appendConditionWitness(cond, false, s,
                                "negated conjunction (not refined)");
                    } else {
                        appendConditionWitness(cond, truth, s,
                                "disjunction (not refined)");
                    }
                    if (truth && kids.size() == 2)
                        appendConditionWitness(cond, true, s, null);
                    return;
                }
                if ("||".equals(op)) {
                    final List<Element> kids = elementChildren(cond);
                    if (!truth && kids.size() == 2) {
                        refine(kids.get(0), false, s);
                        if (s.feasible)
                            refine(kids.get(1), false, s);
                        appendConditionWitness(cond, false, s, null);
                    } else {
                        appendConditionWitness(cond, truth, s,
                                "disjunction (not refined)");
                    }
                    return;
                }

                String effectiveOp = op;
                if (!truth)
                    effectiveOp = invertOp(op);

                final List<Element> kids = elementChildren(cond);
                if (kids.size() == 2)
                    refineComparison(kids.get(0), kids.get(1),
                            effectiveOp, cond, truth, s);
                return;
            }

            if ("intConst".equals(name)) {
                boolean zero = "0".equals(cond.getTextTrim());
                if ((truth && zero) || (!truth && !zero))
                    s.feasible = false;
                appendConditionWitness(cond, truth, s, null);
                return;
            }

            // Bare id / unknown condition: record the real branch event but
            // no numeric refinement.
            appendConditionWitness(cond, truth, s, null);
        }

        private void refineComparison(final Element l, final Element r,
                                      final String op, final Element condEl,
                                      final boolean truth, final State s) {
            final Linear ll = linear(l, s);
            final Linear rl = linear(r, s);

            Linear variable = null;
            long constant = 0;
            // Comparison operator normalized to "variable cop constant".
            String cop = op;
            if (ll != null && ll.name != null && rl != null
                    && rl.name == null) {
                variable = ll;
                constant = rl.constant;
            } else if (rl != null && rl.name != null && ll != null
                    && ll.name == null) {
                variable = rl;
                constant = ll.constant;
                cop = reverseOp(op);
            }

            if (variable == null) {
                appendConditionWitness(condEl, truth, s,
                        "comparison without constant bound (not refined)");
                return;
            }

            final String varName = variable.name;

            if ("!=".equals(cop)) {
                if (variable.mul == 1 && variable.add == 0) {
                    final Range cur = s.ints.get(varName) == null
                            ? Range.TOP : s.ints.get(varName);
                    Range next = cur;
                    if (constant == cur.lo && constant == cur.hi) {
                        s.feasible = false;
                    } else if (constant == cur.lo) {
                        next = new Range(cur.lo + 1, cur.hi);
                    } else if (constant == cur.hi) {
                        next = new Range(cur.lo, cur.hi - 1);
                    }
                    if (next.isBottom())
                        s.feasible = false;
                    else
                        s.ints.put(varName, next);
                }
                appendConditionWitness(condEl, truth, s, null);
                return;
            }

            Range allowed;
            if ("<".equals(cop))
                allowed = new Range(-Range.BOUND, constant - 1);
            else if ("<=".equals(cop))
                allowed = new Range(-Range.BOUND, constant);
            else if (">".equals(cop))
                allowed = new Range(constant + 1, Range.BOUND);
            else if (">=".equals(cop))
                allowed = new Range(constant, Range.BOUND);
            else if ("==".equals(cop))
                allowed = new Range(constant, constant);
            else {
                appendConditionWitness(condEl, truth, s,
                        "unsupported comparison " + cop);
                return;
            }

            final Range linRange = variable.range(s);
            final Range constrained = linRange.intersect(allowed);
            if (constrained.isBottom()) {
                s.feasible = false;
                appendConditionWitness(condEl, truth, s,
                        "condition is infeasible on this path");
                return;
            }

            Range varRange;
            if (variable.mul == 1) {
                varRange = new Range(constrained.lo - variable.add,
                        constrained.hi - variable.add);
            } else {
                varRange = new Range(variable.add - constrained.hi,
                        variable.add - constrained.lo);
            }

            final Range old = s.ints.get(varName);
            if (old != null)
                varRange = varRange.intersect(old);
            if (varRange.isBottom()) {
                s.feasible = false;
            } else {
                s.ints.put(varName, varRange);
            }
            appendConditionWitness(condEl, truth, s, null);
        }

        private Linear linear(final Element e, final State s) {
            if (e == null)
                return null;
            final String name = e.getName();
            if ("id".equals(name)) {
                if (arrayNames.contains(e.getTextTrim()))
                    return null;
                final Linear l = new Linear();
                l.name = e.getTextTrim();
                return l;
            }
            if ("intConst".equals(name)) {
                try {
                    final Linear l = new Linear();
                    l.constant = Long.decode(e.getTextTrim().trim());
                    l.name = null;
                    return l;
                } catch (final Exception ex) {
                    return null;
                }
            }
            if ("prefixExpression".equals(name)
                    && "-".equals(e.attributeValue("op"))
                    && e.elements().size() == 1) {
                final Linear inner = linear((Element) e.elements().get(0), s);
                if (inner == null)
                    return null;
                if (inner.name == null) {
                    inner.constant = -inner.constant;
                } else {
                    inner.mul = -inner.mul;
                    inner.add = -inner.add;
                }
                return inner;
            }
            if ("binaryExpression".equals(name)) {
                final String op = e.attributeValue("op");
                final List<Element> kids = elementChildren(e);
                if (kids.size() != 2)
                    return null;
                final Linear a = linear(kids.get(0), s);
                final Linear b = linear(kids.get(1), s);
                if (a == null || b == null)
                    return null;
                if (a.name == null && b.name == null) {
                    final Linear l = new Linear();
                    l.name = null;
                    l.constant = "+".equals(op)
                            ? a.constant + b.constant
                            : a.constant - b.constant;
                    return l;
                }
                if (a.name != null && b.name == null) {
                    if ("+".equals(op)) {
                        a.add += b.constant;
                        return a;
                    }
                    if ("-".equals(op)) {
                        a.add -= b.constant;
                        return a;
                    }
                }
                if (b.name != null && a.name == null) {
                    if ("+".equals(op)) {
                        b.add += a.constant;
                        return b;
                    }
                    if ("-".equals(op)) {
                        b.mul = -b.mul;
                        b.add = -b.add + a.constant;
                        return b;
                    }
                }
            }
            return null;
        }

        private String invertOp(final String op) {
            if ("<".equals(op)) return ">=";
            if (">=".equals(op)) return "<";
            if ("<=".equals(op)) return ">";
            if (">".equals(op)) return "<=";
            if ("==".equals(op)) return "!=";
            if ("!=".equals(op)) return "==";
            return op;
        }

        /** Operator for {@code constant OP variable} as {@code variable OP' constant}. */
        private String reverseOp(final String op) {
            if ("<".equals(op)) return ">";
            if (">".equals(op)) return "<";
            if ("<=".equals(op)) return ">=";
            if (">=".equals(op)) return "<=";
            return op; // == and != are symmetric
        }

        // ------------------------------------------------------------------
        // Witness recording
        // ------------------------------------------------------------------

        private void appendRewrite(final State s, final Element locationEl,
                                   final String description) {
            final int[] lc = location(locationEl, activeNode);
            s.path.add(new BoundsFinding.WitnessStep(
                    lc[0], lc[1] + 1, "rewrite", description, activeNode));
            if (activeNode != null)
                s.trace.add(activeNode);
        }

        private void appendConditionWitness(final Element cond,
                                           final boolean truth,
                                           final State s,
                                           final String extra) {
            final int[] lc = location(cond, activeNode);
            String text = exprText(cond);
            if (text.length() > 80)
                text = text.substring(0, 80) + "...";
            final String desc = (truth ? "condition true on path: "
                    : "condition false on path: ") + text
                    + (extra == null ? "" : " (" + extra + ")");
            s.path.add(new BoundsFinding.WitnessStep(
                    lc[0], lc[1] + 1, "condition", desc, activeNode));
            if (activeNode != null)
                s.trace.add(activeNode);
        }

        // ------------------------------------------------------------------
        // Access evaluation and records
        // ------------------------------------------------------------------

        private void evaluateAccesses(final CFGNode node,
                                      final Element container,
                                      final State state) {
            if (container == null)
                return;
            @SuppressWarnings("unchecked")
            final List<Element> accs =
                    container.selectNodes("descendant-or-self::arrayAccess");
            for (final Element acc : accs)
                evaluateAccess(node, acc, state);
        }

        private void evaluateAccess(final CFGNode node, final Element acc,
                                    final State state) {
            final LocKey key = LocKey.of(acc, node);
            BoundsFinding.Accumulator rec = records.get(key);
            if (rec == null) {
                final int[] lc = location(acc, node);
                rec = new BoundsFinding.Accumulator(
                        functionName,
                        node == null ? null : node.getFile(),
                        lc[0], lc[1] + 1, lc[1],
                        exprText(acc), node);
                records.put(key, rec);
            }

            final List<Element> kids = elementChildren(acc);
            if (kids.size() < 2) {
                markUnknown(rec, state, node, acc,
                        "malformed arrayAccess");
                return;
            }
            final Element base = kids.get(0);
            final Element indexEl = kids.get(1);
            final String root = rootId(base);
            if (root == null) {
                markUnknown(rec, state, node, acc,
                        "cannot resolve base expression");
                return;
            }
            // Nested arrayAccess means a multi-dimensional array, which is
            // outside the supported one-dimensional subset.
            final boolean unsupportedForm =
                    "arrayAccess".equals(base.getName());

            final Eval index = evalInt(indexEl, state);

            Set<Pair<CFGHandle, String>> origins = null;
            if (pointsTo != null) {
                try {
                    origins = pointsTo.getPointsToSetAt(cfg, node, root);
                } catch (final Throwable t) {
                    origins = null;
                }
            }
            if (origins == null)
                origins = new HashSet<Pair<CFGHandle, String>>();

            // Direct arrays have a certain self origin even if the
            // production query is unavailable.
            if (arrayNames.contains(root) && origins.isEmpty())
                origins = Collections.singleton(Pair.make(cfg, root));

            if (origins.isEmpty()) {
                markUnknown(rec, state, node, acc,
                        "no pointer origin known for '" + root + "'");
                return;
            }

            final PVal binding = state.ptrs.get(root);
            for (final Pair<CFGHandle, String> origin : origins) {
                final String objName = origin.getSecond();
                final boolean local = cfg.equals(origin.getFirst());

                BoundsFinding.Accumulator.Obj obj = rec.objects.get(origin);
                if (obj == null) {
                    obj = new BoundsFinding.Accumulator.Obj();
                    rec.objects.put(origin, obj);
                }

                // Correlation: a precise per-path binding only pairs with
                // its own object(s), never cross-combined with other
                // branches' origins.  A skipped origin stays unevaluated for
                // this state and is paired by the matching path state (or
                // conservatively fails at finalization).
                if (!arrayNames.contains(root) && binding != null
                        && !binding.top) {
                    if (origin.getFirst() != null
                            && !cfg.equals(origin.getFirst()))
                        continue;
                    if (!binding.objs.contains(objName))
                        continue;
                }

                obj.evaluated = true;

                String fail = null;
                Integer length = local ? arrayLengths.get(objName) : null;
                if (unsupportedForm)
                    fail = "multi-dimensional array access is outside the "
                            + "supported one-dimensional subset";
                else if (!local)
                    fail = "object belongs to another CFG";
                else if (length == null)
                    fail = "length of '" + objName + "' is not a known "
                            + "fixed int constant";
                else if (index.unsupported)
                    fail = "index uses an unsupported form (cast/unknown "
                            + "expression)";
                else if (!arrayNames.contains(root) && binding != null
                        && binding.loose)
                    fail = "pointer '" + root + "' was derived through "
                            + "arithmetic/cast, object offset unknown";
                else if (index.range.lo < 0)
                    fail = "index range " + index.range
                            + " may be negative";
                else if (index.range.hi >= length)
                    fail = "index range " + index.range
                            + " reaches length " + length;

                if (fail != null) {
                    obj.proved = false;
                    obj.reason = fail;
                    final List<BoundsFinding.WitnessStep> extra =
                            new ArrayList<BoundsFinding.WitnessStep>();
                    final int[] lc = location(acc, node);
                    extra.add(new BoundsFinding.WitnessStep(
                            lc[0], lc[1] + 1, "bounds-check",
                            "access " + exprText(acc) + " against '"
                                    + objName + "' (length=" + length
                                    + "): " + fail, node));
                    final List<CFGNode> extraNodes =
                            new ArrayList<CFGNode>();
                    if (node != null)
                        extraNodes.add(node);
                    obj.badWitness = BoundsFinding.WitnessChoice.min(
                            obj.badWitness,
                            state.witnessChoice(extra, extraNodes));
                }
            }
        }

        private void markUnknown(final BoundsFinding.Accumulator rec,
                                 final State state, final CFGNode node,
                                 final Element acc, final String reason) {
            rec.unknownObject = true;
            final List<BoundsFinding.WitnessStep> extra =
                    new ArrayList<BoundsFinding.WitnessStep>();
            final int[] lc = location(acc, node);
            extra.add(new BoundsFinding.WitnessStep(
                    lc[0], lc[1] + 1, "bounds-check", reason, node));
            final List<CFGNode> extraNodes = new ArrayList<CFGNode>();
            if (node != null)
                extraNodes.add(node);
            rec.unknownWitness = BoundsFinding.WitnessChoice.min(
                    rec.unknownWitness,
                    state.witnessChoice(extra, extraNodes));
        }

        private void scanAllAccesses() {
            for (final CFGNode node : cfg.getAllNodes()) {
                final Element el = node.getElement();
                if (el == null)
                    continue;
                @SuppressWarnings("unchecked")
                final List<Element> accs =
                        el.selectNodes("descendant-or-self::arrayAccess");
                for (final Element acc : accs) {
                    final LocKey key = LocKey.of(acc, node);
                    if (!records.containsKey(key)) {
                        final int[] lc = location(acc, node);
                        final BoundsFinding.Accumulator rec =
                                new BoundsFinding.Accumulator(
                                        functionName, node.getFile(),
                                        lc[0], lc[1] + 1, lc[1],
                                        exprText(acc), node);
                        rec.unknownObject = true;
                        rec.unknownWitness =
                                new BoundsFinding.WitnessChoice(
                                        Collections.singletonList(
                                                new BoundsFinding.WitnessStep(
                                                        lc[0], lc[1] + 1,
                                                        "bounds-check",
                                                        "access was not "
                                                        + "reached by the "
                                                        + "abstract execution",
                                                        node)),
                                        Collections.singletonList(node));
                        records.put(key, rec);
                    }
                }
            }
        }

        // ------------------------------------------------------------------
        // Finalize one access into a finding
        // ------------------------------------------------------------------

        private BoundsFinding finalize(
                final BoundsFinding.Accumulator rec) {
            boolean proved = !truncated;
            final Map<String, String> statuses =
                    new HashMap<String, String>();
            final Map<String, Integer> lengths =
                    new HashMap<String, Integer>();
            final Set<Pair<CFGHandle, String>> origins =
                    new TreeSet<Pair<CFGHandle, String>>(
                            new Comparator<Pair<CFGHandle, String>>() {
                                public int compare(
                                        final Pair<CFGHandle, String> a,
                                        final Pair<CFGHandle, String> b) {
                                    return a.getSecond().compareTo(
                                            b.getSecond());
                                }
                            });

            BoundsFinding.WitnessChoice witness = rec.unknownWitness;
            if (rec.unknownObject)
                proved = false;

            for (final Map.Entry<Pair<CFGHandle, String>,
                                 BoundsFinding.Accumulator.Obj> e
                    : rec.objects.entrySet()) {
                final Pair<CFGHandle, String> origin = e.getKey();
                final BoundsFinding.Accumulator.Obj obj = e.getValue();
                origins.add(origin);
                final String name = origin.getSecond();
                final Integer len = arrayLengths.get(name);
                if (len != null)
                    lengths.put(name, len);

                if (!obj.evaluated) {
                    obj.proved = false;
                    if (obj.badWitness == null && rec.accessNode != null) {
                        final List<BoundsFinding.WitnessStep> steps =
                                new ArrayList<BoundsFinding.WitnessStep>();
                        steps.add(new BoundsFinding.WitnessStep(
                                rec.line, rec.column, "bounds-check",
                                "possible object '" + name + "' was not "
                                + "paired with a reachable path state",
                                rec.accessNode));
                        obj.badWitness = new BoundsFinding.WitnessChoice(
                                steps,
                                Collections.singletonList(rec.accessNode));
                    }
                }

                if (obj.proved && obj.evaluated) {
                    statuses.put(displayName(origin),
                            BoundsFinding.PROVED);
                } else {
                    proved = false;
                    statuses.put(displayName(origin),
                            BoundsFinding.UNPROVED);
                    witness = BoundsFinding.WitnessChoice.min(
                            witness, obj.badWitness);
                }
            }

            if (rec.unknownObject)
                statuses.put("<unknown>", BoundsFinding.UNPROVED);

            if (statuses.isEmpty()) {
                proved = false;
                statuses.put("<no-origin>", BoundsFinding.UNPROVED);
            }

            if (truncated) {
                proved = false;
                statuses.put("<analysis-budget-exhausted>",
                        BoundsFinding.UNPROVED);
            }

            if (proved) {
                // All known objects proved on every paired path.
                final Map<String, String> provedStatuses =
                        new HashMap<String, String>();
                for (final String k : statuses.keySet())
                    provedStatuses.put(k, BoundsFinding.PROVED);
                statuses.clear();
                statuses.putAll(provedStatuses);
            }

            final String status = proved
                    ? BoundsFinding.PROVED : BoundsFinding.UNPROVED;
            final String message = proved
                    ? "0 <= index < length on every reachable path"
                    : "bounds proof incomplete on at least one path/object";

            List<BoundsFinding.WitnessStep> steps =
                    Collections.emptyList();
            List<CFGNode> nodes = Collections.emptyList();
            if (!proved && witness != null) {
                steps = witness.steps;
                nodes = witness.nodes;
            } else if (!proved) {
                steps = Collections.singletonList(
                        new BoundsFinding.WitnessStep(
                                rec.line, rec.column, "bounds-check",
                                message, rec.accessNode));
                if (rec.accessNode != null)
                    nodes = Collections.singletonList(rec.accessNode);
            }

            return new BoundsFinding(
                    status, CHECKER_NAME, KIND,
                    rec.functionName, rec.file, rec.line, rec.column,
                    rec.rawColumn, rec.accessText, rec.accessNode,
                    origins, statuses, lengths, steps, nodes, message);
        }

        private String displayName(final Pair<CFGHandle, String> origin) {
            if (cfg.equals(origin.getFirst()))
                return origin.getSecond();
            return origin.getFirst() == null
                    ? origin.getSecond()
                    : origin.getFirst().getFunctionName()
                            + ":" + origin.getSecond();
        }

        // ------------------------------------------------------------------
        // Join / widening
        // ------------------------------------------------------------------

        private State hull(final List<State> states) {
            final State h = new State();
            final Set<String> intKeys = new HashSet<String>();
            final Set<String> ptrKeys = new HashSet<String>();
            for (final State s : states) {
                intKeys.addAll(s.ints.keySet());
                ptrKeys.addAll(s.ptrs.keySet());
                h.escaped.addAll(s.escaped);
            }
            for (final String k : intKeys) {
                Range r = null;
                for (final State s : states) {
                    final Range x = s.ints.get(k);
                    if (x == null) { r = Range.TOP; break; }
                    r = r == null ? x : new Range(
                            Math.min(r.lo, x.lo), Math.max(r.hi, x.hi));
                }
                h.ints.put(k, r == null ? Range.TOP : r);
            }
            for (final String k : ptrKeys) {
                PVal p = null;
                for (final State s : states) {
                    final PVal x = s.ptrs.get(k);
                    if (x == null || x.top) { p = PVal.top(); break; }
                    p = p == null ? x : p.merge(x);
                }
                h.ptrs.put(k, p == null ? PVal.top() : p);
            }
            // Pick one real witness, never concatenate branch histories.
            BoundsFinding.WitnessChoice best = null;
            for (final State s : states)
                best = BoundsFinding.WitnessChoice.forJoin(best,
                        s.witnessChoice(null, null));
            if (best != null) {
                h.path.addAll(best.steps);
                h.trace.addAll(best.nodes);
            }
            return h;
        }

        private State widen(final State oldState, final State newState) {
            final State w = new State();
            final Set<String> intKeys = new HashSet<String>();
            intKeys.addAll(oldState.ints.keySet());
            intKeys.addAll(newState.ints.keySet());
            for (final String k : intKeys) {
                final Range o = oldState.ints.get(k);
                final Range n = newState.ints.get(k);
                if (o == null || n == null) {
                    w.ints.put(k, Range.TOP);
                    continue;
                }
                final long lo = n.lo < o.lo ? -Range.BOUND : o.lo;
                final long hi = n.hi > o.hi ? Range.BOUND : o.hi;
                w.ints.put(k, new Range(lo, hi));
            }
            final Set<String> ptrKeys = new HashSet<String>();
            ptrKeys.addAll(oldState.ptrs.keySet());
            ptrKeys.addAll(newState.ptrs.keySet());
            for (final String k : ptrKeys) {
                final PVal o = oldState.ptrs.get(k);
                final PVal n = newState.ptrs.get(k);
                if (o == null || n == null)
                    w.ptrs.put(k, PVal.top());
                else
                    w.ptrs.put(k, o.merge(n));
            }
            w.escaped.addAll(oldState.escaped);
            w.escaped.addAll(newState.escaped);

            final BoundsFinding.WitnessChoice a =
                    oldState.witnessChoice(null, null);
            final BoundsFinding.WitnessChoice b =
                    newState.witnessChoice(null, null);
            final BoundsFinding.WitnessChoice best =
                    BoundsFinding.WitnessChoice.forJoin(a, b);
            if (best != null) {
                w.path.addAll(best.steps);
                w.trace.addAll(best.nodes);
            }
            return w;
        }

        // ------------------------------------------------------------------
        // Back-edge / loop header discovery
        // ------------------------------------------------------------------

        private void findHeaders() {
            final CFGNode start = cfg.getStartNode();
            if (start == null)
                return;
            final Map<CFGNode, Integer> color =
                    new HashMap<CFGNode, Integer>();
            final Deque<CFGNode> stack = new ArrayDeque<CFGNode>();
            stack.push(start);
            // Iterative DFS with entry/exit coloring.
            final Map<CFGNode, java.util.Iterator<CFGNode>> it =
                    new HashMap<CFGNode, java.util.Iterator<CFGNode>>();
            color.put(start, 1);
            final List<CFGNode> initialSuccs =
                    new ArrayList<CFGNode>(start.getSuccessors());
            it.put(start, initialSuccs.iterator());
            while (!stack.isEmpty()) {
                final CFGNode n = stack.peek();
                java.util.Iterator<CFGNode> iter = it.get(n);
                if (iter == null) {
                    iter = new ArrayList<CFGNode>(
                            n.getSuccessors()).iterator();
                    it.put(n, iter);
                }
                if (iter.hasNext()) {
                    final CFGNode m = iter.next();
                    final Integer c = color.get(m);
                    if (c != null && c == 1) {
                        headers.add(m); // back edge n -> m
                    } else if (c == null) {
                        color.put(m, 1);
                        it.put(m, new ArrayList<CFGNode>(
                                m.getSuccessors()).iterator());
                        stack.push(m);
                    }
                } else {
                    color.put(n, 2);
                    stack.pop();
                }
            }
        }
    }

    // ======================================================================
    // Location key
    // ======================================================================

    static final class LocKey {
        final String file;
        final int line;
        final int rawColumn;

        LocKey(final String file, final int line, final int rawColumn) {
            this.file = file == null ? "" : file;
            this.line = line;
            this.rawColumn = rawColumn;
        }

        static LocKey of(final Element acc, final CFGNode node) {
            final int[] lc = location(acc, node);
            String f = null;
            if (node != null && node.getFile() != null)
                f = node.getFile().getPath();
            return new LocKey(f, lc[0], lc[1]);
        }

        @Override
        public boolean equals(final Object o) {
            if (!(o instanceof LocKey))
                return false;
            final LocKey k = (LocKey) o;
            return line == k.line && rawColumn == k.rawColumn
                    && file.equals(k.file);
        }

        @Override
        public int hashCode() {
            return file.hashCode() * 31 + line * 17 + rawColumn;
        }
    }

    // ======================================================================
    // Static XML/location helpers
    // ======================================================================

    static int[] location(final Element e, final CFGNode fallback) {
        int line = fallback == null ? 1 : fallback.getLine();
        int col = fallback == null ? 0 : fallback.getColumn();
        if (e != null) {
            final String bl = e.attributeValue("bl");
            final String bc = e.attributeValue("bc");
            if (bl != null) {
                try { line = Integer.parseInt(bl); }
                catch (final NumberFormatException ignore) { }
            }
            if (bc != null) {
                try { col = Integer.parseInt(bc); }
                catch (final NumberFormatException ignore) { }
            }
        }
        return new int[] { line, col };
    }

    static Element firstChildNamed(final Element e, final String name) {
        if (e == null)
            return null;
        for (final Object o : e.elements(name)) {
            if (o instanceof Element)
                return (Element) o;
        }
        return null;
    }

    static List<Element> elementChildren(final Element e) {
        final List<Element> out = new ArrayList<Element>();
        if (e == null)
            return out;
        for (final Object o : e.elements()) {
            if (o instanceof Element)
                out.add((Element) o);
        }
        return out;
    }

    static String declaratorId(final Element declarator) {
        if (declarator == null)
            return null;
        final Element id = firstChildNamed(declarator, "id");
        if (id != null)
            return id.getTextTrim();
        return rootId(declarator);
    }

    static String rootId(final Element expr) {
        if (expr == null)
            return null;
        if ("id".equals(expr.getName()))
            return expr.getTextTrim();
        @SuppressWarnings("unchecked")
        final List<Element> ids = expr.selectNodes(".//id");
        if (!ids.isEmpty())
            return ids.get(0).getTextTrim();
        return null;
    }

    static String exprText(final Element e) {
        if (e == null)
            return "?";
        final String n = e.getName();
        if ("id".equals(n) || "intConst".equals(n))
            return e.getTextTrim();
        if ("arrayAccess".equals(n) && e.elements().size() >= 2)
            return exprText((Element) e.elements().get(0)) + "["
                    + exprText((Element) e.elements().get(1)) + "]";
        if ("binaryExpression".equals(n) && e.elements().size() == 2)
            return exprText((Element) e.elements().get(0))
                    + opSymbol(e.attributeValue("op"))
                    + exprText((Element) e.elements().get(1));
        if ("prefixExpression".equals(n) && !e.elements().isEmpty())
            return opSymbol(e.attributeValue("op"))
                    + exprText((Element) e.elements().get(0));
        if ("postfixExpression".equals(n) && !e.elements().isEmpty())
            return exprText((Element) e.elements().get(0))
                    + opSymbol(e.attributeValue("op"));
        if ("addrExpression".equals(n) && !e.elements().isEmpty())
            return "&" + exprText((Element) e.elements().get(0));
        if ("derefExpression".equals(n) && !e.elements().isEmpty())
            return "*" + exprText((Element) e.elements().get(0));
        final String t = e.getTextTrim();
        return t.isEmpty() ? n : t;
    }

    static String opSymbol(final String op) {
        if (op == null)
            return "?";
        if ("&&".equals(op)) return " && ";
        if ("||".equals(op)) return " || ";
        return " " + op + " ";
    }
}
