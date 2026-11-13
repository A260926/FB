package org.jsoup.parser;

import org.jsoup.Connection;
import org.jsoup.Jsoup;
import org.jsoup.helper.Validate;
import org.jsoup.nodes.Document;
import org.jsoup.nodes.Element;
import org.jsoup.nodes.Node;
import org.jsoup.select.Evaluator;
import org.jsoup.select.EvaluatorInternals;
import org.jsoup.select.NodeTraversor;
import org.jsoup.select.NodeVisitor;
import org.jsoup.select.Selector;
import org.jsoup.select.StreamStability;
import org.jspecify.annotations.Nullable;

import java.io.Closeable;
import java.io.IOException;
import java.io.Reader;
import java.io.StringReader;
import java.io.UncheckedIOException;
import java.util.ArrayDeque;
import java.util.Collections;
import java.util.Deque;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.Iterator;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;
import java.util.NoSuchElementException;
import java.util.Queue;
import java.util.Set;
import java.util.Spliterator;
import java.util.Spliterators;
import java.util.function.Predicate;
import java.util.stream.Stream;
import java.util.stream.StreamSupport;

/**
 A StreamParser provides a progressive parse of its input. As each Element is completed, it is emitted via a Stream or
 Iterator interface. Elements returned will be complete with all their children, and an (empty) next sibling, if
 applicable.
 <p>To conserve memory, you can {@link Node#remove() remove()} Elements (or their children) from the DOM during the
 parse. This provides a mechanism to parse an input document that would otherwise be too large to fit into memory, yet
 still providing a DOM interface to the document and its elements.</p>
 <p>
 Additionally, the parser provides a {@link #selectFirst(String query)} / {@link #selectNext(String query)}, which will
 run the parser until a hit is found, at which point the parse is suspended. It can be resumed via another
 {@code select()} call, or via the {@link #stream()} or {@link #iterator()} methods.
 </p>
 <p>Once the input has been fully read, the input Reader will be closed. Or, if the whole document does not need to be
 read, call {@link #stop()} and {@link #close()}.</p>
 <p>The {@link #document()} method will return the Document being parsed into, which will be only partially complete
 until the input is fully consumed.</p>
 <p>A StreamParser can be reused via a new {@link #parse(Reader, String)}, but is not thread-safe for concurrent inputs.
 New parsers should be used in each thread.</p>
 <p>If created via {@link Connection.Response#streamParser()}, or another Reader that is I/O backed, the iterator and
 stream consumers will throw an {@link java.io.UncheckedIOException} if the underlying Reader errors during read.</p>
 <p>For examples, see the jsoup
 <a href="https://jsoup.org/cookbook/input/streamparser-dom-sax">StreamParser cookbook.</a></p>
 <p>
 Selectors that depend on knowing all siblings (e.g. {@code :last-child}, {@code :last-of-type}, {@code :nth-last-child},
 {@code :only-child} and their negations) cannot be correctly evaluated by the transient {@link #selectNext(String)}
 while streaming, because the parser does not know if a later sibling will appear. Two options handle them:
 {@link #selectNextStable(String)} retains candidates until each parent group closes and emits only finalized matches,
 or run {@link #complete()} first to finish the parse (which is effectively the same as using
 {@code Jsoup.parse(...)} unless you have already removed nodes during streaming).
 </p>
 @since 1.18.1 */
public class StreamParser implements Closeable {
    final private Parser parser;
    final TreeBuilder treeBuilder;
    final private ElementIterator it = new ElementIterator();
    final private StableSelection stable = new StableSelection();
    private boolean legacyConsumed = false; // true once an iterator/stream/transient select API has been used
    @Nullable private Document document;
    private boolean stopped = true;

    /**
     Construct a new StreamParser, using the supplied base Parser.
     The supplied parser provides configuration and collects parse errors; streaming uses an independent tree builder.
     @param parser the configured base parser
     */
    public StreamParser(Parser parser) {
        this.parser = parser;
        treeBuilder = parser.getTreeBuilder().newInstance();
        treeBuilder.nodeListener(new NodeVisitor() {
            @Override public void head(Node node, int depth) {
                it.head(node, depth);
                stable.head(node, depth);
            }
            @Override public void tail(Node node, int depth) {
                it.tail(node, depth);
                stable.tail(node, depth);
            }
        });
    }

    /**
     Provide the input for a Document parse. The input is not read until a consuming operation is called.
     @param input the input to be read.
     @param baseUri the URL of this input, for absolute link resolution
     @return this parser, for chaining
     */
    public StreamParser parse(Reader input, String baseUri) {
        close(); // probably a no-op, but ensures any previous reader is closed
        it.reset();
        stable.reset();
        legacyConsumed = false;
        treeBuilder.initialiseParse(input, baseUri, parser); // reader is not read, so no chance of IO error
        document = treeBuilder.doc;
        return this;
    }

    /**
     Provide the input for a Document parse. The input is not read until a consuming operation is called.
     @param input the input to be read
     @param baseUri the URL of this input, for absolute link resolution
     @return this parser
     */
    public StreamParser parse(String input, String baseUri) {
        return parse(new StringReader(input), baseUri);
    }

    /**
     Provide the input for a fragment parse. The input is not read until a consuming operation is called.
     @param input the input to be read
     @param context the optional fragment context element
     @param baseUri the URL of this input, for absolute link resolution
     @return this parser
     @see #completeFragment()
     */
    public StreamParser parseFragment(Reader input, @Nullable Element context, String baseUri) {
        parse(input, baseUri);
        treeBuilder.initialiseParseFragment(context);
        return this;
    }

    /**
     Provide the input for a fragment parse. The input is not read until a consuming operation is called.
     @param input the input to be read
     @param context the optional fragment context element
     @param baseUri the URL of this input, for absolute link resolution
     @return this parser
     @see #completeFragment()
     */
    public StreamParser parseFragment(String input, @Nullable Element context, String baseUri) {
        return parseFragment(new StringReader(input), context, baseUri);
    }

    /**
     Creates a {@link Stream} of {@link Element}s, with the input being parsed as each element is consumed. Each
     Element returned will be complete (that is, all of its children will be included, and if it has a next sibling, that
     (empty) sibling will exist at {@link Element#nextElementSibling()}). The stream will be emitted in document order as
     each element is closed. That means that child elements will be returned prior to their parents.
     <p>The stream will start from the current position of the backing iterator and the parse.</p>
     <p>When consuming the stream, if the Reader that the Parser is reading throws an I/O exception (for example a
     SocketTimeoutException), that will be emitted as an {@link UncheckedIOException}</p>
     @return a stream of Element objects
     @throws UncheckedIOException if the underlying Reader excepts during a read (in stream consuming methods)
     */
    public Stream<Element> stream() {
        guardLegacyConsumer("stream()");
        return StreamSupport.stream(
            Spliterators.spliteratorUnknownSize(
                it, Spliterator.DISTINCT | Spliterator.NONNULL | Spliterator.ORDERED),
            false);
    }

    /**
     Returns an {@link Iterator} of {@link Element}s, with the input being parsed as each element is consumed. Each
     Element returned will be complete (that is, all of its children will be included, and if it has a next sibling, that
     (empty) sibling will exist at {@link Element#nextElementSibling()}). The elements will be emitted in document order as
     each element is closed. That means that child elements will be returned prior to their parents.
     <p>The iterator will start from the current position of the parse.</p>
     <p>The iterator is backed by this StreamParser, and the resources it holds.</p>
     @return a stream of Element objects
     */
    public Iterator<Element> iterator() {
        guardLegacyConsumer("iterator()");
        //noinspection ReturnOfInnerClass
        return it;
    }

    /** Blocks the legacy streaming entry points once a stable query owns this parse. */
    private void guardLegacyConsumer(String operation) {
        if (stable.isActive())
            throw new IllegalStateException(
                "Cannot use " + operation + " while a selectNextStable() query is active on this StreamParser.");
        legacyConsumed = true;
    }

    /**
     Flags that the parse should be stopped; the backing iterator will not return any more Elements.
     @return this parser
     */
    public StreamParser stop() {
        stopped = true;
        return this;
    }

    /**
     Closes the input and releases resources including the underlying parser and reader.
     <p>The parser will also be closed when the input is fully read.</p>
     <p>The parser can be reused with another call to {@link #parse(Reader, String)}.</p>
    */
    @Override public void close() {
        it.deferOpenElements();
        stopped = true;
        treeBuilder.closeParse(); // closes the reader, frees resources
    }

    /**
     Get the current {@link Document} as it is being parsed. It will be only partially complete until the input is fully
     read. Structural changes (e.g. insert, remove) may be made to the Document contents.
     @return the (partial) Document
     */
    public Document document() {
        Validate.notNull(document, "Must run parse() before calling.");
        return document;
    }

    /**
     Runs the parser until the input is fully read, and returns the completed Document.
     @return the completed Document
     @throws IOException if an I/O error occurs
     */
    public Document complete() throws IOException {
        Document doc = document();
        treeBuilder.runParser();
        return doc;
    }

    /**
     When initialized as a fragment parse, runs the parser until the input is fully read, and returns the completed
     fragment child nodes.
     @return the completed child nodes
     @throws IOException if an I/O error occurs
     @see #parseFragment(Reader, Element, String)
     */
    public List<Node> completeFragment() throws IOException {
        treeBuilder.runParser();
        return treeBuilder.completeParseFragment();
    }

    /**
     Finds the first Element that matches the provided query. If the parsed Document does not already have a match, the
     input will be parsed until the first match is ready for stream emission, or the input is completely read.
     @param query the {@link org.jsoup.select.Selector} query.
     @return the first matching {@link Element}, or {@code null} if there's no match
     @throws IOException if an I/O error occurs
     @see #selectFirst(Evaluator)
     */
    public @Nullable Element selectFirst(String query) throws IOException {
        guardLegacyConsumer("selectFirst(String)");
        return selectFirst(Selector.evaluatorOf(query));
    }

    /**
     Just like {@link #selectFirst(String)}, but if there is no match, throws an {@link IllegalArgumentException}. This
     is useful if you want to simply abort processing on a failed match.
     @param query the {@link org.jsoup.select.Selector} query.
     @return the first matching element
     @throws IllegalArgumentException if no match is found
     @throws IOException if an I/O error occurs
     */
    public Element expectFirst(String query) throws IOException {
        return Validate.expectNotNull(
            selectFirst(query),
            "No elements matched the query '%s' in the document."
            , query
        );
    }

    /**
     Finds the first Element that matches the provided query. If the parsed Document does not already have a match, the
     input will be parsed until the first match is ready for stream emission, or the input is completely read.
     <p>By providing a compiled evaluator vs. a CSS selector, this method may be more efficient when executing the same
     query against multiple documents.</p>
     @param eval the {@link org.jsoup.select.Selector} evaluator.
     @return the first matching {@link Element}, or {@code null} if there's no match
     @throws IOException if an I/O error occurs
     @see Selector#evaluatorOf(String css)
     */
    public @Nullable Element selectFirst(Evaluator eval) throws IOException {
        guardLegacyConsumer("selectFirst(Evaluator)");
        final Document doc = document();

        // a ready match can be returned without advancing the iterator
        Element first = doc.selectFirst(eval);
        if (first != null && it.isPending(first)) {
            if (!it.awaitReady(first)) return null;
            first = doc.selectFirst(eval);
        }
        if (first != null && !it.isPending(first)) return first;

        // use the iterator for linear progress
        Element emitted = selectNext(eval);
        if (emitted == null) {
            if (!treeBuilder.isComplete()) return null;
            return doc.selectFirst(eval);
        }

        // reconcile the iterator's child-first emission order with document order
        while ((first = doc.selectFirst(eval)) != null && it.isPending(first)) {
            if (!it.awaitReady(first)) return null;
        }
        return first != null ? first : emitted;
    }

    /**
     Finds the next Element that matches the provided query. The input will be parsed until the next match is found, or
     the input is completely read.
     @param query the {@link org.jsoup.select.Selector} query.
     @return the next matching {@link Element}, or {@code null} if there's no match
     @throws IOException if an I/O error occurs
     @see #selectNext(Evaluator)
     */
    public @Nullable Element selectNext(String query) throws IOException {
        guardLegacyConsumer("selectNext(String)");
        return selectNext(Selector.evaluatorOf(query));
    }

    /**
     Just like {@link #selectFirst(String)}, but if there is no match, throws an {@link IllegalArgumentException}. This
     is useful if you want to simply abort processing on a failed match.
     @param query the {@link org.jsoup.select.Selector} query.
     @return the first matching element
     @throws IllegalArgumentException if no match is found
     @throws IOException if an I/O error occurs
     */
    public Element expectNext(String query) throws IOException {
        return Validate.expectNotNull(
            selectNext(query),
            "No elements matched the query '%s' in the document."
            , query
        );
    }

    /**
     Finds the next Element that matches the provided query. The input will be parsed until the next match is found, or
     the input is completely read.
     <p>By providing a compiled evaluator vs a CSS selector, this method may be more efficient when executing the same
     query against multiple documents.</p>
     @param eval the {@link org.jsoup.select.Selector} evaluator.
     @return the next matching {@link Element}, or {@code null} if there's no match
     @throws IOException if an I/O error occurs
     @see Selector#evaluatorOf(String css)
     */
    public @Nullable Element selectNext(Evaluator eval) throws IOException {
        guardLegacyConsumer("selectNext(Evaluator)");
        try {
            final Document doc = document(); // validates the parse was initialized, keeps stack trace out of stream
            return EvaluatorInternals.stream(eval, doc, stream())
                .findFirst()
                .orElse(null);
        } catch (UncheckedIOException e) {
            // Reader threw an IO exception emitted via Iterator's next()
            throw e.getCause();
        }
    }

    /**
     Finds the next Element that matches the provided query, but only returns matches whose result is final.
     <p>Unlike {@link #selectNext(String)}, a candidate that merely matches the provisional tree is not emitted yet:
     position-dependent pseudo-classes such as {@code :nth-last-child}, {@code :nth-last-of-type}, {@code :only-child}
     and {@code :last-child} can change verdict as more siblings arrive. The candidate is retained and re-evaluated when
     its parent's child set closes (the real tree-builder close notification), so every emitted Element has exactly the
     match result it would have after a full {@link Jsoup#parse parse} of the same input. A match is therefore never
     withdrawn, and a temporarily non-matching candidate is never discarded.</p>
     <p>For selectors whose dependencies are confined to the candidate and its sibling set &mdash; tag, id, class,
     attribute and ordinary positional conditions, the {@code nth-last-*}, {@code only-*} pseudo-classes, plus nested
     {@code :is()}, {@code :where()} and {@code :not()} and comma groups without relationship combinators &mdash; each
     parent group is resolved as soon as that parent closes, so the first list can be delivered while the second list is
     still unread. Selectors whose Evaluator genuinely depends on nodes that may appear later in the document (for
     example {@code :has()} on an ancestor, or descendant/sibling combinators) conservatively wait until the input is
     fully parsed; they still return the same results as a complete parse, just less eagerly.</p>
     <p>Within one {@link #parse parse}, the query is fixed: repeat calls keep consuming the same candidate stream, and
     each Element is delivered at most once, in the parser's child-before-parent completion order. Switching to a
     different query, or mixing this method with {@link #selectNext}, {@link #selectFirst}, {@link #iterator()} or
     {@link #stream()}, throws {@link IllegalStateException} without reading further input. {@link #complete()} may be
     called to finish the input, after which further calls with the same stable query deliver any remaining matches.
     {@link #stop()} / {@link #close()} do not treat still-open parent groups as a completed document.</p>
     @param query the {@link org.jsoup.select.Selector} query, held constant for the rest of this parse
     @return the next stable matching {@link Element}, or {@code null} if the input is exhausted with no further match
     @throws IOException if an I/O error occurs; the original cause is preserved
     @throws IllegalStateException if the query changes, or a legacy streaming consumer is used on the same parse
     @since 1.24.2
     */
    public @Nullable Element selectNextStable(String query) throws IOException {
        return selectNextStable(Selector.evaluatorOf(query), query);
    }

    /**
     Stable variant of {@link #selectNextStable(String)} accepting a pre-compiled {@link Evaluator}. The query is
     still fixed for the parse: a later call (string or evaluator overload) with a different query throws
     {@link IllegalStateException}.
     @param eval the {@link org.jsoup.select.Selector} evaluator, held constant for the rest of this parse
     @return the next stable matching {@link Element}, or {@code null} if the input is exhausted with no further match
     @throws IOException if an I/O error occurs; the original cause is preserved
     @throws IllegalStateException if the query changes, or a legacy streaming consumer is used on the same parse
     @see #selectNextStable(String)
     @since 1.24.2
     */
    public @Nullable Element selectNextStable(Evaluator eval) throws IOException {
        return selectNextStable(eval, eval.toString());
    }

    private @Nullable Element selectNextStable(Evaluator eval, String queryKey) throws IOException {
        if (legacyConsumed)
            throw new IllegalStateException(
                "Cannot start selectNextStable() after selectNext(), selectFirst(), iterator() or stream() was used.");
        stable.bind(eval, queryKey);
        try {
            return stable.nextMatch();
        } catch (UncheckedIOException e) {
            // preserve the checked I/O contract and the original cause from the backing Reader
            throw e.getCause();
        }
    }

    final class ElementIterator implements Iterator<Element>, NodeVisitor {
        final private Queue<Element> emitQueue = new LinkedList<>(); // ready elements waiting for iterator delivery, in emission order
        final private HashSet<Element> deferredEls = new HashSet<>(); // off-stack elements not yet ready for emission
        private @Nullable Element current;  // most recently emitted
        private @Nullable Element next;     // element waiting to be picked up
        private @Nullable Element tail;     // The last tailed element (</html>), on hold for final pop

        void reset() {
            emitQueue.clear();
            deferredEls.clear();
            current = next = tail = null;
            stopped = false;
        }

        /** Defers open Elements before an early close releases the tree-builder stack. */
        void deferOpenElements() {
            treeBuilder.copyOpenElementsTo(deferredEls);
        }

        /** Tests whether an Element has reached its iterator emission point. */
        boolean isReady(Element element) {
            return current == element || next == element || emitQueue.contains(element);
        }

        /** Tests whether an Element still needs parser advancement before selection. */
        boolean isPending(Element element) {
            // the document is pending until EOF; other pending elements are open or explicitly deferred
            return !treeBuilder.isComplete() &&
                (element == treeBuilder.doc || treeBuilder.isOpen(element) || deferredEls.contains(element));
        }

        /** Advances parsing until an Element is ready; returns false if parsing was stopped. */
        boolean awaitReady(Element element) throws IOException {
            try {
                while (isPending(element) && !stopped) {
                    if (!hasNext()) break;
                    if (isPending(element)) next(); // hasNext may have buffered the target itself
                }
                return !isPending(element);
            } catch (UncheckedIOException e) {
                // preserve the checked I/O contract when iterator advancement reads input
                throw e.getCause();
            }
        }

        /** Marks an Element ready and queues it for iterator emission. */
        void queueForEmission(Element element) {
            deferredEls.remove(element);
            emitQueue.add(element);
        }

        // Iterator Interface:
        /**
         {@inheritDoc}
         @throws UncheckedIOException if the underlying Reader errors during a read
         */
        @Override public boolean hasNext() {
            maybeFindNext();
            return next != null;
        }

        /**
         {@inheritDoc}
         @throws UncheckedIOException if the underlying Reader errors during a read
         */
        @Override public Element next() {
            maybeFindNext();
            if (next == null) throw new NoSuchElementException();
            current = next;
            next = null;
            return current;
        }

        private void maybeFindNext() {
            if (stopped || next != null) return;

            // drain the current queue before stepping to get more
            if (!emitQueue.isEmpty()) {
                next = emitQueue.remove();
                return;
            }

            // step the parser, which will hit the node listeners to add to the queue:
            while (treeBuilder.stepParser()) {
                if (!emitQueue.isEmpty()) {
                    next = emitQueue.remove();
                    return;
                }
            }
            close();

            // send the final element out:
            if (tail != null) {
                deferredEls.remove(tail);
                next = tail;
                tail = null;
            }
        }

        @Override public void remove() {
            if (current == null) throw new NoSuchElementException();
            current.remove();
        }

        // NodeVisitor Interface:
        @Override public void head(Node node, int depth) {
            if (node instanceof Element) {
                Element prev = node.previousElementSibling();
                // We prefer to wait until an element has a next sibling before emitting it; otherwise, get it in tail
                if (prev != null) {
                    queueForEmission(prev);
                    if (tail == prev) tail = null; // queueing marks it ready
                }
            }
        }

        @Override public void tail(Node node, int depth) {
            if (node instanceof Element) {
                tail = (Element) node; // kept for final hit
                if (!isReady(tail)) deferredEls.add(tail);
                Element lastChild = tail.lastElementChild(); // won't get a nextsib, so emit that:
                if (lastChild != null) queueForEmission(lastChild);
                if (tail == treeBuilder.doc) deferredEls.clear(); // completed document makes stale recovery state irrelevant
            }
        }
    }

    /**
     Holds the state of a {@link #selectNextStable} query: retained candidates, finalized matches, and the fixed
     evaluator. It listens to the same tree-builder close notifications as the element iterator, so retention is tied
     to real element closes rather than token text.
     */
    final class StableSelection implements NodeVisitor {
        private @Nullable String queryKey;
        private @Nullable Predicate<Element> predicate;
        private boolean frontierSafe; // verdicts are fixed once each sibling set closes
        private boolean active;      // a stable query owns this parse
        private boolean eofReached;

        // finalized matches waiting to be handed back, in element completion order (children before parents)
        private final Queue<Element> ready = new LinkedList<>();
        // every Element whose stable verdict has already been settled, so it is evaluated and enqueued at most once
        private final Set<Element> settled = Collections.newSetFromMap(new IdentityHashMap<>());
        // frontier mode: candidates retained until their DOM parent's child set is complete
        private final Map<Element, Deque<Element>> pendingByParent = new IdentityHashMap<>();

        void reset() {
            queryKey = null;
            predicate = null;
            frontierSafe = false;
            active = false;
            eofReached = false;
            ready.clear();
            settled.clear();
            pendingByParent.clear();
        }

        boolean isActive() {
            return active;
        }

        /** Fixes the query for this parse, or verifies a repeat call uses the same one. */
        void bind(Evaluator eval, String key) {
            Validate.notNull(eval);
            if (!active) {
                Document doc = document(); // validates that parse() ran
                eval.reset(); // a reused compiled Evaluator must not carry memoized matches from an earlier parse
                queryKey = key;
                frontierSafe = StreamStability.settlesAtParentFrontier(eval);
                predicate = EvaluatorInternals.elementMatchPredicate(eval, doc);
                active = true;
                if (treeBuilder.isComplete())
                    drainCompletedTree(); // bound after complete(): no further close events will arrive
            } else if (!key.equals(queryKey)) {
                throw new IllegalStateException(
                    "selectNextStable() is bound to query '" + queryKey + "' for this parse; cannot switch to '"
                        + key + "'.");
            }
        }

        /** Advances the parse until a finalized match is available or the input can no longer be read. */
        @Nullable Element nextMatch() throws IOException {
            if (!ready.isEmpty()) return ready.remove();
            try {
                while (ready.isEmpty() && !stopped && !treeBuilder.isComplete()) {
                    treeBuilder.stepParser(); // close notifications settle candidates into the ready queue
                }
                if (treeBuilder.isComplete())
                    treeBuilder.closeParse(); // natural EOF releases the reader, as runParser() would
            } catch (UncheckedIOException e) {
                throw e.getCause();
            }
            return ready.peek() == null ? null : ready.remove();
        }

        @Override public void head(Node node, int depth) {
            // candidates are only considered once their element closes
        }

        @Override public void tail(Node node, int depth) {
            if (!active || !(node instanceof Element)) return;
            Element element = (Element) node;

            if (element == treeBuilder.doc) {
                settleAtEof();
                return;
            }
            if (!frontierSafe)
                return; // relationship-dependent queries are resolved wholesale from the completed tree at EOF

            // this element's child set is now complete, so finalize the candidates retained underneath it
            settleParent(element);

            Element parent = element.parent();
            if (parent == null) {
                // a detached element can gain no further siblings without DOM edits (unsupported while consuming),
                // so its verdict is already final
                Predicate<Element> match = predicate;
                if (match != null && settled.add(element) && match.test(element))
                    ready.add(element);
                return;
            }
            pendingByParent.computeIfAbsent(parent, p -> new ArrayDeque<>()).add(element);
            if (!treeBuilder.isOpen(parent))
                settleParent(parent); // the parent's real close notification makes this sibling set final
        }

        /** Finalizes a parent group now that the parent's child set can no longer change. */
        private void settleParent(Element parent) {
            Deque<Element> siblings = pendingByParent.remove(parent);
            if (siblings == null) return;
            Predicate<Element> match = predicate;
            for (Element element : siblings) {
                // every candidate is marked settled (match or not), so the EOF walk neither re-evaluates nor duplicates it
                if (settled.add(element) && match != null && match.test(element))
                    ready.add(element); // children are settled before their parent can enter the queue
            }
        }

        /** EOF makes every retained candidate and the document root decidable. */
        private void settleAtEof() {
            if (eofReached) return;
            eofReached = true;
            Predicate<Element> match = predicate;
            if (match == null) return;

            // any element still retained (implicit EOF closes, or a conservative query) is covered by the
            // completion-order walk; settled makes already-finalized matches stay exactly-once
            pendingByParent.clear();
            collectCompletedMatches(match); // the document root is the walk's final tail (Web mode excludes it)
        }

        /** Used when the stable query starts on an already fully parsed document. */
        private void drainCompletedTree() {
            settleAtEof();
        }

        /** Walks the completed document in child-before-parent order, enqueuing each not-yet-settled match once. */
        private void collectCompletedMatches(Predicate<Element> match) {
            Deque<Element> completion = new ArrayDeque<>();
            NodeTraversor.traverse(new NodeVisitor() {
                @Override public void head(Node node, int depth) { }
                @Override public void tail(Node node, int depth) {
                    if (node instanceof Element) completion.add((Element) node);
                }
            }, treeBuilder.doc);
            while (!completion.isEmpty()) {
                Element element = completion.remove();
                if (settled.add(element) && match.test(element))
                    ready.add(element);
            }
        }
    }
}
