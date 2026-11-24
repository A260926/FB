package task_support

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"harness.local/jdcore/task_support/taskcheck"
)

const stackCasesSource = `
import java.util.List;
public class StackCases {
  static int direct(int x) {
    return switch (x) {
      case 0, 1 -> 11;
      case 100 -> 101;
      default -> 17;
    };
  }
  static int assigned(int x) {
    int value = switch (x) { case 1 -> 2; default -> 3; };
    return value * 2;
  }
  static int arithmetic(int x) {
    return 10 + switch (x) { case 0 -> 1; default -> 2; };
  }
  static int bothOperands(int x) {
    return switch (x) { case 1 -> 10; default -> 20; }
         + switch (x) { case 1 -> 1; default -> 2; };
  }
  static int leftOperand(int x) {
    return switch (x) { case 0 -> 10; default -> 20; } + 5;
  }
  static int argument(int x) {
    return Integer.sum(1, switch (x) { case 0 -> 2; default -> 3; });
  }
  static boolean receiverPrefix(List<Integer> xs, int x) {
    return xs.add(switch (x) { case 1 -> 1; default -> 2; });
  }
  static int sharedLabels(int x) {
    return switch (x) {
      case 1, 2, 3 -> 7;
      case 4, 5 -> 8;
      default -> 9;
    };
  }
  static int dense(int x) {
    return switch (x) {
      case 0 -> 10; case 1 -> 11; case 2 -> 12;
      case 3 -> 13; case 4 -> 14; default -> -1;
    };
  }
  static int sparse(int x) {
    return switch (x) {
      case 1 -> 100;
      case 100 -> 200;
      case 1000 -> 300;
      default -> 0;
    };
  }
  static int ifExits(int x, boolean b) {
    return switch (x) {
      case 1 -> {
        if (b) yield 2;
        else yield 3;
      }
      default -> 0;
    };
  }
  static int nested(int x, int y) {
    return switch (x) {
      case 1 -> switch (y) { case 2 -> 20; default -> 21; };
      default -> 30;
    };
  }
  static int convergence(int x) {
    return switch (x) {
      case 1 -> 100;
      case 2 -> 200;
      case 3 -> { int temporary = x * 3; yield temporary + 1; }
      case 4 -> throw new IllegalStateException("bad");
      default -> 0;
    };
  }
}
`

const typeCasesSource = `
public class TypeCases {
  static int intValue(int x) {
    return switch (x) { case -1 -> -10; case 0 -> 0; default -> 42; };
  }
  static long longValue(int x) {
    return switch (x) { case 1 -> 123456789012L; default -> 2L; };
  }
  static long mixedWide(int x) {
    return switch (x) { case 1 -> 1; default -> 2L; };
  }
  static String stringValue(int x) {
    return switch (x) { case 1 -> "one"; default -> "other"; };
  }
  static String stringAndNull(int x) {
    return switch (x) { case 1 -> "one"; case 2 -> null; default -> "other"; };
  }
  static String onlyNull(int x) {
    return switch (x) { case 1 -> null; default -> null; };
  }
  static String receiverCall(int x) {
    return (switch (x) { case 1 -> "One"; default -> "Two"; }).toLowerCase(java.util.Locale.ROOT);
  }
}
`

const compatibilityModernSource = `
import java.util.List;
public class ModernRegression {
  static int denseNoDebug(int x) {
    return switch (x) {
      case 0 -> 10; case 1 -> 11; case 2 -> 12; case 3 -> 13; default -> -1;
    };
  }
  static int sparseNoDebug(int x) {
    return switch (x) { case 7 -> 700; case 70 -> 7000; default -> 1; };
  }
  static long wideNoDebug(int x) {
    long value = switch (x) { case 1 -> 9876543210L; default -> 3L; };
    return value + 1;
  }
  static String nullNoDebug(int x) {
    return switch (x) { case 1 -> "x"; default -> null; };
  }
  static int nestedNoDebug(int x, int y) {
    int value = switch (x) {
      case 1 -> switch (y) { case 2 -> 22; default -> 23; };
      default -> 31;
    };
    return value;
  }
}
`

const legacyCasesSource = `
public class LegacyCases {
  static void mark(int x) {}
  static void fallThrough(int x) {
    switch (x) {
      case 1:
        mark(1);
      case 2:
        mark(2);
        break;
      default:
        mark(7);
        break;
    }
  }
  static int breakInSwitch(int x) {
    int result = 0;
    switch (x) {
      case 1: result = 10; break;
      case 2: result = 20; break;
      default: result = 30; break;
    }
    return result;
  }
  static int switchInLoop(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
      if (i == 2) { continue; }
      switch (i) {
        case 0: sum += 1; break;
        default: sum += 2; break;
      }
    }
    return sum;
  }
  static void continueStatement(int i) {
    do {
      mark(i + 1);
      if (i == 1) { continue; }
      if (i == 2) { continue; }
      mark(i + 2);
    } while (i++ > 5 && i < 10);
  }
}
`

type switchInfo struct {
	id     int
	parent int
	arms   int
	typ    string
}

type yieldInfo struct {
	switchID   int
	typ        string
	expression string
}

type armInfo struct {
	switchID int
	index    int
	labels   string
	kind     string
	top      string
}

type astReport struct {
	raw               string
	switches          []switchInfo
	yields            []yieldInfo
	arms              []armInfo
	methodSwitches    int
	statementSwitches int
	yieldCount        int
	breakCount        int
	continueCount     int
}

func TestSwitchStackMerges(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		classes := compileFixture(t, c, "StackCases", stackCasesSource, true)
		if classes == "" {
			return
		}

		expectedArms := map[string]int{
			"direct": 3, "assigned": 2, "arithmetic": 2, "bothOperands": 2,
			"leftOperand": 2,
			"argument":    2, "receiverPrefix": 2, "sharedLabels": 3, "dense": 6,
			"sparse": 4, "ifExits": 2, "nested": 2, "convergence": 5,
		}

		for method, arms := range expectedArms {
			source := decompileClass(t, c, classes, "StackCases")
			report := probeMethod(t, c, classes, "StackCases", method)
			c.Check(strings.Contains(source, "switch ("), method+": source contains switch")
			expectedSwitches := 1
			if method == "nested" || method == "bothOperands" {
				expectedSwitches = 2
			}
			c.Check(report.methodSwitches == expectedSwitches, method+": exactly expected expression switch count")
			c.Check(report.statementSwitches == 0, method+": no statement switch")
			c.Check(len(report.switches) >= 1, method+": AST switch present")
			if len(report.switches) > 0 {
				expectedType := "I"
				if method == "receiverPrefix" {
					expectedType = "Ljava/lang/Integer;"
				}
				c.Check(report.switches[0].typ == expectedType, method+": expression type is "+expectedType+", got "+report.switches[0].typ)
				c.Check(report.switches[0].arms == arms, method+": arm count")
			}
			recompileSource(t, c, "StackCases", source)
		}

		shared := probeMethod(t, c, classes, "StackCases", "sharedLabels")
		c.Check(armLabels(shared, 0, 0) == "1|2|3", "shared labels are grouped without copying body: "+armLabels(shared, 0, 0))
		c.Check(armLabels(shared, 0, 1) == "4|5", "second shared label group: "+armLabels(shared, 0, 1))

		convergence := probeMethod(t, c, classes, "StackCases", "convergence")
		c.Check(armByIndex(convergence, 0, 3).kind == "throw", "throwing convergence arm is not treated as a result")
		c.Check(len(yieldsForSwitch(convergence, 0)) == 4, "three normal convergence arms plus default yield results")

		ifs := probeMethod(t, c, classes, "StackCases", "ifExits")
		c.Check(len(ifs.yields) == 3, "if arm and default contribute three yields")
		for _, y := range ifs.yields {
			c.Check(y.switchID == 0, "if-exit yield belongs to outer switch")
		}
		c.Check(armByIndex(ifs, 0, 0).kind == "block", "if arm is a block")
		c.Check(strings.Contains(armByIndex(ifs, 0, 0).top, "IfStatement"), "if arm retains IfStatement")

		nested := probeMethod(t, c, classes, "StackCases", "nested")
		c.Check(len(nested.switches) == 2, "nested AST contains two switches")
		if len(nested.switches) == 2 {
			c.Check(nested.switches[0].parent == -1, "outer switch has no parent")
			c.Check(nested.switches[1].parent == 0, "inner switch yield is owned by inner switch")
		}
		innerYields := yieldsForSwitch(nested, 1)
		c.Check(len(innerYields) == 2, "inner switch owns two yields")
		outerYields := yieldsForSwitch(nested, 0)
		c.Check(len(outerYields) == 2, "outer switch owns two yields")
		if len(outerYields) > 0 {
			c.Check(outerYields[0].expression == "SwitchExpression", "outer arm contains the inner SwitchExpression")
		}

		javap := runTool(t, c, "javap", "-c", "-p", "-classpath", classes, "StackCases")
		c.Check(strings.Contains(javap, "tableswitch"), "dense fixture uses tableswitch")
		c.Check(strings.Contains(javap, "lookupswitch"), "sparse fixture uses lookupswitch")
	})
}

func TestSwitchTypes(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		classes := compileFixture(t, c, "TypeCases", typeCasesSource, true)
		if classes == "" {
			return
		}

		source := decompileClass(t, c, classes, "TypeCases")
		recompileSource(t, c, "TypeCases", source)

		intReport := probeMethod(t, c, classes, "TypeCases", "intValue")
		c.Check(len(intReport.switches) == 1 && intReport.switches[0].typ == "I", "int switch has type I")
		for _, y := range intReport.yields {
			c.Check(y.typ == "I", "int yield has target type I")
		}

		longReport := probeMethod(t, c, classes, "TypeCases", "longValue")
		c.Check(len(longReport.switches) == 1 && longReport.switches[0].typ == "J", "long switch has two-slot type J")
		c.Check(strings.Contains(source, "123456789012L"), "wide long constant is preserved")
		for _, y := range longReport.yields {
			c.Check(y.typ == "J", "long yield has target type J")
		}

		mixed := probeMethod(t, c, classes, "TypeCases", "mixedWide")
		c.Check(len(mixed.switches) == 1 && mixed.switches[0].typ == "J", "mixed int/long switch promotes to J")

		str := probeMethod(t, c, classes, "TypeCases", "stringValue")
		c.Check(len(str.switches) == 1 && str.switches[0].typ == "Ljava/lang/String;", "String switch has String type")

		nullMixed := probeMethod(t, c, classes, "TypeCases", "stringAndNull")
		c.Check(len(nullMixed.switches) == 1 && nullMixed.switches[0].typ == "Ljava/lang/String;", "String/null switch has String target type")
		sawNull := false
		for _, y := range nullMixed.yields {
			c.Check(y.typ == "Ljava/lang/String;", "null yield receives String target type")
			if y.expression == "NullExpression" {
				sawNull = true
			}
		}
		c.Check(sawNull, "AST retains a NullExpression")

		onlyNull := probeMethod(t, c, classes, "TypeCases", "onlyNull")
		c.Check(len(onlyNull.switches) == 1 && onlyNull.switches[0].typ == "Ljava/lang/String;", "all-null switch receives consuming String type")
		for _, y := range onlyNull.yields {
			c.Check(y.typ == "Ljava/lang/String;", "all-null yield receives String target type")
		}

		receiver := probeMethod(t, c, classes, "TypeCases", "receiverCall")
		c.Check(len(receiver.switches) == 1 && receiver.switches[0].typ == "Ljava/lang/String;", "switch used as method receiver remains a typed expression")
		c.Check(strings.Contains(source, ").toLowerCase("), "receiver switch is parenthesized for javac")
	})
}

func TestSwitchCompatibility(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		root := repoRoot(t)
		fixtureClasses := filepath.Join(root, ".task-build", "fixtures")

		modernSource := decompilePath(t, c, fixtureClasses, "ModernSwitch")
		modernAst := probePath(t, c, fixtureClasses, "ModernSwitch", "choose")
		c.Check(strings.Contains(modernSource, "->"), "checked-in modern fixture uses arrow arms")
		c.Check(!strings.Contains(modernSource, "MODERN_CURRENT_OUTPUT") && !strings.Contains(modernSource, "Byte code:"), "modern fixture has no fallback placeholder")
		c.Check(modernAst.methodSwitches == 1, "checked-in modern fixture has one expression switch")
		c.Check(modernAst.statementSwitches == 0, "checked-in modern fixture is not a statement switch")
		c.Check(len(modernAst.switches) == 1 && modernAst.switches[0].typ == "I", "checked-in modern switch is typed int")
		recompileSource(t, c, "ModernSwitch", modernSource)

		legacySource := decompilePath(t, c, fixtureClasses, "LegacySwitch")
		legacyAst := probePath(t, c, fixtureClasses, "LegacySwitch", "choose")
		c.Check(len(legacyAst.switches) == 0, "legacy switch is not converted to SwitchExpression")
		c.Check(legacyAst.statementSwitches >= 1, "legacy switch remains a statement")
		c.Check(strings.Contains(legacySource, "case 1:"), "legacy case keeps colon syntax")
		c.Check(strings.Contains(legacySource, "break;"), "legacy break remains")
		recompileSource(t, c, "LegacySwitch", legacySource)

		modernClasses := compileFixture(t, c, "ModernRegression", compatibilityModernSource, false)
		if modernClasses != "" {
			source := decompileClass(t, c, modernClasses, "ModernRegression")
			c.Check(!strings.Contains(source, "Byte code:"), "no-debug modern cases do not fall back to bytecode")
			recompileSource(t, c, "ModernRegression", source)

			for _, method := range []string{"denseNoDebug", "sparseNoDebug", "wideNoDebug", "nullNoDebug"} {
				report := probeMethod(t, c, modernClasses, "ModernRegression", method)
				c.Check(report.methodSwitches == 1, method+": no-debug expression switch recovered")
				c.Check(report.statementSwitches == 0, method+": no statement switch")
			}
			nestedCount := probeMethod(t, c, modernClasses, "ModernRegression", "nestedNoDebug")
			c.Check(nestedCount.methodSwitches == 2, "nestedNoDebug: both expression switches recovered")
			c.Check(nestedCount.statementSwitches == 0, "nestedNoDebug: no statement switch")
			wide := probeMethod(t, c, modernClasses, "ModernRegression", "wideNoDebug")
			c.Check(len(wide.switches) == 1 && wide.switches[0].typ == "J", "no-debug wide result keeps J type")
			nullReport := probeMethod(t, c, modernClasses, "ModernRegression", "nullNoDebug")
			c.Check(len(nullReport.switches) == 1 && nullReport.switches[0].typ == "Ljava/lang/String;", "no-debug null target keeps String type")
			nested := probeMethod(t, c, modernClasses, "ModernRegression", "nestedNoDebug")
			c.Check(len(nested.switches) == 2, "no-debug nested switch recovered")
		}

		legacyClasses := compileFixture(t, c, "LegacyCases", legacyCasesSource, false)
		if legacyClasses != "" {
			source := decompileClass(t, c, legacyClasses, "LegacyCases")
			recompileSource(t, c, "LegacyCases", source)

			fall := probeMethod(t, c, legacyClasses, "LegacyCases", "fallThrough")
			c.Check(len(fall.switches) == 0 && fall.statementSwitches == 1, "fall-through remains a statement switch")
			breakReport := probeMethod(t, c, legacyClasses, "LegacyCases", "breakInSwitch")
			c.Check(len(breakReport.switches) == 0 && breakReport.statementSwitches == 1 && breakReport.breakCount >= 3, "ordinary switch breaks remain statement breaks")
			continueReport := probeMethod(t, c, legacyClasses, "LegacyCases", "switchInLoop")
			c.Check(len(continueReport.switches) == 0, "loop switch is not converted")
			c.Check(continueReport.statementSwitches == 1, "loop switch remains a statement switch")
			continueOnly := probeMethod(t, c, legacyClasses, "LegacyCases", "continueStatement")
			c.Check(continueOnly.continueCount >= 1, "loop continue remains continue")
			c.Check(strings.Contains(source, "continue;"), "source retains continue statement")
		}
	})
}

func compileFixture(t *testing.T, c *taskcheck.C, className, source string, debug bool) string {
	t.Helper()
	dir := t.TempDir()
	src := filepath.Join(dir, className+".java")
	if err := os.WriteFile(src, []byte(source), 0o644); err != nil {
		c.Check(false, "write fixture: %v", err)
		return ""
	}
	out := filepath.Join(dir, "classes")
	if err := os.MkdirAll(out, 0o755); err != nil {
		c.Check(false, "classes dir: %v", err)
		return ""
	}
	args := []string{"--release", "17", "-proc:none"}
	if debug {
		args = append(args, "-g")
	} else {
		args = append(args, "-g:none")
	}
	args = append(args, "-encoding", "UTF-8", "-d", out, src)
	runTool(t, c, "javac", args...)
	return out
}

func recompileSource(t *testing.T, c *taskcheck.C, className, source string) {
	t.Helper()
	if source == "" {
		c.Check(false, className+": empty decompiled source")
		return
	}
	dir := t.TempDir()
	src := filepath.Join(dir, className+".java")
	if err := os.WriteFile(src, []byte(source), 0o644); err != nil {
		c.Check(false, "write decompiled source: %v", err)
		return
	}
	out := filepath.Join(dir, "out")
	if err := os.MkdirAll(out, 0o755); err != nil {
		c.Check(false, "recompile classes dir: %v", err)
		return
	}
	output := runToolAllowFailure(t, "javac", "--release", "17", "-proc:none", "-encoding", "UTF-8", "-d", out, src)
	c.Check(!strings.Contains(output, "error:"), className+": decompiled source recompiles\n"+output)
}

func decompileClass(t *testing.T, c *taskcheck.C, classes, className string) string {
	t.Helper()
	return runJavaProbe(t, c, "task_support.Analyze", classes, className)
}

func decompilePath(t *testing.T, c *taskcheck.C, classes, internalName string) string {
	t.Helper()
	return runJavaProbe(t, c, "task_support.Analyze", classes, internalName)
}

func probeMethod(t *testing.T, c *taskcheck.C, classes, className, method string) astReport {
	t.Helper()
	return probePath(t, c, classes, className, method)
}

func probePath(t *testing.T, c *taskcheck.C, classes, internalName, method string) astReport {
	t.Helper()
	output := runJavaProbe(t, c, "task_support.SwitchAstProbe", classes, internalName, method)
	report := parseAst(output)
	c.Check(strings.HasPrefix(output, "METHOD "), "AST probe produced report for "+internalName+"."+method)
	return report
}

func runJavaProbe(t *testing.T, c *taskcheck.C, mainClass string, args ...string) string {
	t.Helper()
	root := repoRoot(t)
	separator := string(os.PathListSeparator)
	classpath := filepath.Join(root, ".task-build", "classes") + separator + filepath.Join(root, ".task-build", "analyzer")
	fullArgs := append([]string{"-cp", classpath, mainClass}, args...)
	cmd := exec.Command("java", fullArgs...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	stdout, err := cmd.Output()
	if err != nil {
		c.Check(false, "java probe failed: %v\n%s", err, stderr.String())
		return ""
	}
	return string(stdout)
}

func runTool(t *testing.T, c *taskcheck.C, name string, args ...string) string {
	t.Helper()
	cmd := exec.Command(name, args...)
	var combined bytes.Buffer
	cmd.Stdout = &combined
	cmd.Stderr = &combined
	err := cmd.Run()
	output := combined.String()
	if err != nil {
		c.Check(false, name+" failed: "+err.Error()+"\n"+output)
	}
	return output
}

func runToolAllowFailure(t *testing.T, name string, args ...string) string {
	cmd := exec.Command(name, args...)
	var combined bytes.Buffer
	cmd.Stdout = &combined
	cmd.Stderr = &combined
	if err := cmd.Run(); err != nil {
		return combined.String() + "\n" + err.Error()
	}
	return combined.String()
}

func repoRoot(t *testing.T) string {
	t.Helper()
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	if filepath.Base(wd) == "task_support" {
		return filepath.Dir(wd)
	}
	return wd
}

func parseAst(output string) astReport {
	report := astReport{raw: output}
	for _, line := range strings.Split(output, "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "METHOD ") {
			re := regexp.MustCompile(`switches=(\d+).*statementSwitches=(\d+).*yields=(\d+).*breaks=(\d+).*continues=(\d+)`)
			m := re.FindStringSubmatch(line)
			if m != nil {
				report.methodSwitches, _ = strconv.Atoi(m[1])
				report.statementSwitches, _ = strconv.Atoi(m[2])
				report.yieldCount, _ = strconv.Atoi(m[3])
				report.breakCount, _ = strconv.Atoi(m[4])
				report.continueCount, _ = strconv.Atoi(m[5])
			}
		} else if strings.HasPrefix(line, "SWITCH ") {
			var id, parent, arms int
			var typ string
			parts := strings.Fields(line)
			for _, p := range parts {
				kv := strings.SplitN(p, "=", 2)
				if len(kv) != 2 {
					continue
				}
				switch kv[0] {
				case "id":
					id, _ = strconv.Atoi(kv[1])
				case "parent":
					parent, _ = strconv.Atoi(kv[1])
				case "arms":
					arms, _ = strconv.Atoi(kv[1])
				case "type":
					typ = kv[1]
				}
			}
			report.switches = append(report.switches, switchInfo{id: id, parent: parent, arms: arms, typ: typ})
		} else if strings.HasPrefix(line, "YIELD ") {
			var sw int
			var typ, expression string
			for _, p := range strings.Fields(line) {
				kv := strings.SplitN(p, "=", 2)
				if len(kv) != 2 {
					continue
				}
				switch kv[0] {
				case "switch":
					sw, _ = strconv.Atoi(kv[1])
				case "type":
					typ = kv[1]
				case "expression":
					expression = kv[1]
				}
			}
			report.yields = append(report.yields, yieldInfo{switchID: sw, typ: typ, expression: expression})
		} else if strings.HasPrefix(line, "ARM ") {
			var sw, index int
			var labels, kind, top string
			if i := strings.Index(line, " labels="); i >= 0 {
				rest := line[i+len(" labels="):]
				if j := strings.Index(rest, " kind="); j >= 0 {
					labels = rest[:j]
					rest = rest[j+len(" kind="):]
					if k := strings.Index(rest, " top="); k >= 0 {
						kind = rest[:k]
						top = rest[k+len(" top="):]
					} else {
						kind = rest
					}
				}
			}
			for _, p := range strings.Fields(line) {
				kv := strings.SplitN(p, "=", 2)
				if len(kv) == 2 && kv[0] == "switch" {
					sw, _ = strconv.Atoi(kv[1])
				}
				if len(kv) == 2 && kv[0] == "index" {
					index, _ = strconv.Atoi(kv[1])
				}
			}
			report.arms = append(report.arms, armInfo{switchID: sw, index: index, labels: labels, kind: kind, top: top})
		}
	}
	return report
}

func armByIndex(report astReport, sw, index int) armInfo {
	for _, arm := range report.arms {
		if arm.switchID == sw && arm.index == index {
			return arm
		}
	}
	return armInfo{}
}

func armLabels(report astReport, sw, index int) string {
	return armByIndex(report, sw, index).labels
}

func yieldsForSwitch(report astReport, sw int) []yieldInfo {
	var result []yieldInfo
	for _, y := range report.yields {
		if y.switchID == sw {
			result = append(result, y)
		}
	}
	return result
}
