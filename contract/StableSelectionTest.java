import org.jsoup.Jsoup;
import org.jsoup.nodes.Document;
import org.jsoup.nodes.Element;
import org.jsoup.parser.Parser;
import org.jsoup.parser.StreamParser;

import java.io.IOException;
import java.io.Reader;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 Contract tests for {@link StreamParser#selectNextStable(String)}. Default package, plain {@code main}, grouped into
 three printed checkpoints: STABLE_MATCHES_OK, PARENT_FRONTIER_OK, STABLE_REUSE_OK.
 */
public final class StableSelectionTest {
    private StableSelectionTest() {
    }

    public static void main(String[] args) throws Exception {
        stableMatches();
        parentFrontier();
        stableReuse();
    }

    // -------------------------------------------------------------------------------------------------------------
    // Group 1: stable results equal a full parse, including the transient selectNext() counterexample
    // -------------------------------------------------------------------------------------------------------------
    static void stableMatches() throws Exception {
        // 1. the three-item list counterexample from the baseline
        String lists = "<ul id=one><li id=a>A</li><li id=b>B</li><li id=c>C</li></ul>"
            + "<ul id=two><li id=d>D</li><li id=e>E</li><li id=f>F</li></ul>";
        assertIds(lists, "li:nth-last-child(2)", List.of("b", "e"));

        // the legacy streaming API still reports the transient [a, b] on the first list
        List<String> legacy = new ArrayList<>();
        try (StreamParser p = new StreamParser(Parser.htmlParser()).parse(lists, "")) {
            Element next;
            while ((next = p.selectNext("li:nth-last-child(2)")) != null)
                legacy.add(next.id());
        }
        check(legacy.equals(List.of("a", "b", "d", "e")),
            "legacy transient stream keeps its old behavior, got " + legacy);

        // 2. nested logical conditions: :is(...), :not(...), comma groups, compound attribute/id/class conditions
        String logical = "<ul id=log>"
            + "<li id=l1 class=hot data-skip=1>1</li>"
            + "<li id=l2 class=hot>2</li>"
            + "<li id=l3>3</li>"
            + "<li id=l4>4</li>"
            + "</ul>"
            + "<div id=mix><p id=p1>p1</p><!-- a comment --> plain text "
            + "<span id=s1>s1</span><p id=p2>p2</p><em id=e1>e1</em></div>"
            + "<div id=only><span id=sole>sole</span></div>";
        assertIds(logical,
            "li:is(.hot, :nth-last-child(2)):not([data-skip]), p:nth-last-of-type(1)",
            List.of("l2", "l3", "p2"));
        assertIds(logical, "li#l3[data-skip]:nth-last-child(2), span:only-child, em:only-of-type",
            List.of("e1", "sole"));
        assertIds(logical, "li:last-child", List.of("l4"));
        assertIds(logical, ":is(ul, div):not(#log)", List.of("mix", "only"));
        assertIds(logical, "li:where(.hot):not([data-skip])", List.of("l2"));
        assertIds(logical, ":where(span, em):only-of-type", List.of("s1", "e1", "sole"));

        // 3. mixed element types, negative nth coefficients, and text/comments not occupying element positions
        String mixed = "<ul id=nth>"
            + "<li id=n1>1</li> <!-- comment --> whitespace text"
            + "<li id=n2>2</li><li id=n3>3</li><li id=n4>4</li>"
            + "</ul>"
            + "<div id=types><p id=t1>t1</p><span id=t2>t2</span><p id=t3>t3</p><i id=t4>t4</i></div>";
        assertIds(mixed, "li:nth-last-child(-n+2)", List.of("n3", "n4"));
        assertIds(mixed, "li:nth-last-child(2)", List.of("n3"));
        assertIds(mixed, "p:nth-last-of-type(1)", List.of("t3"));
        assertIds(mixed, "i:only-of-type", List.of("t4"));

        // 4. multiple independent parent groups settle separately, in element completion order
        assertIds(lists, "li:only-child, li:nth-last-of-type(1)", List.of("c", "f"));

        // 5. complex relationship CSS takes the conservative EOF path but keeps the full-parse answer
        String relational = "<section id=sec><div id=d1><p id=rp1>rp1</p></div>"
            + "<div id=d2><p id=rp2>rp2</p><p id=rp3>rp3</p></div></section>";
        assertIds(relational, "div:has(p)", List.of("d1", "d2"));
        assertIds(relational, "section p", List.of("rp1", "rp2", "rp3"));
        assertIds(relational, "p + p", List.of("rp3"));
        assertIds(relational, "div:has(> p:first-child)", List.of("d1", "d2"));

        // 6. continue fetching after complete() drains the rest of the same fixed query
        List<String> afterComplete = new ArrayList<>();
        try (StreamParser p = new StreamParser(Parser.htmlParser()).parse(lists, "")) {
            Element first = p.selectNextStable("li:nth-last-child(2)");
            check(first != null && first.id().equals("b"), "first stable match is b");
            p.complete();
            afterComplete.addAll(idsOf(drainStable(p, "li:nth-last-child(2)")));
        }
        check(afterComplete.equals(List.of("e")), "matches after complete() continue the same query, got "
            + afterComplete);

        System.out.println("STABLE_MATCHES_OK");
    }

    // -------------------------------------------------------------------------------------------------------------
    // Group 2: parent groups are delivered at their close frontier, never waiting for document EOF
    // -------------------------------------------------------------------------------------------------------------
    static void parentFrontier() throws Exception {
        String firstList = "<ul id=one><li id=a>A</li><li id=b>B</li><li id=c>C</li></ul>";
        String tailMarker = "<li id=tail-late></li><li>last</li>";
        String secondList = "<ul id=two>" + repeat("<li>padding</li>", 9000) + tailMarker + "</ul>";
        String html = firstList + secondList;

        CountingReader reader = new CountingReader(html);
        try (StreamParser p = new StreamParser(Parser.htmlParser()).parse(reader, "")) {
            Element hit = p.selectNextStable("li:nth-last-child(2)");

            // the first list closes before the second list's body is consumed; only CharacterReader prefetch may run ahead
            check(hit != null && hit.id().equals("b"), "first parent group yields b at its close frontier");
            check(hit == p.document().getElementById("b"), "stable hit is the live source node");
            check(p.document().getElementById("tail-late") == null,
                "the still-unread second list tail must not exist in the DOM yet");
            long slack = (long) 64 * 1024;
            check(reader.served <= firstList.length() + slack,
                "first group delivered without reading the second list body (served=" + reader.served
                    + ", firstList=" + firstList.length() + ")");
            check(reader.served < html.length() - slack,
                "delivery happened well before document EOF (served=" + reader.served + ", total=" + html.length()
                    + ")");

            // the second group resolves in turn; overall answer still matches the full tree
            List<Element> rest = drainStable(p, "li:nth-last-child(2)");
            check(idsOf(rest).equals(List.of("tail-late")), "second parent group settles on its own, got "
                + idsOf(rest));
        }

        // the conservative path genuinely waits for EOF even when an early element already matches
        String relationalHead = "<div id=has-div><p id=has-p>x</p></div>";
        String relationalHtml = relationalHead + repeat("<span>pad</span>", 9000);
        CountingReader relationalReader = new CountingReader(relationalHtml);
        try (StreamParser p = new StreamParser(Parser.htmlParser()).parse(relationalReader, "")) {
            Element hit = p.selectNextStable("div:has(p)");
            check(hit != null && hit.id().equals("has-div"), "conservative query still returns the full-parse match");
            check(relationalReader.served >= relationalHtml.length(),
                "relationship-dependent query waits until EOF (served=" + relationalReader.served + ")");
        }

        // stop()/close() must not synthesize an EOF: open parent groups stay undelivered
        String openSecond = "<ul id=one><li id=a>A</li><li id=b>B</li><li id=c>C</li></ul>"
            + "<ul id=two><li id=d>D</li><li id=e>E</li>";
        try (StreamParser p = new StreamParser(Parser.htmlParser()).parse(openSecond, "")) {
            Element b = p.selectNextStable("li:nth-last-child(2)");
            check(b != null && b.id().equals("b"), "closed first group is delivered before stop()");
            p.stop();
            check(p.selectNextStable("li:nth-last-child(2)") == null,
                "a stopped parse exposes no fabricated matches from open groups");
            p.close();
            check(p.selectNextStable("li:nth-last-child(2)") == null,
                "a closed parse exposes no fabricated matches from open groups");
        }

        System.out.println("PARENT_FRONTIER_OK");
    }

    // -------------------------------------------------------------------------------------------------------------
    // Group 3: query fixation, reuse rules, failure handling, and clean state across parses
    // -------------------------------------------------------------------------------------------------------------
    static void stableReuse() throws Exception {
        String html = "<ul id=one><li id=a>A</li><li id=b>B</li><li id=c>C</li></ul>"
            + "<ul id=two><li id=d>D</li><li id=e>E</li><li id=f>F</li></ul>";

        // fixed query: switching query throws IllegalStateException without advancing the reader
        CountingReader reader = new CountingReader(html);
        try (StreamParser p = new StreamParser(Parser.htmlParser()).parse(reader, "")) {
            Element b = p.selectNextStable("li:nth-last-child(2)");
            check(b != null && b.id().equals("b"), "initial stable match delivered");
            long servedBefore = reader.served;
            expectIllegalState(() -> p.selectNextStable("li:last-child"), "changed stable query");
            check(reader.served == servedBefore, "rejected query switch must not read more input");
            // the original fixed query still progresses normally
            Element e = p.selectNextStable("li:nth-last-child(2)");
            check(e != null && e.id().equals("e"), "original query remains usable after a rejected switch");

            // legacy consumers are refused while the stable query owns the parse
            expectIllegalState(() -> p.selectNext("li"), "selectNext mixing");
            expectIllegalState(() -> p.selectFirst("li"), "selectFirst mixing");
            expectIllegalState(p::iterator, "iterator mixing");
            expectIllegalState(p::stream, "stream mixing");
        }

        // stable cannot start after a legacy consumer already advanced the parse
        try (StreamParser p = new StreamParser(Parser.htmlParser()).parse(html, "")) {
            p.selectNext("li");
            expectIllegalState(() -> p.selectNextStable("li:last-child"), "stable after legacy selectNext");
        }

        // a failing Reader surfaces the checked IOException with its original cause, then the parser can be reused
        IOException failure = new IOException("boom");
        // CharacterReader fills its 2 KiB buffer at construction in a loop, so the initial content must be exactly
        // one buffer: parse() then succeeds, and the failure surfaces on refill while the stable selection is still
        // waiting for the open <ul> to close
        int bufferSize = 2048;
        String head = "<ul id=one><li id=a>A</li><li id=b>B</li>";
        String initial = head + repeat(" ", bufferSize - head.length());
        check(initial.length() == bufferSize, "the failing-reader prefix must match CharacterReader's buffer size");
        try (StreamParser p = new StreamParser(Parser.htmlParser())) {
            p.parse(new FailingReader(initial, failure), "");
            IOException thrown = null;
            try {
                p.selectNextStable("li:nth-last-child(2)");
            } catch (IOException actual) {
                thrown = actual;
            }
            check(thrown == failure, "the Reader's original IOException cause is preserved");

            // re-parse clears candidates, query binding and delivered records from the failed input
            List<Element> reused = drainStable(p.parse(html, ""), "li:last-child");
            check(idsOf(reused).equals(List.of("c", "f")),
                "fresh parse after a Reader failure works with a new query, got " + idsOf(reused));
            Document doc = p.document();
            check(doc.getElementById("a") != null && doc.select("ul").size() == 2,
                "the reused parser holds only the new input");
        }

        // re-parsing with the same query resets delivered state: every match of the new input is returned once
        try (StreamParser p = new StreamParser(Parser.htmlParser())) {
            List<Element> first = drainStable(p.parse(html, ""), "li:nth-last-child(2)");
            check(idsOf(first).equals(List.of("b", "e")), "first parse baseline");
            String other = "<ul><li id=x>X</li><li id=y>Y</li><li id=z>Z</li></ul>";
            List<Element> second = drainStable(p.parse(other, ""), "li:nth-last-child(2)");
            check(idsOf(second).equals(List.of("y")), "re-parse delivers only the new input's matches");
            check(new HashSet<>(second).size() == second.size(), "each element is delivered at most once per parse");
        }

        System.out.println("STABLE_REUSE_OK");
    }

    // -------------------------------------------------------------------------------------------------------------
    // helpers
    // -------------------------------------------------------------------------------------------------------------
    private static List<Element> drainStable(StreamParser parser, String query) throws IOException {
        List<Element> matches = new ArrayList<>();
        Element next;
        while ((next = parser.selectNextStable(query)) != null)
            matches.add(next);
        return matches;
    }

    private static List<String> idsOf(List<Element> elements) {
        List<String> ids = new ArrayList<>(elements.size());
        for (Element element : elements)
            ids.add(element.id());
        return ids;
    }

    /** Compares the stable stream against a full Jsoup parse: ID sequence, source-node identity, and uniqueness. */
    private static void assertIds(String html, String query, List<String> expected) throws IOException {
        Document full = Jsoup.parse(html);
        List<String> fullIds = full.select(query).eachAttr("id");
        check(fullIds.equals(expected), query + " full-parse expectation " + expected + " but was " + fullIds);

        try (StreamParser parser = new StreamParser(Parser.htmlParser()).parse(html, "")) {
            List<Element> matches = drainStable(parser, query);
            List<String> streamedIds = idsOf(matches);
            check(streamedIds.equals(expected),
                query + " stable stream expected " + expected + " but was " + streamedIds);

            Set<Element> unique = new HashSet<>(matches);
            check(unique.size() == matches.size(), query + " delivered an element more than once");
            for (Element element : matches) {
                check(element == parser.document().getElementById(element.id()),
                    query + " returned a node that is not the live source element: " + element.id());
            }
        }
    }

    private static void expectIllegalState(ThrowingRunnable runnable, String label) {
        boolean thrown = false;
        try {
            runnable.run();
        } catch (IllegalStateException expected) {
            thrown = true;
        } catch (Exception unexpected) {
            throw new AssertionError(label + " threw " + unexpected.getClass().getSimpleName()
                + " instead of IllegalStateException");
        }
        check(thrown, label + " did not throw IllegalStateException");
    }

    private static String repeat(String value, int times) {
        StringBuilder builder = new StringBuilder(value.length() * times);
        for (int i = 0; i < times; i++)
            builder.append(value);
        return builder.toString();
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    @FunctionalInterface
    interface ThrowingRunnable {
        void run() throws Exception;
    }

    /** Reader that counts every character handed to the parser's CharacterReader. */
    static final class CountingReader extends Reader {
        private final String data;
        private int position;
        long served;

        CountingReader(String data) {
            this.data = data;
        }

        @Override public int read(char[] buffer, int offset, int length) {
            if (position >= data.length()) return -1;
            int count = Math.min(length, data.length() - position);
            data.getChars(position, position + count, buffer, offset);
            position += count;
            served += count;
            return count;
        }

        @Override public void close() {
        }
    }

    /** Reader that serves a prefix once, then fails every subsequent read with the supplied checked exception. */
    static final class FailingReader extends Reader {
        private final String prefix;
        private final IOException failure;
        private int position;

        FailingReader(String prefix, IOException failure) {
            this.prefix = prefix;
            this.failure = failure;
        }

        @Override public int read(char[] buffer, int offset, int length) throws IOException {
            if (position >= prefix.length()) throw failure;
            int count = Math.min(length, prefix.length() - position);
            prefix.getChars(position, position + count, buffer, offset);
            position += count;
            return count;
        }

        @Override public void close() {
        }
    }
}
