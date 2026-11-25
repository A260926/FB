import task_support.Inputs;
import task_support.ContractSuite;
import task_support.ContractSuite.Body;
import task_support.ContractSuite.Case;

import cz.muni.stanse.boundschecker.BoundsChecker;
import cz.muni.stanse.boundschecker.BoundsFinding;
import cz.muni.stanse.checker.Checker;
import cz.muni.stanse.checker.CheckerError;
import cz.muni.stanse.checker.CheckerErrorReceiver;
import cz.muni.stanse.checker.CheckerFactory;
import cz.muni.stanse.codestructures.CFGHandle;
import cz.muni.stanse.codestructures.CFGNode;
import cz.muni.stanse.codestructures.LazyInternalStructures;
import cz.muni.stanse.codestructures.LazyInternalStructuresIntra;
import cz.muni.stanse.pointeranalyzer.PointsToAnalyzer;
import cz.muni.stanse.pointeranalyzer.shapirohorwitz.ShapiroHorwitzAnalyzer;
import cz.muni.stanse.pointeranalyzer.shapirohorwitz.SteensgaardCategorizationProvider;
import cz.muni.stanse.utils.Pair;

import org.dom4j.Element;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.Set;

/**
 * Static regression contract for {@link BoundsChecker}.
 *
 * <p>Every case runs the real CUnit parser, the real Stanse CFG and the new
 * checker; there is no regex scanning of source text and no execution of
 * analyzed code.  Cases are registered in the three required groups
 * {@code correlated}, {@code loops} and {@code witness}.</p>
 */
public final class BoundsContract {

    private static final List<Case> CASES = new ArrayList<Case>();

    static {
        // ------------------------------------------------------------
        // correlated: branch/object/index pairing and alias independence
        // ------------------------------------------------------------
        reg("correlated", "branch_constants_stay_paired", new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bc_c1",
                    "void f(int sel){int small[4];int big[16];int *p;int i;"
                    + "if(sel){p=small;i=1;}else{p=big;i=10;}p[i]=0;}");
                List<BoundsFinding> fs = new BoundsChecker().analyze(cfg);
                check(fs.size() == 1, "one merged access expected: " + fs);
                BoundsFinding f = fs.get(0);
                check(f.isProved(), "correlated branches must prove: " + f);
                check(BoundsFinding.PROVED.equals(f.getStatus()),
                        "status literal proved");
                check(BoundsFinding.PROVED.equals(
                                f.getObjectStatuses().get("small")),
                        "small paired and proved: " + f.getObjectStatuses());
                check(BoundsFinding.PROVED.equals(
                                f.getObjectStatuses().get("big")),
                        "big paired and proved: " + f.getObjectStatuses());
                check(f.getOrigins().contains(Pair.make(cfg, "small")),
                        "origin small present");
                check(f.getOrigins().contains(Pair.make(cfg, "big")),
                        "origin big present");
            }
        });

        reg("correlated", "branch_guards_with_early_return_stay_paired",
                new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bc_c2",
                    "void f(int c,int i){int small[4];int big[16];int *p;"
                    + "if(c){p=small;if(i<0||i>=4)return;}"
                    + "else{p=big;if(i<0||i>=16)return;}p[i]=0;}");
                BoundsFinding f = only(new BoundsChecker().analyze(cfg));
                check(f.isProved(),
                        "length-correlated guards must prove: " + f);
                check(BoundsFinding.PROVED.equals(
                                f.getObjectStatuses().get("small")),
                        "small proved under its own guard");
                check(BoundsFinding.PROVED.equals(
                                f.getObjectStatuses().get("big")),
                        "big proved under its own guard");
            }
        });

        reg("correlated", "constant_on_left_guards_are_normalized",
                new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bc_c2b",
                    "void f(int i){int a[4];if(0<=i&&4>i){a[i]=0;}}");
                check(only(new BoundsChecker().analyze(cfg)).isProved(),
                        "0 <= i && 4 > i proves a[i]");

                CFGHandle cfg2 = parse("bc_c2c",
                    "void g(int i){int a[4];if(0<=i&&i<4){a[i]=0;}}");
                check(only(new BoundsChecker().analyze(cfg2)).isProved(),
                        "0 <= i && i < 4 proves a[i]");
            }
        });

        reg("correlated", "one_bad_branch_cannot_be_covered_by_good_other",
                new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bc_c3",
                    "void f(int sel){int small[4];int big[16];int *p;int i;"
                    + "if(sel){p=small;i=9;}else{p=big;i=10;}p[i]=0;}");
                BoundsFinding f = only(new BoundsChecker().analyze(cfg));
                check(f.isUnproved(), "bad small branch taints result: " + f);
                check(BoundsFinding.UNPROVED.equals(
                                f.getObjectStatuses().get("small")),
                        "small unproved");
                check(BoundsFinding.PROVED.equals(
                                f.getObjectStatuses().get("big")),
                        "big remains proved independently");
                check(!f.getWitness().isEmpty(),
                        "unproved result carries a witness");
            }
        });

        reg("correlated", "alias_copy_and_rebind_are_independent", new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bc_c4",
                    "void f(void){int a[4];int b[4];int *p=a;int *q=p;"
                    + "p=b;q[2]=0;p[3]=0;}");
                List<BoundsFinding> fs = new BoundsChecker().analyze(cfg);
                check(fs.size() == 2, "two accesses expected: " + fs);
                BoundsFinding q = forRoot(fs, "q");
                BoundsFinding p = forRoot(fs, "p");
                check(q.isProved(), "q still aliases a and proves: " + q);
                check(p.isProved(), "p rebound to b and proves: " + p);
                check(q.getOrigins().contains(Pair.make(cfg, "a")),
                        "q origin is a");
                check(!q.getOrigins().contains(Pair.make(cfg, "b")),
                        "q must not see rebinding of p");
                check(p.getOrigins().contains(Pair.make(cfg, "b")),
                        "p origin is b");
            }
        });

        reg("correlated", "stale_alias_length_mismatch_is_unproved",
                new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bc_c5",
                    "void f(void){int a[2];int b[4];int *p=a;int *q=p;"
                    + "p=b;q[2]=0;p[3]=0;}");
                List<BoundsFinding> fs = new BoundsChecker().analyze(cfg);
                BoundsFinding q = forRoot(fs, "q");
                BoundsFinding p = forRoot(fs, "p");
                check(q.isUnproved(), "q[2] against length 2 unproved: " + q);
                check(p.isProved(), "p[3] against length 4 proved: " + p);
                check(BoundsFinding.UNPROVED.equals(
                                q.getObjectStatuses().get("a")),
                        "a marked unproved for q");
            }
        });

        reg("correlated", "production_pointsTo_query_is_program_point_sensitive",
                new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bc_c6",
                    "void f(void){int a[4];int b[8];int *p=a;int *q=p;"
                    + "p=b;q[2]=0;p[3]=0;}");
                PointsToAnalyzer pta = new ShapiroHorwitzAnalyzer(
                        new SteensgaardCategorizationProvider());
                pta.analyze(Collections.singletonList(cfg));

                CFGNode qNode = accessNodeForRoot(cfg, "q");
                CFGNode pNode = accessNodeForRoot(cfg, "p");
                check(qNode != null && pNode != null,
                        "real access CFGNodes required");

                Set<Pair<CFGHandle, String>> qAt =
                        pta.getPointsToSetAt(cfg, qNode, "q");
                Set<Pair<CFGHandle, String>> pAt =
                        pta.getPointsToSetAt(cfg, pNode, "p");
                check(qAt.contains(Pair.make(cfg, "a")),
                        "program-point q points to a: " + qAt);
                check(!qAt.contains(Pair.make(cfg, "b")),
                        "program-point q must not contain rebound b: " + qAt);
                check(pAt.contains(Pair.make(cfg, "b")),
                        "program-point p points to rebound b: " + pAt);

                // Whole-function query behaviour is retained and non-null.
                check(pta.getPointsToSetOf(cfg, "p") != null,
                        "legacy getPointsToSetOf still available");

                // Default interface behaviour for implementations without
                // program-point snapshots is an explicit unsupported report.
                PointsToAnalyzer bare = new PointsToAnalyzer() {
                    public void analyze(Collection<CFGHandle> cfgs) { }
                    public Set<Pair<CFGHandle, String>> getPointsToSetOf(
                            CFGHandle c, String id) {
                        return Collections.emptySet();
                    }
                };
                boolean threw = false;
                try {
                    bare.getPointsToSetAt(null, null, "x");
                } catch (UnsupportedOperationException expected) {
                    threw = true;
                }
                check(threw, "other implementations may refuse getPointsToSetAt");
            }
        });

        reg("correlated", "integer_overwrite_invalidates_old_constraints",
                new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bc_c7",
                    "void f(int c){int a[4];int i=0;"
                    + "while(i<4){a[i]=0;i=i+1;}i=9;a[i]=0;}");
                List<BoundsFinding> fs = new BoundsChecker().analyze(cfg);
                check(fs.size() == 2, "loop and post-overwrite access: " + fs);
                int proved = 0;
                int unproved = 0;
                BoundsFinding after = null;
                for (BoundsFinding f : fs) {
                    if (f.isProved())
                        proved++;
                    if (f.isUnproved()) {
                        unproved++;
                        after = f;
                    }
                }
                check(proved == 1, "loop access proved exactly once: " + fs);
                check(unproved == 1 && after != null,
                        "overwritten i=9 loses old i<4 constraint: " + fs);
                check(after.getWitnessLines().contains(Integer.valueOf(1)),
                        "overwrite witness stays on the real line");
                boolean sawOverwrite = false;
                for (BoundsFinding.WitnessStep s : after.getWitness())
                    sawOverwrite |= s.getDescription() != null
                            && s.getDescription().contains("i = 9");
                check(sawOverwrite,
                        "unproved witness contains the i=9 rewrite");
            }
        });

        // ------------------------------------------------------------
        // loops: convergence, guards, break/continue, early return
        // ------------------------------------------------------------
        reg("loops", "for_loop_converges_and_proves", new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bl_l1",
                    "void safe(void){int a[4];int i;"
                    + "for(i=0;i<4;++i)a[i]=0;}");
                BoundsFinding f = only(new BoundsChecker().analyze(cfg));
                check(f.isProved(), "canonical for loop must prove: " + f);
            }
        });

        reg("loops", "while_loop_constant_increment_proves", new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bl_l2",
                    "void f(void){int a[4];int i=0;"
                    + "while(i<4){a[i]=0;i=i+1;}}");
                check(only(new BoundsChecker().analyze(cfg)).isProved(),
                        "while i<4 proves");
            }
        });

        reg("loops", "descending_loop_proves", new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bl_l3",
                    "void f(void){int a[4];int i=3;"
                    + "while(i>=0){a[i]=0;i=i-1;}}");
                check(only(new BoundsChecker().analyze(cfg)).isProved(),
                        "descending i>=0 proves");
            }
        });

        reg("loops", "unguarded_wide_loop_is_not_falsely_proved_by_unroll",
                new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bl_l4",
                    "void f(void){int a[4];int i;"
                    + "for(i=0;i<10;i++)a[i]=0;}");
                BoundsFinding f = only(new BoundsChecker().analyze(cfg));
                check(f.isUnproved(),
                        "iterations beyond the threshold must stay checked: "
                                + f);
                check(!f.getWitness().isEmpty(),
                        "unproved wide loop has a real witness");

                // A huge bound must still converge quickly through widening
                // and must not turn into a false proof.
                CFGHandle huge = parse("bl_l4b",
                    "void h(void){int a[4];int i;"
                    + "for(i=0;i<1000000;i++)a[i]=0;}");
                long t0 = System.currentTimeMillis();
                BoundsFinding hf = only(new BoundsChecker().analyze(huge));
                long dt = System.currentTimeMillis() - t0;
                check(hf.isUnproved(), "huge loop unproved, not unrolled");
                check(dt < 60000, "widening converges promptly, was " + dt);
            }
        });

        reg("loops", "break_guard_idiom_proves", new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bl_l5",
                    "void f(void){int a[4];int i=0;"
                    + "while(1){if(i>=4)break;a[i]=0;i++;}}");
                check(only(new BoundsChecker().analyze(cfg)).isProved(),
                        "break at i>=4 guards the access");
            }
        });

        reg("loops", "continue_skips_one_value_but_access_stays_proved",
                new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bl_l6",
                    "void f(void){int a[4];int i=0;"
                    + "while(i<4){if(i==2)continue;a[i]=0;i=i+1;}}");
                check(only(new BoundsChecker().analyze(cfg)).isProved(),
                        "continue inside bounded loop still proves");
            }
        });

        reg("loops", "repeated_scans_are_stable_and_isolated", new Body() {
            public void run() throws Exception {
                CFGHandle good = parse("bl_l7",
                    "void g(void){int a[4];for(int i=0;i<4;i++)a[i]=0;}");
                BoundsChecker bc = new BoundsChecker();
                List<BoundsFinding> first = bc.analyze(good);
                List<BoundsFinding> second = bc.analyze(good);
                check(first.size() == 1 && second.size() == 1,
                        "stable finding count");
                check(first.get(0).getStatus().equals(
                                second.get(0).getStatus()),
                        "stable status across scans");

                CFGHandle bad = parse("bl_l7b",
                    "void b(void){int a[4];a[40]=0;}");
                check(new BoundsChecker().analyze(bad).get(0).isUnproved(),
                        "bad scan unproved");
                // Reusing the same checker after a bad scan must not leak.
                check(bc.analyze(good).get(0).isProved(),
                        "good scan still proved after bad scan");
            }
        });

        reg("loops", "early_return_kills_only_exit_path", new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bl_l8",
                    "void f(int c){int a[4];if(c)return;a[1]=0;a[5]=0;}");
                List<BoundsFinding> fs = new BoundsChecker().analyze(cfg);
                check(fs.size() == 2, "two post-guard accesses: " + fs);
                int proved = 0;
                int unproved = 0;
                for (BoundsFinding f : fs) {
                    if (f.isProved())
                        proved++;
                    if (f.isUnproved())
                        unproved++;
                }
                check(proved == 1, "a[1] reachable and proved: " + fs);
                check(unproved == 1, "a[5] unproved: " + fs);

                // Error branch containing its own access is reported, but
                // its state does not flow into the normal access.
                CFGHandle err = parse("bl_l8b",
                    "void e(int c){int a[4];if(c){a[9]=0;return;}a[1]=0;}");
                List<BoundsFinding> ef = new BoundsChecker().analyze(err);
                check(ef.size() == 2, "error access plus normal access");
                boolean normalProved = false;
                boolean errorUnproved = false;
                for (BoundsFinding f : ef) {
                    if (f.getLine() >= 1 && f.getAccessText() != null
                            && f.getAccessText().contains("1"))
                        normalProved |= f.isProved();
                    if (f.isUnproved())
                        errorUnproved = true;
                }
                check(normalProved, "normal path access proved");
                check(errorUnproved, "error path access unproved");
            }
        });

        // ------------------------------------------------------------
        // witness: positions, sorting, dedup, real traces, isolation
        // ------------------------------------------------------------
        reg("witness", "findings_sorted_by_file_line_column", new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bw_w1",
                    "void w_lines(int c){\n"
                    + " int a[4];\n"
                    + " int i;\n"
                    + " if(c){i=9;}else{i=1;}\n"
                    + " a[i]=0;\n"
                    + " a[2]=0;\n"
                    + "}\n");
                List<BoundsFinding> fs = new BoundsChecker().analyze(cfg);
                check(fs.size() == 2, "two accesses on distinct lines: " + fs);
                BoundsFinding x = fs.get(0);
                BoundsFinding y = fs.get(1);
                check(x.getLine() == 5, "first access on line 5: " + x);
                check(y.getLine() == 6, "second access on line 6: " + y);
                check(x.isUnproved(), "line 5 unproved");
                check(y.isProved(), "line 6 proved");
                check(x.getColumn() >= 1 && y.getColumn() >= 1,
                        "1-based columns exposed");
                check(x.getFileName() != null
                        && x.getFileName().contains("bw_w1"),
                        "file identity exposed: " + x.getFileName());
                for (int i = 1; i < fs.size(); i++)
                    check(fs.get(i - 1).compareTo(fs.get(i)) <= 0,
                            "findings globally sorted");
            }
        });

        reg("witness", "same_access_deduped_across_backedge_and_paths",
                new Body() {
            public void run() throws Exception {
                CFGHandle loop = parse("bw_w2",
                    "void f(void){int a[4];int i;for(i=0;i<4;++i)a[i]=0;}");
                List<BoundsFinding> lf = new BoundsChecker().analyze(loop);
                check(lf.size() == 1,
                        "one stable finding despite backedge visits: " + lf);
                int xmlAccesses = loop.getElement()
                        .selectNodes(".//arrayAccess").size();
                check(xmlAccesses == 1, "fixture has one syntactic access");

                // Many paths, one syntactic access, still one finding.
                CFGHandle multi = parse("bw_w2b",
                    "void m(int c,int d){int a[4];int i=0;"
                    + "if(c){i=1;}if(d){i=2;}a[i]=0;}");
                BoundsFinding mf = only(new BoundsChecker().analyze(multi));
                check(mf.isProved(), "all merged paths stay in bounds: " + mf);

                // Two possible origins but one access -> one finding.
                CFGHandle corr = parse("bw_w2c",
                    "void k(int s){int a[4];int b[4];int *p;"
                    + "if(s)p=a;else p=b;p[1]=0;}");
                BoundsFinding cf = only(new BoundsChecker().analyze(corr));
                check(cf.getOrigins().size() == 2,
                        "both origins recorded on single finding");
                check(cf.isProved(), "index 1 fits both length-4 arrays");
            }
        });

        reg("witness", "witness_is_real_and_never_splices_exclusive_branches",
                new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bw_w3",
                    "void w_coh(int c){\n"
                    + " int small[4]; int big[16]; int *p; int i;\n"
                    + " if(c){\n"
                    + "   p=small;\n"
                    + "   i=9;\n"
                    + " } else {\n"
                    + "   p=big;\n"
                    + "   i=99;\n"
                    + " }\n"
                    + " p[i]=0;\n"
                    + "}\n");
                BoundsFinding f = only(new BoundsChecker().analyze(cfg));
                check(f.isUnproved(), "both branches bad: " + f);
                List<Integer> lines = f.getWitnessLines();
                check(!lines.isEmpty(), "witness steps present");
                check(lines.contains(Integer.valueOf(10)),
                        "witness ends at access line 10: " + lines);
                boolean thenSide = lines.contains(4) || lines.contains(5);
                boolean elseSide = lines.contains(7) || lines.contains(8);
                check(thenSide ^ elseSide,
                        "witness must follow exactly one branch, lines="
                                + lines);
                check(!(thenSide && elseSide),
                        "exclusive branch rewrites must not be concatenated");
                boolean hasCheckStep = false;
                for (BoundsFinding.WitnessStep s : f.getWitness()) {
                    check(s.getLine() >= 1, "step has real line");
                    check(s.getDescription() != null
                            && s.getDescription().length() > 0,
                            "step describes condition/rewrite");
                    if ("bounds-check".equals(s.getKind()))
                        hasCheckStep = true;
                }
                check(hasCheckStep, "witness terminates in bounds-check step");
            }
        });

        reg("witness", "witness_contains_condition_source_line", new Body() {
            public void run() throws Exception {
                CFGHandle cfg = parse("bw_w4",
                    "void w_cond(int i){\n"
                    + " int a[4];\n"
                    + " if(i>=9){\n"
                    + "   a[i]=0;\n"
                    + " }\n"
                    + "}\n");
                BoundsFinding f = only(new BoundsChecker().analyze(cfg));
                check(f.isUnproved(), "i>=9 path unproved: " + f);
                check(f.getWitnessLines().contains(Integer.valueOf(3)),
                        "condition line 3 in witness: " + f.getWitnessLines());
                check(f.getWitnessLines().contains(Integer.valueOf(4)),
                        "access line 4 in witness");
                boolean conditionStep = false;
                for (BoundsFinding.WitnessStep s : f.getWitness())
                    conditionStep |= "condition".equals(s.getKind());
                check(conditionStep, "witness records a condition event");
            }
        });

        reg("witness", "multiple_functions_and_report_channel_isolation",
                new Body() {
            public void run() throws Exception {
                List<CFGHandle> cfgs = Inputs.parse("bw_w5",
                    "void one(void){int a[3];a[1]=0;}\n"
                    + "void two(void){int b[2];b[5]=0;}\n");
                check(cfgs.size() == 2, "two functions parsed");
                BoundsChecker bc = new BoundsChecker();
                BoundsFinding f1 = only(bc.analyze(cfgs.get(0)));
                BoundsFinding f2 = only(bc.analyze(cfgs.get(1)));
                check("one".equals(f1.getFunctionName())
                        || "two".equals(f1.getFunctionName()),
                        "function name exposed: " + f1.getFunctionName());
                check(f1.isProved() || f2.isProved(),
                        "first function has a proved result");
                check(f1.isUnproved() || f2.isUnproved(),
                        "second function has an unproved result");
                check(!f1.getFunctionName().equals(f2.getFunctionName())
                        || f1.isProved() != f2.isProved(),
                        "functions stay distinguishable");

                // Checker interface routes unproved findings to the
                // established CheckerErrorReceiver report channel.
                LazyInternalStructures internals =
                        new LazyInternalStructuresIntra(
                                Collections.emptyList(), cfgs);
                final List<CheckerError> errors =
                        new ArrayList<CheckerError>();
                CheckerErrorReceiver receiver = new CheckerErrorReceiver() {
                    @Override
                    public void receive(CheckerError error) {
                        errors.add(error);
                    }
                };
                Checker checker = (Checker) CheckerFactory
                        .createIntraprocedural("BoundsChecker",
                                Collections.<File>emptyList());
                checker.check(internals, receiver, null);
                check(errors.size() == 1,
                        "exactly the unproved function reported: "
                                + errors.size());
                for (CheckerError e : errors)
                    check("BoundsChecker".equals(e.getCheckerName()),
                            "error tagged with checker name");
            }
        });

        reg("witness", "factory_registration_and_scope_info", new Body() {
            public void run() throws Exception {
                check(CheckerFactory.getRegisteredCheckers()
                                .contains("BoundsChecker"),
                        "BoundsChecker registered: "
                                + CheckerFactory.getRegisteredCheckers());
                Object made = CheckerFactory.createIntraprocedural(
                        "BoundsChecker", Collections.<File>emptyList());
                check(made instanceof BoundsChecker,
                        "factory creates a BoundsChecker");
                String info = CheckerFactory.getCheckerCreationInfo(
                        "BoundsChecker");
                check(info != null && info.length() > 20,
                        "short support-scope description required");
                check(info.contains("array") || info.contains("bounds"),
                        "scope note mentions bounds/array support");
            }
        });
    }

    // ------------------------------------------------------------------
    // ContractSuite entry point
    // ------------------------------------------------------------------

    public static List<Case> cases() {
        return CASES;
    }

    private static void reg(final String group, final String name,
                            final ContractSuite.Body body) {
        CASES.add(new Case(group, name, body));
    }

    // ------------------------------------------------------------------
    // helpers
    // ------------------------------------------------------------------

    private static void check(final boolean ok, final String message) {
        ContractSuite.check(ok, message);
    }

    private static CFGHandle parse(final String name, final String source)
            throws Exception {
        List<CFGHandle> cfgs = Inputs.parse(name, source);
        check(!cfgs.isEmpty(), "CUnit produced a CFG for " + name);
        return cfgs.get(0);
    }

    private static BoundsFinding only(final List<BoundsFinding> fs) {
        check(fs.size() == 1, "expected exactly one finding but got "
                + fs.size() + ": " + fs);
        return fs.get(0);
    }

    private static BoundsFinding forRoot(final List<BoundsFinding> fs,
                                         final String root) {
        for (BoundsFinding f : fs) {
            String t = f.getAccessText();
            if (t != null && (t.startsWith(root + "[")
                    || t.replace(" ", "").startsWith(root + "[")))
                return f;
        }
        check(false, "no finding for root '" + root + "' in " + fs);
        return null;
    }

    private static CFGNode accessNodeForRoot(final CFGHandle cfg,
                                             final String root) {
        for (CFGNode node : cfg.getAllNodes()) {
            Element el = node.getElement();
            if (el == null)
                continue;
            @SuppressWarnings("unchecked")
            List<Element> accs =
                    el.selectNodes("descendant-or-self::arrayAccess");
            for (Element acc : accs) {
                if (acc.elements().isEmpty())
                    continue;
                Element base = (Element) acc.elements().get(0);
                if ("id".equals(base.getName())
                        && root.equals(base.getTextTrim()))
                    return node;
            }
        }
        return null;
    }
}
