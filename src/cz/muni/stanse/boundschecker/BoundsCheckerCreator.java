package cz.muni.stanse.boundschecker;

import cz.muni.stanse.checker.Checker;
import cz.muni.stanse.checker.CheckerCreator;
import cz.muni.stanse.checker.CheckerException;

import java.io.File;
import java.util.List;

/**
 * Registers {@link BoundsChecker} under the factory name
 * {@code "BoundsChecker"} for the intraprocedural entry point.
 */
public final class BoundsCheckerCreator extends CheckerCreator {

    @Override
    public String getCheckerName() {
        return getNameForCheckerFactory();
    }

    @Override
    public String getCheckerCreationInfo() {
        return "BoundsChecker proves one-dimensional fixed-size int array "
                + "accesses (a[i], p[i]) in headless intraprocedural C code "
                + "with local pointer copies/rebindings, integer constant "
                + "arithmetic and if/for/while/break/continue/return; "
                + "unsupported forms are conservatively unproved.";
    }

    @Override
    public List<String> getDataFilesExtensions() {
        return java.util.Collections.emptyList();
    }

    @Override
    public boolean checkArgumentList(final List<File> args) {
        return args != null && args.size() == 0;
    }

    @Override
    public Checker createIntraprocedural(final List<File> args)
            throws CheckerException {
        if (!checkArgumentList(args))
            throw new CheckerException("Bad number of data arguments. "
                    + "BoundsChecker accepts no definition files.");
        return new BoundsChecker();
    }

    @Override
    public Checker createInterprocedural(final List<File> args)
            throws CheckerException {
        return createIntraprocedural(args);
    }

    static String getNameForCheckerFactory() {
        return BoundsChecker.CHECKER_NAME;
    }
}
