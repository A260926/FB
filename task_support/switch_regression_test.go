package task_support

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"harness.local/jdcore/task_support/taskcheck"
)

// toolBin resolves the java / javac executables from JAVA_HOME or PATH.
func toolBin(t *testing.T, name string) string {
	t.Helper()
	if home := os.Getenv("JAVA_HOME"); home != "" {
		p := filepath.Join(home, "bin", name)
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	p, err := exec.LookPath(name)
	if err != nil {
		t.Fatalf("cannot locate %s: set JAVA_HOME: %v", name, err)
	}
	return p
}

// repoRoot walks parent directories until it finds go.mod.
func repoRoot(t *testing.T) string {
	t.Helper()
	dir, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "go.mod")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			t.Fatal("go.mod not found")
		}
		dir = parent
	}
}

func analyzerClassPath(t *testing.T) string {
	t.Helper()
	root := repoRoot(t)
	return strings.Join([]string{
		filepath.Join(root, ".task-build", "classes"),
		filepath.Join(root, ".task-build", "analyzer"),
	}, string(os.PathListSeparator))
}

func runJavaTool(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command(args[0], args[1:]...)
	if dir != "" {
		cmd.Dir = dir
	}
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("command failed (%v): %s", args, err)
	}
	return string(out)
}

func compileClasses(t *testing.T, src, out string, noDebug bool) {
	t.Helper()
	javac := toolBin(t, "javac")
	args := []string{javac, "--release", "17", "-proc:none"}
	if noDebug {
		args = append(args, "-g:none")
	}
	args = append(args, "-d", out, src)
	runJavaTool(t, "", args...)
}

// runProbe executes one of the task_support Java probes against compiled classes.
func runProbe(t *testing.T, classDir, internalName, probe string) string {
	t.Helper()
	java := toolBin(t, "java")
	return runJavaTool(t, "", java, "-cp", analyzerClassPath(t), probe, classDir, internalName)
}

func decompile(t *testing.T, classDir, internalName string) string {
	t.Helper()
	return runProbe(t, classDir, internalName, "task_support.Analyze")
}

func recompileSource(t *testing.T, sourcePath string) {
	t.Helper()
	javac := toolBin(t, "javac")
	out := t.TempDir()
	runJavaTool(t, "", javac, "--release", "17", "-proc:none", "-d", out, sourcePath)
}

// compileFixture builds the production switch expression fixture once per
// debug mode and returns its output directory.
func compileFixture(t *testing.T, noDebug bool) (string, string) {
	t.Helper()
	root := repoRoot(t)
	src := filepath.Join(root, "task_support", "fixtures", "SwitchExprSamples.java")
	out := filepath.Join(t.TempDir(), "classes")
	if err := os.MkdirAll(out, 0o755); err != nil {
		t.Fatal(err)
	}
	compileClasses(t, src, out, noDebug)
	return out, "SwitchExprSamples"
}

func writeDecompiled(t *testing.T, internalName, source string) string {
	t.Helper()
	simpleName := internalName
	if i := strings.LastIndex(simpleName, "/"); i >= 0 {
		simpleName = simpleName[i+1:]
	}
	dir := t.TempDir()
	path := filepath.Join(dir, simpleName+".java")
	if err := os.WriteFile(path, []byte(source), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func contains(line, fragment string) bool {
	return strings.Contains(line, fragment)
}

// outlineIndex maps an outline prefix (e.g. "METHOD denseReturn") to its lines.
func outlineBlocks(outline string) map[string][]string {
	blocks := make(map[string][]string)
	var current string
	for _, line := range strings.Split(outline, "\n") {
		trimmed := strings.TrimLeft(line, " ")
		if strings.HasPrefix(trimmed, "METHOD ") {
			current = strings.TrimPrefix(trimmed, "METHOD ")
			blocks[current] = nil
			continue
		}
		if current != "" && trimmed != "" {
			blocks[current] = append(blocks[current], trimmed)
		}
	}
	return blocks
}

func findLine(lines []string, fragment string) (string, bool) {
	for _, line := range lines {
		if contains(line, fragment) {
			return line, true
		}
	}
	return "", false
}

// TestSwitchStackMerges verifies how the switch expression interacts with the
// operand stack: dense/sparse jumps, shared targets, pre-stacked operands,
// block yields, throws, embedded ifs and nesting, both with and without debug
// information. The two debug modes are exercised inside a single test group so
// the task harness records one TASK_GROUP_DONE line for this group.
func TestSwitchStackMerges(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, noDebug := range []bool{false, true} {
			mode := "debug"
			if noDebug {
				mode = "nodebug"
			}
			classDir, internalName := compileFixture(t, noDebug)
			outline := runProbe(t, classDir, internalName, "task_support.SwitchAstProbe")
			blocks := outlineBlocks(outline)
			source := decompile(t, classDir, internalName)

			// Every fixture method must be modeled as a switch expression.
			c.Check(strings.Count(outline, "SWITCH_EXPR") >= 10, "expected at least ten switch expressions, got %d", strings.Count(outline, "SWITCH_EXPR"))
			c.Check(!strings.Contains(outline, "STMT_SWITCH"), "no statement switch must be produced for expression fixtures")
			c.Check(!strings.Contains(source, "Byte code:"), "decompilation must not fall back to byte code comments")

			// Dense tableswitch: six distinct arms including default.
			dense := blocks["denseReturn"]
			line, ok := findLine(dense, "SWITCH_EXPR")
			c.Check(ok && contains(line, "arms=6"), "dense switch must expose six arms: %q", line)
			c.Check(countFragments(dense, "ARM label=") == 6, "dense switch must keep six label blocks")

			// Sparse lookupswitch with a shared target: labels are grouped, not duplicated.
			sparse := blocks["sparseAssign"]
			shared, ok := findLine(sparse, "ARM label=10,100")
			c.Check(ok, "shared switch target must produce one multi-label arm: %q", shared)
			c.Check(countFragments(sparse, "ARM label=") == 4, "sparse switch must expose four arms")

			// Block arm keeps the local variable scope and ends in a yield.
			blockArm := blocks["blockYieldAndThrow"]
			_, ok = findLine(blockArm, "BODY block-yield")
			c.Check(ok, "block arm must be modeled as block-yield")
			_, ok = findLine(blockArm, "LOCAL_VAR PrimitiveType{primitive=int}")
			c.Check(ok, "block arm must preserve its local variable declaration")
			_, ok = findLine(blockArm, "YIELD")
			c.Check(ok, "block arm must contain a yield statement")

			// Throwing arm contributes no result value.
			_, ok = findLine(blockArm, "BODY throw")
			c.Check(ok, "throwing arm must be modeled as a throw body")
			c.Check(!containsBlock(blockArm, "THROW", "YIELD"), "a throwing arm must not carry a yield")

			// An embedded if has two normal exits that both yield to the same switch.
			ifArm := blocks["ifInsideArm"]
			_, ok = findLine(ifArm, "IF")
			c.Check(ok, "if arm must keep its if statement")
			c.Check(countFragments(ifArm, "YIELD ") == 2, "both if exits must yield to the enclosing switch")

			// Nested switch: inner yields belong to the inner switch expression.
			nested := blocks["nestedSwitch"]
			c.Check(countFragments(nested, "SWITCH_EXPR") == 2, "nested switch must model two switch expressions")
			outerDefault := blockFor(nested, "ARM label=default")
			c.Check(len(outerDefault) > 0 && !containsLine(outerDefault, "YIELD"), "outer default arm must not contain a nested yield")

			// Pre-stacked left operand for an arithmetic sub-expression.
			arith := blocks["arithmeticConsumer"]
			binary, ok := findLine(arith, "BINARY op=+")
			c.Check(ok, "arithmetic consumer keeps the binary operator")
			posBinary := indexOf(arith, binary)
			switchPos, hasSwitch := findLine(arith, "SWITCH_EXPR")
			c.Check(hasSwitch && posBinary >= 0 && indexOf(arith, switchPos) > posBinary, "switch expression is the right operand of the binary expression")
			_, ok = findLine(arith, "VAR_REF")
			c.Check(ok, "pre-stacked left operand is preserved")

			// Pre-stacked method receiver / earlier argument.
			args := blocks["argumentConsumer"]
			_, ok = findLine(args, "METHOD_CALL name=addExact")
			c.Check(ok, "method argument consumer preserved")
			receiver := blocks["receiverConsumer"]
			_, ok = findLine(receiver, "THIS ")
			c.Check(ok, "pre-stacked instance receiver is preserved")
			_, ok = findLine(receiver, "SWITCH_EXPR")
			c.Check(ok, "receiver method takes the switch expression as argument")

			// The produced source recompiles under javac 17.
			recompileSource(t, writeDecompiled(t, internalName, source))
			_ = mode
		}
	})
}

// TestSwitchTypes checks the result type handling for int, long and String
// (including a null arm), the two-slot long merge and the narrowing of
// maybe-typed integer constants.
func TestSwitchTypes(t *testing.T) {
	classDir, internalName := compileFixture(t, true)
	taskcheck.Run(t, func(c *taskcheck.C) {
		outline := runProbe(t, classDir, internalName, "task_support.SwitchAstProbe")
		blocks := outlineBlocks(outline)
		source := decompile(t, classDir, internalName)

		// Int arms are narrowed to int constants, never rendered as boolean.
		dense := blocks["denseReturn"]
		line, ok := findLine(dense, "INT_CONST value=10")
		c.Check(ok && contains(line, "PrimitiveType{primitive=int}"), "int arm constant must have int type: %q", line)
		c.Check(!containsBlock(dense, "INT_CONST", "maybe_"), "maybe-typed int constants must be narrowed")

		// Long: two-slot category-2 result merged across arms.
		longs := blocks["longResult"]
		longLine, ok := findLine(longs, "SWITCH_EXPR")
		c.Check(ok && contains(longLine, "primitive=long"), "long switch expression must keep long type: %q", longLine)
		_, ok = findLine(longs, "LONG_CONST value=100000000000L")
		c.Check(ok, "long constant arm preserved")
		c.Check(contains(source, "100000000000L"), "long literal keeps its L suffix in source")

		// String reference result including a null arm.
		strings := blocks["stringResult"]
		strLine, ok := findLine(strings, "SWITCH_EXPR")
		c.Check(ok && contains(strLine, "java/lang/String"), "string switch expression must keep String type: %q", strLine)
		_, ok = findLine(strings, "STRING_CONST value=one ")
		c.Check(ok, "string constant arm preserved")
		nullLine, ok := findLine(strings, "NULL")
		c.Check(ok && contains(nullLine, "java/lang/String"), "null arm keeps the String target type: %q", nullLine)

		// Boolean result renders int constants as true/false, never as 1/0.
		boolean := blocks["booleanResult"]
		c.Check(countFragments(boolean, "BOOL_CONST") == 3, "all boolean arm values become boolean constants")
		c.Check(containsLine(boolean, "BOOL_CONST value=true") && containsLine(boolean, "BOOL_CONST value=false"),
			"boolean arm constants render true and false")
		c.Check(!containsLine(boolean, "INT_CONST"), "boolean arms must not keep raw int constants")
		c.Check(contains(source, "true;") && contains(source, "false;"), "boolean source emits true/false")

		// char result: arm constants stay narrowed to int (valid char values).
		char := blocks["charResult"]
		charLine, ok := findLine(char, "SWITCH_EXPR")
		c.Check(ok, "char switch expression is present: %q", charLine)
		c.Check(containsLine(char, "INT_CONST value=97") && containsLine(char, "INT_CONST value=122"),
			"char arm constants are preserved as int values")
		c.Check(contains(source, "char charResult"), "char-returning method keeps its declared return type")

		double := blocks["doubleResult"]
		doubleLine, ok := findLine(double, "SWITCH_EXPR")
		c.Check(ok && contains(doubleLine, "primitive=double"), "double switch keeps double type: %q", doubleLine)
		c.Check(containsLine(double, "DOUBLE_CONST value=1.5"), "double arm constant preserved")

		// if/else inside a block arm yields once after the conditional and is
		// never mistaken for a boolean result.
		ifElse := blocks["ifElseInsideArm"]
		_, ok = findLine(ifElse, "IF_ELSE")
		c.Check(ok, "if/else arm keeps its if/else statement")
		c.Check(countFragments(ifElse, "YIELD") == 1, "if/else arm yields once after the conditional")
		c.Check(countFragments(ifElse, "BINARY op==") == 2, "both if/else branches keep their assignments")
		c.Check(!containsLine(ifElse, "BOOL_CONST"), "int if/else arm must not be interpreted as boolean")

		// Long/string/source recompiles (category-2 and reference merges stay valid).
		recompileSource(t, writeDecompiled(t, internalName, source))
	})
}

// TestSwitchCompatibility makes sure the legacy statement switch, fall-through,
// break and continue paths are untouched, while the modern fixture round-trips.
func TestSwitchCompatibility(t *testing.T) {
	root := repoRoot(t)
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Legacy statement switch fixture stays a statement switch with colon labels.
		legacyDir := filepath.Join(root, ".task-build", "fixtures")
		legacySource := decompile(t, legacyDir, "LegacySwitch")
		c.Check(strings.Contains(legacySource, "switch ("), "legacy switch is still rendered")
		c.Check(strings.Contains(legacySource, "case 1:") || strings.Contains(legacySource, "case 2:"), "legacy switch keeps colon labels")
		c.Check(strings.Contains(legacySource, "break;"), "legacy switch keeps break statements")
		c.Check(!strings.Contains(legacySource, " -> "), "legacy switch must not be converted to arrow form")
		c.Check(!strings.Contains(legacySource, "yield"), "legacy switch must not contain yield")
		recompileSource(t, writeDecompiled(t, "LegacySwitch", legacySource))

		// A plain loop containing a switch statement keeps its statement form and
		// loop structure (statement switches are not converted to expressions).
		loopSrc := filepath.Join(t.TempDir(), "LoopSwitch.java")
		loopOut := filepath.Join(t.TempDir(), "loop-classes")
		if err := os.WriteFile(loopSrc, []byte(`
public class LoopSwitch {
    public static int run(int[] values) {
        int total = 0;
        for (int value : values) {
            switch (value) {
                case 0:
                    break;
                case 1:
                    break;
                default:
                    total += value;
            }
        }
        return total;
    }
}
`), 0o644); err != nil {
			t.Fatal(err)
		}
		compileClasses(t, loopSrc, loopOut, false)
		loopSource := decompile(t, loopOut, "LoopSwitch")
		c.Check(strings.Contains(loopSource, "for ("), "loop around a statement switch is preserved")
		c.Check(strings.Contains(loopSource, "switch ("), "statement switch inside a loop is preserved")
		c.Check(strings.Contains(loopSource, "break;"), "statement switch keeps break")
		c.Check(!strings.Contains(loopSource, " -> "), "statement switch inside a loop is not converted")
		recompileSource(t, writeDecompiled(t, "LoopSwitch", loopSource))

		// The shipped modern fixture round-trips through the full decompiler.
		modernSource := decompile(t, legacyDir, "ModernSwitch")
		c.Check(strings.Contains(modernSource, "switch ("), "modern switch expression is rendered")
		c.Check(strings.Contains(modernSource, " -> "), "modern switch uses arrow branches")
		c.Check(strings.Contains(modernSource, "yield "), "modern block arm keeps yield")
		recompileSource(t, writeDecompiled(t, "ModernSwitch", modernSource))
	})
}

func countFragments(lines []string, fragment string) int {
	n := 0
	for _, line := range lines {
		if contains(line, fragment) {
			n++
		}
	}
	return n
}

func containsBlock(lines []string, wanted, forbidden string) bool {
	sawWanted := false
	for _, line := range lines {
		if contains(line, wanted) {
			sawWanted = true
		}
		if sawWanted && contains(line, forbidden) {
			return true
		}
	}
	return false
}

func containsLine(lines []string, fragment string) bool {
	_, ok := findLine(lines, fragment)
	return ok
}

func blockFor(lines []string, label string) []string {
	start := -1
	for i, line := range lines {
		if contains(line, label) {
			start = i
			break
		}
	}
	if start < 0 {
		return nil
	}
	var block []string
	for i := start + 1; i < len(lines); i++ {
		if contains(lines[i], "ARM label=") {
			break
		}
		block = append(block, lines[i])
	}
	return block
}

func indexOf(lines []string, target string) int {
	for i, line := range lines {
		if line == target {
			return i
		}
	}
	return -1
}
