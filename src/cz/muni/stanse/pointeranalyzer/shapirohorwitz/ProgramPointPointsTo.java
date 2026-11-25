package cz.muni.stanse.pointeranalyzer.shapirohorwitz;

import cz.muni.stanse.codestructures.CFGHandle;
import cz.muni.stanse.codestructures.CFGNode;
import cz.muni.stanse.utils.Pair;

import org.dom4j.Element;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Queue;
import java.util.Set;

/**
 * Intraprocedural, flow-sensitive may-points-to snapshots.
 *
 * <p>This is a small monotone dataflow used to answer
 * {@link cz.muni.stanse.pointeranalyzer.PointsToAnalyzer#getPointsToSetAt}.
 * The lattice for every symbol is the powerset of local symbols with an
 * additional top value (represented by a <code>null</code> set) for pointers
 * the transfer cannot model.  Joins are set unions; assignments perform
 * strong updates of the assigned symbol only, so pointer rebindings are
 * reflected at later program points while old aliases keep their objects.</p>
 *
 * <p>The analysis is deliberately conservative and only feeds origin
 * information; it never interprets index arithmetic.</p>
 */
final class ProgramPointPointsTo {

    /** Top value for a symbol's points-to set. */
    private static final Set<String> TOP = null;

    private final CFGHandle cfg;
    private final Set<String> localSymbols = new HashSet<String>();
    private final Set<String> arrays = new HashSet<String>();

    private final Map<CFGNode, Map<String, Set<String>>> entry =
            new HashMap<CFGNode, Map<String, Set<String>>>();
    private final Map<CFGNode, Map<String, Set<String>>> exit =
            new HashMap<CFGNode, Map<String, Set<String>>>();

    private ProgramPointPointsTo(final CFGHandle cfg) {
        this.cfg = cfg;
        collectSymbols();
        run();
    }

    static Set<Pair<CFGHandle, String>> query(
            final Map<CFGHandle, ProgramPointPointsTo> cache,
            final CFGHandle cfg, final CFGNode node, final String id) {
        if (cfg == null || node == null || id == null)
            return Collections.emptySet();
        ProgramPointPointsTo snapshot;
        synchronized (cache) {
            snapshot = cache.get(cfg);
            if (snapshot == null) {
                snapshot = new ProgramPointPointsTo(cfg);
                cache.put(cfg, snapshot);
            }
        }
        return snapshot.lookup(node, id);
    }

    private Set<Pair<CFGHandle, String>> lookup(
            final CFGNode node, final String id) {
        final Set<Pair<CFGHandle, String>> result =
                new LinkedHashSet<Pair<CFGHandle, String>>();

        // An array identifier decays to a pointer to its own first element.
        if (arrays.contains(id))
            result.add(Pair.make(cfg, id));

        final Map<String, Set<String>> in = entry.get(node);
        if (in != null) {
            final Set<String> pts = in.get(id);
            if (pts != null) {
                for (final String object : pts)
                    result.add(Pair.make(cfg, object));
            }
        }
        return result;
    }

    private void collectSymbols() {
        final Set<String> symbols = cfg.getSymbols();
        if (symbols != null) {
            for (final String s : symbols)
                localSymbols.add(s);
        }

        final Element root = cfg.getElement();
        if (root == null)
            return;
        @SuppressWarnings("unchecked")
        final List<Element> declarators =
                root.selectNodes(".//declarator[arrayDecl]");
        for (final Element declarator : declarators) {
            final Element id = firstChildNamed(declarator, "id");
            if (id != null)
                arrays.add(id.getTextTrim());
        }
    }

    private Map<String, Set<String>> bottomState() {
        final Map<String, Set<String>> m =
                new HashMap<String, Set<String>>();
        for (final String s : localSymbols)
            m.put(s, new HashSet<String>());
        for (final String a : arrays)
            m.put(a, new HashSet<String>(Collections.singleton(a)));
        return m;
    }

    private void run() {
        final CFGNode start = cfg.getStartNode();
        if (start == null)
            return;

        entry.put(start, bottomState());

        final Queue<CFGNode> worklist = new ArrayDeque<CFGNode>();
        worklist.add(start);
        int steps = 0;
        final int cap = 200000;

        while (!worklist.isEmpty()) {
            final CFGNode node = worklist.poll();
            if (++steps > cap)
                break;

            Map<String, Set<String>> in = entry.get(node);
            if (in == null)
                in = bottomState();

            final Map<String, Set<String>> out = transfer(node, in);
            final Map<String, Set<String>> oldOut = exit.get(node);
            if (sameState(oldOut, out))
                continue;

            exit.put(node, out);
            for (final CFGNode succ : node.getSuccessors()) {
                Map<String, Set<String>> succIn = entry.get(succ);
                if (succIn == null) {
                    succIn = bottomState();
                    entry.put(succ, succIn);
                }
                mergeInto(succIn, out);
                worklist.add(succ);
            }
        }
    }

    private boolean sameState(final Map<String, Set<String>> a,
                               final Map<String, Set<String>> b) {
        if (a == b)
            return true;
        if (a == null || b == null)
            return false;
        if (a.size() != b.size())
            return false;
        for (final Map.Entry<String, Set<String>> e : a.entrySet()) {
            final Set<String> x = e.getValue();
            final Set<String> y = b.get(e.getKey());
            if (x == null || y == null) {
                if (x != y)
                    return false;
            } else if (!x.equals(y)) {
                return false;
            }
        }
        return true;
    }

    private void mergeInto(final Map<String, Set<String>> target,
                           final Map<String, Set<String>> add) {
        for (final Map.Entry<String, Set<String>> e : add.entrySet()) {
            final String key = e.getKey();
            final Set<String> gained = e.getValue();
            final Set<String> cur = target.get(key);
            if (cur == null || gained == null) {
                target.put(key, TOP);
            } else {
                cur.addAll(gained);
            }
        }
    }

    private Map<String, Set<String>> transfer(
            final CFGNode node, final Map<String, Set<String>> in) {
        final Map<String, Set<String>> out = copyState(in);
        final Element el = node.getElement();
        if (el == null)
            return out;

        final String name = el.getName();

        if ("initDeclarator".equals(name)) {
            final Element decl = firstChildNamed(el, "declarator");
            final String lhs = declaratorId(decl != null ? decl : el);
            Element rhs = null;
            final Element initBlock = firstChildNamed(el, "initializer");
            if (initBlock != null && !initBlock.elements().isEmpty())
                rhs = (Element) initBlock.elements().get(0);
            if (rhs == null)
                rhs = (Element) el.selectSingleNode("initializer/*[1]");
            if (lhs != null)
                out.put(lhs, evalPointer(rhs, out));
            return out;
        }

        if ("assignExpression".equals(name)) {
            applyAssignment(el, out);
            return out;
        }

        if ("postfixExpression".equals(name)
                || "prefixExpression".equals(name)) {
            // ++/-- on a pointer is pointer arithmetic but the may-points-to
            // object set stays the same; nothing to update.
            return out;
        }

        if ("functionCall".equals(name)) {
            havocEscapedThroughCall(el, out);
            return out;
        }

        // Conditions or other expression nodes may contain nested
        // assignments/calls in exotic code; handle those conservatively.
        @SuppressWarnings("unchecked")
        final List<Element> assigns = el.selectNodes(".//assignExpression");
        for (final Element a : assigns)
            applyAssignment(a, out);
        @SuppressWarnings("unchecked")
        final List<Element> calls = el.selectNodes(".//functionCall");
        for (final Element c : calls)
            havocEscapedThroughCall(c, out);

        return out;
    }

    private void applyAssignment(final Element assign,
                                 final Map<String, Set<String>> state) {
        if (assign.elements().size() < 2)
            return;
        final Element lhs = (Element) assign.elements().get(0);
        final Element rhs = (Element) assign.elements().get(1);
        final String op = assign.attributeValue("op");

        if ("id".equals(lhs.getName())) {
            final String lhsName = lhs.getTextTrim();
            if (op != null) {
                // compound assignment: +=, -=, ... pointer result keeps the
                // same origin but becomes top for exact offset reasoning.
                final Set<String> cur = state.get(lhsName);
                state.put(lhsName, cur == null ? TOP : new HashSet<String>(cur));
            } else {
                state.put(lhsName, evalPointer(rhs, state));
            }
        } else if ("derefExpression".equals(lhs.getName())
                || "arrayAccess".equals(lhs.getName())) {
            // Writes through a pointer / array cell do not rebind the
            // pointer itself.  They may modify address-taken scalars only in
            // ways the scalar domain does not track; leave bindings intact.
        } else {
            // Unknown LHS shape: conservatively havoc any root id found in it.
            final String root = rootId(lhs);
            if (root != null)
                state.put(root, TOP);
        }
    }

    private void havocEscapedThroughCall(final Element call,
                                         final Map<String, Set<String>> state) {
        // Arguments of the form &x let the callee write x.
        @SuppressWarnings("unchecked")
        final List<Element> addrs = call.selectNodes(".//addrExpression/id");
        for (final Element id : addrs)
            state.put(id.getTextTrim(), TOP);
    }

    private Set<String> evalPointer(final Element e,
                                    final Map<String, Set<String>> state) {
        if (e == null)
            return TOP;

        final String name = e.getName();

        if ("id".equals(name)) {
            final String id = e.getTextTrim();
            if (arrays.contains(id))
                return singleton(id);
            final Set<String> cur = state.get(id);
            return cur == null ? TOP : new HashSet<String>(cur);
        }

        if ("addrExpression".equals(name)) {
            final Element addressed = e.elements().isEmpty()
                    ? null : (Element) e.elements().get(0);
            if (addressed == null)
                return TOP;
            if ("id".equals(addressed.getName()))
                return singleton(addressed.getTextTrim());
            if ("arrayAccess".equals(addressed.getName())) {
                final String root = rootId(addressed);
                return root == null ? TOP : singleton(root);
            }
            return TOP;
        }

        if ("castExpression".equals(name)) {
            final Element inner = e.elements().size() >= 2
                    ? (Element) e.elements().get(1)
                    : (e.elements().isEmpty() ? null
                            : (Element) e.elements().get(0));
            return evalPointer(inner, state);
        }

        if ("binaryExpression".equals(name)) {
            Set<String> union = new HashSet<String>();
            boolean top = false;
            for (final Object child : e.elements()) {
                if (!(child instanceof Element))
                    continue;
                final Set<String> v = evalPointer((Element) child, state);
                if (v == null) {
                    top = true;
                } else {
                    union.addAll(v);
                }
            }
            // Integer operands evaluate to empty sets; pointer operands
            // contribute their objects.  A top operand only matters if no
            // pointer operand contributed anything.
            if (union.isEmpty() && top)
                return TOP;
            return union;
        }

        if ("conditionalExpression".equals(name)) {
            final Set<String> union = new HashSet<String>();
            boolean top = false;
            for (final Object child : e.elements()) {
                if (!(child instanceof Element))
                    continue;
                final Set<String> v = evalPointer((Element) child, state);
                if (v == null)
                    top = true;
                else
                    union.addAll(v);
            }
            if (top && union.isEmpty())
                return TOP;
            return union;
        }

        if ("functionCall".equals(name))
            return TOP;

        if ("initDeclarator".equals(name) || "assignExpression".equals(name))
            return TOP;

        if ("parenExpression".equals(name) && !e.elements().isEmpty())
            return evalPointer((Element) e.elements().get(0), state);

        // intConst, stringConst, sizeof, ... are not pointer origins.
        if ("intConst".equals(name) || "stringConst".equals(name))
            return new HashSet<String>();

        return TOP;
    }

    private Map<String, Set<String>> copyState(
            final Map<String, Set<String>> in) {
        final Map<String, Set<String>> out =
                new HashMap<String, Set<String>>();
        for (final Map.Entry<String, Set<String>> e : in.entrySet()) {
            final Set<String> v = e.getValue();
            out.put(e.getKey(), v == null ? TOP : new HashSet<String>(v));
        }
        return out;
    }

    private static Set<String> singleton(final String s) {
        final Set<String> set = new HashSet<String>();
        set.add(s);
        return set;
    }

    private static Element firstChildNamed(final Element e,
                                           final String name) {
        if (e == null)
            return null;
        for (final Object o : e.elements(name))
            if (o instanceof Element)
                return (Element) o;
        return null;
    }

    private static String declaratorId(final Element declarator) {
        if (declarator == null)
            return null;
        final Element id = firstChildNamed(declarator, "id");
        if (id != null)
            return id.getTextTrim();
        return rootId(declarator);
    }

    /** Leftmost <code>id</code> descendant, used as the root variable. */
    static String rootId(final Element expr) {
        if (expr == null)
            return null;
        if ("id".equals(expr.getName()))
            return expr.getTextTrim();
        @SuppressWarnings("unchecked")
        final List<Element> ids = expr.selectNodes(".//id");
        for (final Element id : ids)
            return id.getTextTrim();
        return null;
    }

    /** All local fixed-size array identifiers mapped to their length. */
    static Map<String, Integer> collectArrayLengths(final CFGHandle cfg) {
        final Map<String, Integer> lengths = new HashMap<String, Integer>();
        final Element root = cfg.getElement();
        if (root == null)
            return lengths;
        @SuppressWarnings("unchecked")
        final List<Element> declarators =
                root.selectNodes(".//declarator[arrayDecl]");
        for (final Element declarator : declarators) {
            final Element id = firstChildNamed(declarator, "id");
            final Element sizeConst =
                    declarator.selectSingleNode("arrayDecl/intConst") != null
                    ? (Element) declarator.selectSingleNode("arrayDecl/intConst")
                    : firstChildNamed(
                            firstChildNamed(declarator, "arrayDecl"),
                            "intConst");
            if (id == null)
                continue;
            Integer length = null;
            if (sizeConst != null) {
                try {
                    length = Integer.decode(sizeConst.getTextTrim().trim());
                } catch (final NumberFormatException ex) {
                    length = null;
                }
            }
            lengths.put(id.getTextTrim(), length);
        }
        return lengths;
    }

    /** Exposes the ordered array-access elements of a function document. */
    static List<Element> arrayAccesses(final CFGHandle cfg) {
        final List<Element> result = new ArrayList<Element>();
        final Element root = cfg.getElement();
        if (root == null)
            return result;
        @SuppressWarnings("unchecked")
        final List<Element> all = root.selectNodes(".//arrayAccess");
        result.addAll(all);
        return result;
    }
}
