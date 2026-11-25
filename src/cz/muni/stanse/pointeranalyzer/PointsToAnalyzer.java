package cz.muni.stanse.pointeranalyzer;

import java.util.Collection;
import java.util.Set;
import cz.muni.stanse.utils.Pair;
import cz.muni.stanse.codestructures.CFGHandle;
import cz.muni.stanse.codestructures.CFGNode;

/**
 *
 * @author Michal
 */
public interface PointsToAnalyzer {

    public void analyze(Collection<CFGHandle> cfgs);

    public Set<Pair<CFGHandle, String>> getPointsToSetOf(CFGHandle cfg, String id);

    /**
     * Returns the may-points-to set for the symbol {@code id} at the
     * intraprocedural program point immediately before {@code node} is
     * executed. Every returned pair identifies the owning CFG and an array
     * (or abstract symbol) identifier.
     *
     * <p>This is a program-point snapshot and therefore must reflect
     * flow-sensitive events such as pointer rebindings; it is not the final
     * whole-function points-to set.</p>
     *
     * <p>Analyzers which do not provide program-point snapshots keep the
     * default implementation, which reports that the query is unsupported.</p>
     *
     * @param cfg  CFG of the function containing {@code node}
     * @param node node whose pre-state is requested
     * @param id   local symbol whose points-to set is requested
     * @return set of {@code (CFGHandle, identifier)} possible pointees,
     *         never {@code null} for a supported implementation
     */
    default Set<Pair<CFGHandle, String>> getPointsToSetAt(
            CFGHandle cfg, CFGNode node, String id) {
        throw new UnsupportedOperationException(
                getClass().getName() + " does not provide program-point "
                + "points-to snapshots (getPointsToSetAt)");
    }

}
