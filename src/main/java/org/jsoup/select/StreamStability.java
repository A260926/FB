package org.jsoup.select;

import java.util.ArrayDeque;
import java.util.Deque;

/**
 Decides whether an {@link Evaluator} can be resolved element by element as each parent's child set closes, or
 whether it depends on nodes that may still be unparsed and so must be held back until the document is complete.
 <p>The check follows the actual Evaluator composition, not the selector text: a selector is frontier-safe only when
 none of its branches use a relationship to another node (descendant, sibling, {@code :has()}, {@code ::node}
 pseudo-elements). Nested {@code :is()}, {@code :where()} and {@code :not()} are inspected recursively, and the
 negation inside {@code :not()} is safe exactly when its inner selector would be safe on its own.</p>
 */
public final class StreamStability {
    private StreamStability() {
    }

    /**
     Tests whether the evaluator's verdict for an element is fixed once the element itself and its sibling set are
     complete.
     @param evaluator the compiled query evaluator
     @return {@code true} if matches can be settled at parent close; {@code false} if the document must finish first
     */
    public static boolean settlesAtParentFrontier(Evaluator evaluator) {
        // cycle-safe: structural evaluators only nest other evaluators, so a revisited node shares its verdict
        Deque<Evaluator> path = new ArrayDeque<>();
        return settles(evaluator, path);
    }

    private static boolean settles(Evaluator evaluator, Deque<Evaluator> path) {
        for (Evaluator seen : path) {
            if (seen == evaluator) return true;
        }
        path.push(evaluator);
        try {
            if (evaluator instanceof CombiningEvaluator) {
                CombiningEvaluator combining = (CombiningEvaluator) evaluator;
                for (int i = 0; i < combining.num; i++) {
                    if (!settles(combining.evaluators.get(i), path)) return false;
                }
                return true;
            }
            if (evaluator instanceof StructuralEvaluator.Not) {
                // :not() only re-checks the candidate itself; the negation is frontier-safe if its inner query is
                return settles(((StructuralEvaluator.Not) evaluator).evaluator, path);
            }
            if (evaluator instanceof StructuralEvaluator.Is) {
                return settles(((StructuralEvaluator.Is) evaluator).evaluator, path);
            }
            if (evaluator instanceof StructuralEvaluator) {
                // ancestor, parent or sibling combinators look at nodes other than the candidate
                return false;
            }
            if (evaluator instanceof HasEvaluator) {
                // :has() may depend on descendants or later siblings that have not been parsed yet
                return false;
            }
            if (evaluator.wantsNodes()) {
                // ::text / ::comment etc. match leaf nodes, which the element-only stream never emits as candidates
                return false;
            }
            // every remaining evaluator (tag, id, class, attributes, positional pseudo-classes, text predicates...)
            // decides from the candidate and its already-complete sibling set
            return true;
        } finally {
            path.pop();
        }
    }
}
