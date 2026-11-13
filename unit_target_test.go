package expect

import (
	"bytes"
	"errors"
	"io"
	"os"
	"reflect"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/Netflix/go-expect/support/unitcheck"
)

// chunkReader hands out one pre-split chunk per Read so a PassthroughPipe and
// bufio.Reader are forced to deal with fragmented input.
type chunkReader struct {
	chunks [][]byte
	pos    int
}

func (r *chunkReader) Read(p []byte) (int, error) {
	if r.pos >= len(r.chunks) {
		return 0, io.EOF
	}
	b := r.chunks[r.pos]
	r.pos++
	return copy(p, b), nil
}

func runeChunks(s string) *chunkReader {
	var chunks [][]byte
	for _, r := range s {
		chunks = append(chunks, []byte(string(r)))
	}
	return &chunkReader{chunks: chunks}
}

func byteChunks(s string) *chunkReader {
	chunks := make([][]byte, 0, len(s))
	for i := 0; i < len(s); i++ {
		chunks = append(chunks, []byte{s[i]})
	}
	return &chunkReader{chunks: chunks}
}

type errWriter struct {
	err error
}

func (w errWriter) Write(p []byte) (int, error) {
	return 0, w.err
}

// trackingMatcher is a custom Matcher used to prove that option sets that
// cannot be compiled keep the original per-rune Match call order and count.
type trackingMatcher struct {
	target string
	calls  int
}

func (m *trackingMatcher) Match(v interface{}) bool {
	m.calls++
	buf, ok := v.(*bytes.Buffer)
	if !ok {
		return false
	}
	return strings.Contains(buf.String(), m.target)
}

func (m *trackingMatcher) Criteria() interface{} {
	return m.target
}

// refLeaf / refAll are an independent reference model. They deliberately
// enumerate every rune prefix and re-evaluate the whole tree with
// strings.Contains / regexp, mirroring what the old per-rune Match did, so
// the plan's completion timing can be checked against something other than
// its own implementation.
type refLeaf struct {
	lit string
	re  *regexp.Regexp
}

func (l refLeaf) satisfied(prefix string) bool {
	if l.re != nil {
		return l.re.MatchString(prefix)
	}
	return strings.Contains(prefix, l.lit)
}

type refAll struct {
	leaves []refLeaf
	inner  []*refAll
}

// at returns the smallest rune-prefix length at which the All is satisfied,
// or -1 if it never is. Empty All nodes are true on the first prefix, exactly
// as the original allMatcher is after the first rune is read.
func (a *refAll) at(runes []rune) int {
	if len(a.leaves) == 0 && len(a.inner) == 0 {
		if len(runes) == 0 {
			return -1
		}
		return 1
	}
	best := 0
	for _, leaf := range a.leaves {
		at := -1
		for i := 1; i <= len(runes); i++ {
			if leaf.satisfied(string(runes[:i])) {
				at = i
				break
			}
		}
		if at < 0 {
			return -1
		}
		if at > best {
			best = at
		}
	}
	for _, child := range a.inner {
		at := child.at(runes)
		if at < 0 {
			return -1
		}
		if at > best {
			best = at
		}
	}
	return best
}

func specOpt(a *refAll) ExpectOpt {
	var opts []ExpectOpt
	for _, leaf := range a.leaves {
		if leaf.re != nil {
			opts = append(opts, Regexp(leaf.re))
		} else {
			opts = append(opts, String(leaf.lit))
		}
	}
	for _, child := range a.inner {
		opts = append(opts, specOpt(child))
	}
	return All(opts...)
}

// referencePick evaluates top-level specs in declaration order against every
// prefix and returns the selected index and prefix length.
func referencePick(specs []*refAll, s string) (int, int) {
	runes := []rune(s)
	bestIndex, bestAt := -1, len(runes)+1
	for i, spec := range specs {
		at := spec.at(runes)
		if at >= 0 && at < bestAt {
			bestIndex, bestAt = i, at
		}
	}
	return bestIndex, bestAt
}

// assertPlanBasics is invoked directly by each top-level test so unitcheck.Run
// records the top-level UNIT_DONE marker as well as the subtest ones. It also
// pins down the compile-time contract shared by every test group.
func assertPlanBasics(t *testing.T) {
	unitcheck.Run(t, func(C *unitcheck.C) {
		plan, err := CompileExpect(String("marker"), WithTimeout(time.Second))
		C.Check(t, err == nil && plan != nil, "compile built-ins: %v", err)
		C.Check(t, plan.readTimeout != nil, "WithTimeout must be captured at compile time")

		_, err = CompileExpect(func(o *ExpectOpts) error {
			o.Matchers = append(o.Matchers, &trackingMatcher{target: "x"})
			return nil
		})
		C.Check(t, err != nil, "custom matcher must fail CompileExpect")
	})
}

func TestExpectPlanLogic(t *testing.T) {
	assertPlanBasics(t)
	t.Run("prefix_model_parity", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			specs := []*refAll{
				{leaves: []refLeaf{{lit: "ready"}, {lit: "y>"}}},
				{inner: []*refAll{{leaves: []refLeaf{{lit: "ad"}, {lit: "done"}}}}},
				{leaves: []refLeaf{{lit: "world"}, {lit: "Hello"}}},
				{leaves: []refLeaf{{re: regexp.MustCompile(`d[o]+ne`)}}},
			}
			input := "Hello world ready> done"

			var opts []ExpectOpt
			for _, spec := range specs {
				opts = append(opts, specOpt(spec))
			}
			plan, err := CompileExpect(opts...)
			C.Check(t, err == nil, "compile: %v", err)

			console, err := baselineConsole(runeChunks(input))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()

			out, stats, err := console.ExpectPlan(plan)
			C.Check(t, err == nil, "expect plan: %v", err)

			idx, at := referencePick(specs, input)
			C.Check(t, idx == 2, "reference selected index = %d", idx)
			want := string([]rune(input)[:at])
			C.Check(t, out == want, "out=%q want=%q", out, want)
			C.Check(t, out == "Hello world", "out=%q", out)
			C.Check(t, stats.DecodedBytes == uint64(len(out)), "decoded bytes=%d want=%d", stats.DecodedBytes, len(out))
			C.Check(t, stats.RegexChecks > 0, "regex leaf was never checked")
		})
	})

	t.Run("all_children_complete_on_different_prefixes", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			spec := &refAll{leaves: []refLeaf{{lit: "a"}, {lit: "bb"}, {lit: "ccc"}}}
			plan, err := CompileExpect(specOpt(spec))
			C.Check(t, err == nil, "compile: %v", err)

			for _, input := range []string{"accbbccc", "cccbbax", "acccbb"} {
				runes := []rune(input)
				wantAt := spec.at(runes)
				console, err := baselineConsole(runeChunks(input))
				C.Check(t, err == nil, "console: %v", err)
				out, _, err := console.ExpectPlan(plan)
				C.Check(t, err == nil, "expect %q: %v", input, err)
				C.Check(t, out == string(runes[:wantAt]), "input=%q out=%q want prefix len %d", input, out, wantAt)
				console.Close()
			}
		})
	})

	t.Run("top_level_priority_on_shared_prefix", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			// Both top-level conditions first become true at prefix "ab";
			// callbacks make the declaration-order tie-break observable.
			var fired []string
			plan, err := CompileExpect(
				String("ab").Then(func(*bytes.Buffer) error {
					fired = append(fired, "first")
					return nil
				}),
				All(String("a"), String("b")).Then(func(*bytes.Buffer) error {
					fired = append(fired, "second")
					return nil
				}),
			)
			C.Check(t, err == nil, "compile: %v", err)

			console, err := baselineConsole(runeChunks("ab"))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()

			out, _, err := console.ExpectPlan(plan)
			C.Check(t, err == nil, "expect plan: %v", err)
			C.Check(t, out == "ab", "out=%q", out)
			C.Check(t, reflect.DeepEqual(fired, []string{"first"}), "only the first declared top-level Then runs, got %v", fired)

			// An earlier matching prefix still beats a later one.
			plan2, err := CompileExpect(String("a"), String("ab"))
			C.Check(t, err == nil, "compile: %v", err)
			console2, err := baselineConsole(runeChunks("ab"))
			C.Check(t, err == nil, "console: %v", err)
			defer console2.Close()
			out2, _, err := console2.ExpectPlan(plan2)
			C.Check(t, err == nil, "expect plan: %v", err)
			C.Check(t, out2 == "a", "earlier prefix must win, got %q", out2)
		})
	})

	t.Run("repeated_positions_complete_independently", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			plan, err := CompileExpect(All(String("x"), String("x")))
			C.Check(t, err == nil, "compile: %v", err)

			console, err := baselineConsole(runeChunks("xx"))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()

			out, stats, err := console.ExpectPlan(plan)
			C.Check(t, err == nil, "expect plan: %v", err)
			C.Check(t, out == "x", "both declarations share the first occurrence, got %q", out)
			// Two leaf positions, one notification each; the second 'x' must not
			// re-notify either satisfied leaf.
			C.Check(t, stats.LiteralSignals == 2, "signals=%d want 2", stats.LiteralSignals)
		})
	})

	t.Run("empty_string_and_empty_all_wait_for_first_rune", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			plan, err := CompileExpect(String(""), All())
			C.Check(t, err == nil, "compile: %v", err)

			console, err := baselineConsole(runeChunks("ab"))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()

			out, _, err := console.ExpectPlan(plan)
			C.Check(t, err == nil, "expect plan: %v", err)
			C.Check(t, out == "a", "empty conditions must not succeed before the first rune, got %q", out)

			plan2, err := CompileExpect(All(String(""), String("b")))
			C.Check(t, err == nil, "compile: %v", err)
			console2, err := baselineConsole(runeChunks("ab"))
			C.Check(t, err == nil, "console: %v", err)
			defer console2.Close()
			out2, _, err := console2.ExpectPlan(plan2)
			C.Check(t, err == nil, "expect plan: %v", err)
			C.Check(t, out2 == "ab", "empty leaf at prefix 1, sibling leaf at prefix 2, got %q", out2)
		})
	})

	t.Run("regexp_leaves", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			plan, err := CompileExpect(
				All(String("Hello"), RegexpPattern(`wo[a-z]ld`)),
				Regexp(regexp.MustCompile(`END$`)),
			)
			C.Check(t, err == nil, "compile: %v", err)

			console, err := baselineConsole(runeChunks("Hello woxld END"))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()

			out, stats, err := console.ExpectPlan(plan)
			C.Check(t, err == nil, "expect plan: %v", err)
			C.Check(t, out == "Hello woxld", "all satisfied before the standalone regexp, got %q", out)
			// Two regexp leaves, both still necessary, one check each per rune.
			C.Check(t, stats.RegexChecks == 2*uint64(len([]rune("Hello woxld"))), "regex checks=%d", stats.RegexChecks)

			plan2, err := CompileExpect(RegexpPattern("c"))
			C.Check(t, err == nil, "compile: %v", err)
			console2, err := baselineConsole(runeChunks("abc"))
			C.Check(t, err == nil, "console: %v", err)
			defer console2.Close()
			out2, stats2, err := console2.ExpectPlan(plan2)
			C.Check(t, err == nil, "expect plan: %v", err)
			C.Check(t, out2 == "abc" && stats2.RegexChecks == 3, "out=%q checks=%d", out2, stats2.RegexChecks)
		})
	})

	t.Run("then_chain_order_and_errors", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			var order []string
			plan, err := CompileExpect(String("x").Then(func(*bytes.Buffer) error {
				order = append(order, "first")
				return nil
			}).Then(func(*bytes.Buffer) error {
				order = append(order, "second")
				return nil
			}))
			C.Check(t, err == nil, "compile: %v", err)

			console, err := baselineConsole(runeChunks("x"))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()
			out, _, err := console.ExpectPlan(plan)
			C.Check(t, err == nil, "expect plan: %v", err)
			C.Check(t, out == "x" && reflect.DeepEqual(order, []string{"first", "second"}), "out=%q order=%v", out, order)

			errFirst, errSecond := errors.New("first"), errors.New("second")
			var ran int
			plan2, err := CompileExpect(String("x").Then(func(*bytes.Buffer) error {
				ran++
				return errFirst
			}).Then(func(*bytes.Buffer) error {
				ran++
				return errSecond
			}))
			C.Check(t, err == nil, "compile: %v", err)
			console2, err := baselineConsole(runeChunks("xx"))
			C.Check(t, err == nil, "console: %v", err)
			defer console2.Close()
			out2, _, err2 := console2.ExpectPlan(plan2)
			C.Check(t, err2 == errFirst, "got err=%v", err2)
			C.Check(t, out2 == "x" && ran == 1, "out=%q ran=%d", out2, ran)

			// A Then nested inside an All must not run when the All itself is
			// selected: only the selected top-level matcher's chain runs.
			innerRan, outerRan := 0, 0
			plan3, err := CompileExpect(All(String("x").Then(func(*bytes.Buffer) error {
				innerRan++
				return nil
			})).Then(func(*bytes.Buffer) error {
				outerRan++
				return nil
			}))
			C.Check(t, err == nil, "compile: %v", err)
			console3, err := baselineConsole(runeChunks("x"))
			C.Check(t, err == nil, "console: %v", err)
			defer console3.Close()
			_, _, err = console3.ExpectPlan(plan3)
			C.Check(t, err == nil, "expect plan: %v", err)
			C.Check(t, innerRan == 0 && outerRan == 1, "inner=%d outer=%d", innerRan, outerRan)
		})
	})

	t.Run("custom_matcher_rejected_but_legacy_path_unchanged", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			custom := &trackingMatcher{target: "c"}
			customOpt := func(o *ExpectOpts) error {
				o.Matchers = append(o.Matchers, custom)
				return nil
			}
			plan, err := CompileExpect(customOpt)
			C.Check(t, err != nil && plan == nil, "plan=%v err=%v", plan, err)

			console, err := baselineConsole(runeChunks("abc"))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()
			out, err := console.Expect(customOpt)
			C.Check(t, err == nil, "expect: %v", err)
			C.Check(t, out == "abc", "out=%q", out)
			// One Match call per rune read; the match lands on the third rune.
			C.Check(t, custom.calls == 3, "custom Match calls=%d want 3", custom.calls)

			custom2 := &trackingMatcher{target: "z"}
			opt2 := func(o *ExpectOpts) error {
				o.Matchers = append(o.Matchers, custom2)
				return nil
			}
			console2, err := baselineConsole(runeChunks("ab"))
			C.Check(t, err == nil, "console: %v", err)
			defer console2.Close()
			out2, err2 := console2.Expect(opt2)
			C.Check(t, err2 == io.EOF, "err=%v", err2)
			C.Check(t, out2 == "ab", "out=%q", out2)
			// Two buffer evaluations plus the original error-path evaluation.
			C.Check(t, custom2.calls == 3, "custom Match calls=%d want 3", custom2.calls)
		})
	})

	t.Run("observer_snapshots_are_frozen", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			plan, err := CompileExpect(All(String("a"), String("b")), String("z"))
			C.Check(t, err == nil, "compile: %v", err)

			var observed []Matcher
			var observedBuf string
			var observedErr error
			calls := 0
			console, err := baselineConsole(runeChunks("a"))
			C.Check(t, err == nil, "console: %v", err)
			console.opts.ExpectObservers = append(console.opts.ExpectObservers, func(matchers []Matcher, buf string, err error) {
				calls++
				observed = matchers
				observedBuf = buf
				observedErr = err
			})
			defer console.Close()

			out, _, err := console.ExpectPlan(plan)
			C.Check(t, err == io.EOF, "err=%v", err)
			C.Check(t, out == "a" && observedBuf == "a", "out=%q buf=%q", out, observedBuf)
			C.Check(t, calls == 1, "observer calls=%d", calls)
			C.Check(t, len(observed) == 2, "active top-level matchers=%d", len(observed))

			// Parity with the legacy allMatcher after one matching prefix:
			// leaf "a" is gone from Criteria, leaf "b" remains.
			var legacy ExpectOpts
			C.Check(t, All(String("a"), String("b"))(&legacy) == nil, "legacy build")
			allM := legacy.Matchers[0].(*allMatcher)
			buf := new(bytes.Buffer)
			buf.WriteString("a")
			allM.Match(buf)
			C.Check(t, reflect.DeepEqual(observed[0].Criteria(), allM.Criteria()), "criteria=%v legacy=%v", observed[0].Criteria(), allM.Criteria())
			frozen := observed[0].Criteria()

			// A later, successful call must not mutate the earlier snapshot.
			console2, err := baselineConsole(runeChunks("ab"))
			C.Check(t, err == nil, "console: %v", err)
			defer console2.Close()
			success := 0
			console2.opts.ExpectObservers = append(console2.opts.ExpectObservers, func(matchers []Matcher, buf string, err error) {
				success++
				C.Check(t, len(matchers) == 1 && err == nil, "success observer state")
				crit := matchers[0].Criteria()
				_, isSlice := crit.([]interface{})
				C.Check(t, isSlice && len(crit.([]interface{})) == 0, "completed All criteria=%v", crit)
			})
			out2, _, err := console2.ExpectPlan(plan)
			C.Check(t, err == nil && out2 == "ab" && success == 1, "out=%q err=%v calls=%d", out2, err, success)
			C.Check(t, reflect.DeepEqual(observed[0].Criteria(), frozen), "snapshot mutated by later call")
			C.Check(t, observedErr == io.EOF, "observed error changed: %v", observedErr)
		})
	})
}

func TestExpectPlanLifecycle(t *testing.T) {
	assertPlanBasics(t)
	t.Run("shared_plan_runs_concurrently", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			plan, err := CompileExpect(All(String("foo"), String("bar")))
			C.Check(t, err == nil, "compile: %v", err)

			const workers = 8
			var wg sync.WaitGroup
			var mu sync.Mutex
			ok := true
			for i := 0; i < workers; i++ {
				wg.Add(1)
				go func() {
					defer wg.Done()
					observerCalls := 0
					console, cerr := baselineConsole(runeChunks("foobarbaz"))
					if cerr != nil {
						mu.Lock()
						ok = false
						mu.Unlock()
						return
					}
					console.opts.ExpectObservers = append(console.opts.ExpectObservers, func([]Matcher, string, error) {
						observerCalls++
					})
					out, stats, eerr := console.ExpectPlan(plan)
					console.Close()
					mu.Lock()
					if eerr != nil || out != "foobar" || stats.LiteralSignals != 2 || observerCalls != 1 {
						ok = false
					}
					mu.Unlock()
				}()
			}
			wg.Wait()
			C.Check(t, ok, "at least one concurrent ExpectPlan diverged")
		})
	})

	t.Run("interleaved_reuse_starts_fresh", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			plan, err := CompileExpect(All(String("foo"), String("bar")))
			C.Check(t, err == nil, "compile: %v", err)

			consoleA, err := baselineConsole(runeChunks("foobarXfoobarZZ"))
			C.Check(t, err == nil, "console: %v", err)
			defer consoleA.Close()
			consoleB, err := baselineConsole(runeChunks("foobar"))
			C.Check(t, err == nil, "console: %v", err)
			defer consoleB.Close()

			out1, st1, err := consoleA.ExpectPlan(plan)
			C.Check(t, err == nil && out1 == "foobar" && st1.LiteralSignals == 2, "A1: %q %d %v", out1, st1.LiteralSignals, err)
			out2, st2, err := consoleB.ExpectPlan(plan)
			C.Check(t, err == nil && out2 == "foobar" && st2.LiteralSignals == 2, "B1: %q %d %v", out2, st2.LiteralSignals, err)
			out3, st3, err := consoleA.ExpectPlan(plan)
			C.Check(t, err == nil && out3 == "Xfoobar" && st3.LiteralSignals == 2, "A2: %q %d %v", out3, st3.LiteralSignals, err)
		})
	})

	t.Run("matching_does_not_consume_the_tail", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			plan, err := CompileExpect(String("ab"))
			C.Check(t, err == nil, "compile: %v", err)
			planCD, err := CompileExpect(String("cd"))
			C.Check(t, err == nil, "compile: %v", err)
			planEOF, err := CompileExpect(EOF)
			C.Check(t, err == nil, "compile: %v", err)

			console, err := baselineConsole(runeChunks("abcdef"))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()

			out1, _, err := console.ExpectPlan(plan)
			C.Check(t, err == nil && out1 == "ab", "first: %q %v", out1, err)
			out2, _, err := console.ExpectPlan(planCD)
			C.Check(t, err == nil && out2 == "cd", "second: %q %v", out2, err)
			out3, _, err := console.ExpectPlan(planEOF)
			C.Check(t, err == nil && out3 == "ef", "eof prefix: %q %v", out3, err)
		})
	})

	t.Run("stdout_error_is_surfaced_and_plan_survives", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			writerErr := errors.New("stdout broken")
			plan, err := CompileExpect(String("z"))
			C.Check(t, err == nil, "compile: %v", err)

			console, err := baselineConsole(runeChunks("abc"))
			C.Check(t, err == nil, "console: %v", err)
			console.opts.Stdouts = []io.Writer{errWriter{err: writerErr}}
			defer console.Close()

			out, _, err := console.ExpectPlan(plan)
			C.Check(t, err == writerErr, "err=%v", err)
			C.Check(t, out == "", "nothing was buffered before the writer failed: %q", out)

			console2, err := baselineConsole(runeChunks("z"))
			C.Check(t, err == nil, "console: %v", err)
			defer console2.Close()
			out2, _, err := console2.ExpectPlan(plan)
			C.Check(t, err == nil && out2 == "z", "reused plan: %q %v", out2, err)
		})
	})

	t.Run("callback_error_does_not_poison_next_call", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			callbackErr := errors.New("callback failed")
			runs := 0
			plan, err := CompileExpect(All(String("a")).Then(func(*bytes.Buffer) error {
				runs++
				return callbackErr
			}))
			C.Check(t, err == nil, "compile: %v", err)

			console, err := baselineConsole(runeChunks("aa"))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()
			out1, _, err1 := console.ExpectPlan(plan)
			C.Check(t, err1 == callbackErr && out1 == "a", "first call: %q %v", out1, err1)

			console2, err := baselineConsole(runeChunks("aa"))
			C.Check(t, err == nil, "console: %v", err)
			defer console2.Close()
			out2, _, err2 := console2.ExpectPlan(plan)
			C.Check(t, err2 == callbackErr && out2 == "a", "fresh call must redo the All: %q %v", out2, err2)
			C.Check(t, runs == 2, "callback runs=%d want 2", runs)
		})
	})

	t.Run("eof_prefix_and_matched_error", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			plan, err := CompileExpect(String("never"))
			C.Check(t, err == nil, "compile: %v", err)
			console, err := baselineConsole(runeChunks("abc"))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()
			out, stats, err := console.ExpectPlan(plan)
			C.Check(t, err == io.EOF, "err=%v", err)
			C.Check(t, out == "abc" && stats.DecodedBytes == 3, "out=%q bytes=%d", out, stats.DecodedBytes)

			planEOF, err := CompileExpect(EOF, PTSClosed)
			C.Check(t, err == nil, "compile: %v", err)
			console2, err := baselineConsole(runeChunks("abc"))
			C.Check(t, err == nil, "console: %v", err)
			defer console2.Close()
			out2, _, err := console2.ExpectPlan(planEOF)
			C.Check(t, err == nil && out2 == "abc", "matched EOF: %q %v", out2, err)
			_, isPathError := planEOF.roots[1].criteria.(os.PathError)
			C.Check(t, isPathError, "PTSClosed compiled with criteria %T", planEOF.roots[1].criteria)

			sentinel := errors.New("pipe blew up")
			pr, pw := io.Pipe()
			console3, err := baselineConsole(pr)
			C.Check(t, err == nil, "console: %v", err)
			defer console3.Close()
			planErr, err := CompileExpect(Error(sentinel))
			C.Check(t, err == nil, "compile: %v", err)
			_, werr := pw.Write([]byte("hi"))
			C.Check(t, werr == nil, "pipe write: %v", werr)
			C.Check(t, pw.CloseWithError(sentinel) == nil, "close with error")
			out3, _, err := console3.ExpectPlan(planErr)
			C.Check(t, err == nil && out3 == "hi", "matched error condition: %q %v", out3, err)
		})
	})

	t.Run("timeout_returns_prefix_and_stats", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			pr, pw := io.Pipe()
			console, err := baselineConsole(pr)
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()
			defer pw.Close()

			plan, err := CompileExpect(String("never"), WithTimeout(30*time.Millisecond))
			C.Check(t, err == nil, "compile: %v", err)
			out, stats, err := console.ExpectPlan(plan)
			C.Check(t, err != nil && os.IsTimeout(err), "err=%v", err)
			C.Check(t, out == "" && stats.DecodedBytes == 0 && stats.LiteralWork == 0, "out=%q stats=%+v", out, stats)
		})
	})

	t.Run("utf8_fragments_and_invalid_bytes", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			// Multi-byte rune split across reads, plus an invalid byte.
			raw := string([]byte{'f', 0xc3, 0xa9, 0xff, 'g'})
			plan, err := CompileExpect(String("g"))
			C.Check(t, err == nil, "compile: %v", err)

			console, err := baselineConsole(byteChunks(raw))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()

			out, stats, err := console.ExpectPlan(plan)
			C.Check(t, err == nil, "expect plan: %v", err)
			want := "f" + "é" + string(utf8.RuneError) + "g"
			C.Check(t, out == want, "out=%q want=%q", out, want)
			// DecodedBytes counts the bytes actually written into the match
			// buffer; the invalid byte is re-encoded as U+FFFD (three bytes)
			// by WriteRune, exactly like the legacy Expect buffer.
			C.Check(t, stats.DecodedBytes == uint64(len(out)), "bytes=%d want=%d", stats.DecodedBytes, len(out))
			C.Check(t, stats.DecodedBytes == 7, "bytes=%d want 7", stats.DecodedBytes)
		})
	})
}

func TestExpectPlanWork(t *testing.T) {
	assertPlanBasics(t)
	t.Run("stats_counters", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			plan, err := CompileExpect(All(String("a"), String("c")))
			C.Check(t, err == nil, "compile: %v", err)

			console, err := baselineConsole(runeChunks("abc"))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()

			out, stats, err := console.ExpectPlan(plan)
			C.Check(t, err == nil && out == "abc", "out=%q err=%v", out, err)
			C.Check(t, stats.DecodedBytes == 3, "decoded=%d", stats.DecodedBytes)
			C.Check(t, stats.LiteralSignals == 2, "signals=%d", stats.LiteralSignals)
			C.Check(t, stats.RegexChecks == 0, "regex checks=%d", stats.RegexChecks)
			C.Check(t, stats.LiteralWork >= 3, "each rune needs at least one automaton lookup, got %d", stats.LiteralWork)
		})
	})

	t.Run("long_output_is_linear_not_pattern_times_length", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			run := func(padding int) (string, MatchStats) {
				var suffixes []string
				var opts []ExpectOpt
				for i := 0; i < 20; i++ {
					s := "STATE_" + string(rune('A'+i))
					suffixes = append(suffixes, s)
					opts = append(opts, String(s))
				}
				opts = append(opts, String("x"), String("FINAL_MARKER"))
				plan, err := CompileExpect(All(opts...))
				C.Check(t, err == nil, "compile: %v", err)

				input := strings.Join(suffixes, ",") + strings.Repeat("x", padding) + "FINAL_MARKER"
				console, err := baselineConsole(runeChunks(input))
				C.Check(t, err == nil, "console: %v", err)
				out, stats, err := console.ExpectPlan(plan)
				console.Close()
				C.Check(t, err == nil, "expect plan: %v", err)
				C.Check(t, out == input, "returned prefix diverged")
				return input, stats
			}

			input, stats := run(4000)
			_, statsHalf := run(2000)

			// 22 literal leaf positions; the 'x' leaf fires once although the
			// rune occurs thousands of times, and every other leaf fires once
			// at its single occurrence.
			C.Check(t, stats.LiteralSignals == 22, "signals=%d want 22", stats.LiteralSignals)
			C.Check(t, stats.DecodedBytes == uint64(len(input)), "decoded=%d want=%d", stats.DecodedBytes, len(input))

			n, m := uint64(len(input)), uint64(22)
			// The naive implementation performs up to n*m Contains scans.
			C.Check(t, stats.LiteralWork < n*m, "rescan bound violated: work=%d n*m=%d", stats.LiteralWork, n*m)
			// One transition plus at most one output-chain examination per
			// rune, with suffix fallback amortized: work stays linear.
			C.Check(t, stats.LiteralWork <= 3*n+100, "linear bound violated: work=%d n=%d", stats.LiteralWork, n)
			// Doubling only the padding length adds proportionally little work.
			C.Check(t, stats.LiteralWork-statsHalf.LiteralWork <= uint64(2000)*3+100,
				"extra work=%d for 2000 extra runes", stats.LiteralWork-statsHalf.LiteralWork)
		})
	})

	t.Run("regex_skips_completed_branches", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			// The inner All completes at the first rune; its regexp leaf must
			// not be re-checked while the outer All waits for the marker.
			plan, err := CompileExpect(All(
				All(RegexpPattern("a"), String("a")),
				String("MARKER"),
			))
			C.Check(t, err == nil, "compile: %v", err)

			input := "a" + strings.Repeat("x", 50) + "MARKER"
			console, err := baselineConsole(runeChunks(input))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()

			out, stats, err := console.ExpectPlan(plan)
			C.Check(t, err == nil, "expect plan: %v", err)
			C.Check(t, out == input, "out diverged")
			C.Check(t, stats.RegexChecks == 1, "regex checks=%d want 1", stats.RegexChecks)
			C.Check(t, stats.LiteralSignals == 3, "three leaf positions signaled, got %d", stats.LiteralSignals)
		})
	})

	t.Run("shared_suffix_output_chain", func(t *testing.T) {
		unitcheck.Run(t, func(C *unitcheck.C) {
			// "ab" ends with "b"; the suffix link at state "ab" must surface
			// both leaves in one output-chain examination, and a long tail of
			// repeated suffixes must not re-notify the satisfied positions.
			plan, err := CompileExpect(All(String("xab"), String("ab"), String("b")))
			C.Check(t, err == nil, "compile: %v", err)

			input := "xab" + strings.Repeat("b", 200)
			console, err := baselineConsole(runeChunks(input))
			C.Check(t, err == nil, "console: %v", err)
			defer console.Close()

			out, stats, err := console.ExpectPlan(plan)
			C.Check(t, err == nil, "expect plan: %v", err)
			C.Check(t, out == "xab", "all three leaves complete at the first xab prefix, got %q", out)
			C.Check(t, stats.LiteralSignals == 3, "each declared position signals once, got %d", stats.LiteralSignals)
			// Exactly three runes were processed to completion; the 200 tail
			// runes are never read (no rune past the matching prefix).
			C.Check(t, stats.DecodedBytes == 3, "decoded=%d want 3", stats.DecodedBytes)
			C.Check(t, stats.LiteralWork <= 9, "three runes cost a constant number of lookups, got %d", stats.LiteralWork)
		})
	})
}
