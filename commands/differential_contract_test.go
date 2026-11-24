// Contract tests for stateful differential command testing.
//
// All scenarios use benign in-memory handle objects: no crashes, no real
// services and no external modules.
package commands_test

import (
	"context"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/leanovate/gopter"
	"github.com/leanovate/gopter/commands"
	"github.com/leanovate/gopter/gen"
	"github.com/leanovate/gopter/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// Benign in-memory handle harness
// ---------------------------------------------------------------------------

type zdcStore struct {
	suite     *zdcSuite
	name      string
	prefix    string
	next      int
	live      map[string]bool
	allocated []string
	reuse     []string
}

func (s *zdcStore) countCall() {
	s.suite.calls[s.name]++
}

// alloc allocates a physical handle. Released strings are reused first, so a
// release followed by an allocation can deliberately reproduce the exact
// same physical string.
func (s *zdcStore) alloc() string {
	var physical string
	if n := len(s.reuse); n > 0 {
		physical = s.reuse[n-1]
		s.reuse = s.reuse[:n-1]
	} else {
		physical = fmt.Sprintf("%s%d", s.prefix, s.next)
		s.next++
	}
	s.live[physical] = true
	s.allocated = append(s.allocated, physical)
	return physical
}

func (s *zdcStore) recycle(physical string) {
	delete(s.live, physical)
	s.reuse = append(s.reuse, physical)
}

type zdcSuite struct {
	*commands.ProtoCommands

	names []string

	factorySeq   int
	creates      int
	closes       int
	createOrder  []string
	destroyOrder []string
	calls        map[string]int
	adapterErrs  int

	failFactoryFor string
}

func zdcNewSuite(names ...string) *zdcSuite {
	if len(names) == 0 {
		names = []string{"alpha", "beta"}
	}
	s := &zdcSuite{
		names: names,
		calls: map[string]int{},
	}
	s.ProtoCommands = &commands.ProtoCommands{
		InitialStateGen:         gen.Const(0),
		InitialPreConditionFunc: func(commands.State) bool { return true },
		GenCommandFunc:          func(commands.State) gopter.Gen { return gen.Fail(reflect.TypeOf((*commands.Command)(nil)).Elem()) },
	}
	return s
}

func (s *zdcSuite) ImplementationNames() []string {
	if s == nil {
		return nil
	}
	return s.names
}

// CloneState: the tests mostly use immutable int models; being explicit keeps
// the contract honest.
func (s *zdcSuite) CloneState(state commands.State) commands.State { return state }

func (s *zdcSuite) NewImplementation(name string, initial commands.State) (commands.SystemUnderTest, error) {
	s.creates++
	s.createOrder = append(s.createOrder, name)
	if s.failFactoryFor == name {
		return nil, errors.New("zdc scripted factory failure")
	}
	if initial.(int) != 0 {
		return nil, fmt.Errorf("zdc unexpected initial state %v", initial)
	}
	s.factorySeq++
	// Every fresh system gets its own run-specific numbering base and every
	// name gets a distinct prefix, so cross-run or cross-implementation map
	// leakage would produce visibly wrong resolutions.
	base := 1000*s.factorySeq + 1
	prefix := "zdc-a-"
	if len(s.names) > 0 && name != s.names[0] {
		prefix = "zdc-b-"
	}
	return &zdcStore{
		suite:  s,
		name:   name,
		prefix: prefix,
		next:   base,
		live:   map[string]bool{},
	}, nil
}

func (s *zdcSuite) DestroyImplementation(name string, _ commands.SystemUnderTest) {
	s.closes++
	s.destroyOrder = append(s.destroyOrder, name)
}

type zdcRunFunc func(store *zdcStore, bindings *commands.HandleBindings) (commands.DiffOutput, error)

type zdcCmd struct {
	*commands.ProtoCommand
	run zdcRunFunc
}

func (s *zdcSuite) newCmd(name string, run zdcRunFunc) *zdcCmd {
	return &zdcCmd{
		ProtoCommand: &commands.ProtoCommand{Name: name},
		run:          run,
	}
}

func (c *zdcCmd) RunDifferential(system commands.SystemUnderTest, bindings *commands.HandleBindings) (commands.DiffOutput, error) {
	return c.run(system.(*zdcStore), bindings)
}

func zdcScalar(text string) commands.DiffOutput {
	return commands.DiffOutput{Value: zdcText(text)}
}

func zdcText(text string) commands.DiffValue {
	return commands.DiffValue{Kind: commands.DiffScalar, Text: text}
}

func zdcObject(fields map[string]commands.DiffValue) commands.DiffValue {
	return commands.DiffValue{Kind: commands.DiffObject, Fields: fields}
}

func zdcCreate(physical string) commands.DiffValue {
	return commands.DiffValue{Kind: commands.DiffCreate, Text: physical}
}

func zdcRef(physical string) commands.DiffValue {
	return commands.DiffValue{Kind: commands.DiffRef, Text: physical}
}

func zdcLabelContains(result *gopter.PropResult, fragment string) bool {
	if result == nil {
		return false
	}
	for _, label := range result.Labels {
		if strings.Contains(label, fragment) {
			return true
		}
	}
	return false
}

// zdcForkOutput builds a genuine alias-graph difference: the first named
// implementation aliases y to x, the others allocate a distinct y handle.
// First differing canonical path is /y at command index 0.
func zdcForkOutput(store *zdcStore) commands.DiffOutput {
	x := store.alloc()
	if store.name == store.suite.names[0] {
		return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{
			"x": zdcCreate(x),
			"y": zdcRef(x),
		})}
	}
	y := store.alloc()
	return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{
		"x": zdcCreate(x),
		"y": zdcCreate(y),
	})}
}

// ---------------------------------------------------------------------------
// TestDiffHandles: normalization, identities, aliases and releases
// ---------------------------------------------------------------------------

func TestDiffHandles(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ctx := context.Background()

		// 1. Different physical numbering normalizes to equal identities,
		//    aliases stay aliases and committed bindings resolve exactly.
		suite := zdcNewSuite()
		createCmd := suite.newCmd("create-h", func(s *zdcStore, b *commands.HandleBindings) (commands.DiffOutput, error) {
			s.countCall()
			physical := s.alloc()
			return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{
				"h": zdcCreate(physical),
			})}, nil
		})
		useCmd := suite.newCmd("use-h", func(s *zdcStore, b *commands.HandleBindings) (commands.DiffOutput, error) {
			s.countCall()
			physical := s.allocated[0]
			resolved, ok := b.Resolve("0:/h")
			c.Check(ok && resolved == physical, "committed identity 0:/h must resolve to this store's physical handle")
			_, emptyOK := b.Resolve("")
			c.Check(!emptyOK, "empty identity must not resolve")
			_, unknownOK := b.Resolve("9:/missing")
			c.Check(!unknownOK, "unknown identity must not resolve")

			return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{
				"a":    zdcRef(physical),
				"b":    zdcCreate(physical), // second Create of same live physical = alias
				"list": {Kind: commands.DiffList, Items: []commands.DiffValue{zdcRef(physical), {Kind: commands.DiffScalar, Text: "tail"}}},
			})}, nil
		})
		result := commands.ReplayDifferential(ctx, suite, 0, createCmd, useCmd)
		c.Check(result != nil && result.Status == gopter.PropTrue, "different numbering with aliases must compare equal, got %+v", result)
		c.Check(suite.creates == 2 && suite.closes == 2, "both implementations must be created and closed once")
		c.Check(suite.calls["alpha"] == 2 && suite.calls["beta"] == 2, "each command must run on both implementations in order")

		// 2. Object key insertion order is irrelevant.
		keyOrderSuite := zdcNewSuite()
		keyOrderCmd := keyOrderSuite.newCmd("key-order", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			s.countCall()
			if s.name == "alpha" {
				return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{
					"z": {Kind: commands.DiffScalar, Text: "1"},
					"a": {Kind: commands.DiffScalar, Text: "2"},
					"m": {Kind: commands.DiffScalar, Text: "3"},
				})}, nil
			}
			return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{
				"a": {Kind: commands.DiffScalar, Text: "2"},
				"m": {Kind: commands.DiffScalar, Text: "3"},
				"z": {Kind: commands.DiffScalar, Text: "1"},
			})}, nil
		})
		keyOrderResult := commands.ReplayDifferential(ctx, keyOrderSuite, 0, keyOrderCmd)
		c.Check(keyOrderResult.Status == gopter.PropTrue, "object key order must not affect equality, got %+v", keyOrderResult)

		// 3. Alias fork is a real difference at canonical path /y.
		forkSuite := zdcNewSuite()
		forkCmd := forkSuite.newCmd("fork", func(s *zdcStore, b *commands.HandleBindings) (commands.DiffOutput, error) {
			s.countCall()
			// The snapshot of the last committed round must not contain
			// tentative identities from the current output.
			if _, ok := b.Resolve("0:/x"); ok {
				c.Check(false, "current-round Create identity must not be visible in bindings snapshot")
			}
			return zdcForkOutput(s), nil
		})
		forkResult := commands.ReplayDifferential(ctx, forkSuite, 0, forkCmd)
		c.Check(forkResult.Status == gopter.PropFalse, "alias fork must be PropFalse, got %+v", forkResult)
		c.Check(zdcLabelContains(forkResult, "command=0"), "label must identify command index 0: %v", forkResult.Labels)
		c.Check(zdcLabelContains(forkResult, `path="/y"`), "label must identify first differing path /y: %v", forkResult.Labels)
		c.Check(zdcLabelContains(forkResult, "0:/y"), "label should carry combined identity 0:/y: %v", forkResult.Labels)

		// 4. Lexical object key order decides which differing path is first.
		lexicalSuite := zdcNewSuite()
		lexicalCmd := lexicalSuite.newCmd("lexical", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			if s.name == "alpha" {
				return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{
					"b": {Kind: commands.DiffScalar, Text: "different-b"},
					"a": {Kind: commands.DiffScalar, Text: "different-a"},
				})}, nil
			}
			return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{
				"b": {Kind: commands.DiffScalar, Text: "x"},
				"a": {Kind: commands.DiffScalar, Text: "y"},
			})}, nil
		})
		lexicalResult := commands.ReplayDifferential(ctx, lexicalSuite, 0, lexicalCmd)
		c.Check(lexicalResult.Status == gopter.PropFalse && zdcLabelContains(lexicalResult, `path="/a"`),
			"lexically first differing key /a must be reported, got %+v", lexicalResult)

		// 5. Nested canonical paths and JSON pointer escaping.
		nestedSuite := zdcNewSuite()
		nestedCmd := nestedSuite.newCmd("nested", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			deep := func(text string) commands.DiffValue {
				return zdcObject(map[string]commands.DiffValue{"k": {Kind: commands.DiffScalar, Text: text}})
			}
			if s.name == "alpha" {
				return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{"outer": deep("one")})}, nil
			}
			return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{"outer": deep("two")})}, nil
		})
		nestedResult := commands.ReplayDifferential(ctx, nestedSuite, 0, nestedCmd)
		c.Check(nestedResult.Status == gopter.PropFalse && zdcLabelContains(nestedResult, `path="/outer/k"`),
			"nested path /outer/k must be reported, got %+v", nestedResult)

		escapeSuite := zdcNewSuite()
		escapeCmd := escapeSuite.newCmd("escape", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			key := "a/b~c"
			if s.name == "alpha" {
				return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{
					key: {Kind: commands.DiffScalar, Text: "one"},
				})}, nil
			}
			return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{
				key: {Kind: commands.DiffScalar, Text: "two"},
			})}, nil
		})
		escapeResult := commands.ReplayDifferential(ctx, escapeSuite, 0, escapeCmd)
		c.Check(escapeResult.Status == gopter.PropFalse && zdcLabelContains(escapeResult, `path="/a~1b~0c"`),
			"JSON Pointer escaped path /a~1b~0c must be reported, got %+v", escapeResult)

		// 6. Release then reallocate the same physical string: new identity,
		//    old identity dead, snapshot stays committed-only.
		reuseSuite := zdcNewSuite()
		reuseCreate := reuseSuite.newCmd("reuse-create", func(s *zdcStore, b *commands.HandleBindings) (commands.DiffOutput, error) {
			physical := s.alloc()
			return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{"h": zdcCreate(physical)})}, nil
		})
		reuseRelease := reuseSuite.newCmd("reuse-release", func(s *zdcStore, b *commands.HandleBindings) (commands.DiffOutput, error) {
			physical := s.allocated[0]
			if resolved, ok := b.Resolve("0:/h"); !ok || resolved != physical {
				c.Check(false, "before release commit 0:/h must be live in committed snapshot")
			}
			s.recycle(physical)
			// The released handle may be referenced in the same output.
			return commands.DiffOutput{
				Value:   zdcObject(map[string]commands.DiffValue{"gone": zdcRef(physical)}),
				Release: []string{physical},
			}, nil
		})
		reuseAlloc := reuseSuite.newCmd("reuse-alloc", func(s *zdcStore, b *commands.HandleBindings) (commands.DiffOutput, error) {
			physical := s.alloc() // recycles the exact old physical string
			c.Check(physical == s.allocated[0], "test store should reproduce the released physical string")
			if _, ok := b.Resolve("0:/h"); ok {
				c.Check(false, "released logical identity 0:/h must no longer resolve")
			}
			if _, ok := b.Resolve("2:/h2"); ok {
				c.Check(false, "uncommitted new identity must not leak into bindings snapshot")
			}
			return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{"h2": zdcCreate(physical)})}, nil
		})
		reuseVerify := reuseSuite.newCmd("reuse-verify", func(s *zdcStore, b *commands.HandleBindings) (commands.DiffOutput, error) {
			physical, ok := b.Resolve("2:/h2")
			c.Check(ok, "new identity 2:/h2 must resolve after commit")
			c.Check(physical == s.allocated[0], "new identity must bind the reused physical string")
			if _, oldOK := b.Resolve("0:/h"); oldOK {
				c.Check(false, "old identity must stay dead after reallocation")
			}
			return zdcScalar("verified"), nil
		})
		reuseResult := commands.ReplayDifferential(ctx, reuseSuite, 0, reuseCreate, reuseRelease, reuseAlloc, reuseVerify)
		c.Check(reuseResult.Status == gopter.PropTrue, "release+reallocate with new identity must compare equal, got %+v", reuseResult)

		// 7. Differing release sets are a PropFalse mentioning releases.
		releaseDiffSuite := zdcNewSuite()
		releaseCreate := releaseDiffSuite.newCmd("rd-create", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			p := s.alloc()
			return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{"h": zdcCreate(p)})}, nil
		})
		releaseCompare := releaseDiffSuite.newCmd("rd-release", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			p := s.allocated[0]
			if s.name == "alpha" {
				s.recycle(p)
				return commands.DiffOutput{Value: zdcText("ok"), Release: []string{p}}, nil
			}
			return zdcScalar("ok"), nil
		})
		releaseDiffResult := commands.ReplayDifferential(ctx, releaseDiffSuite, 0, releaseCreate, releaseCompare)
		c.Check(releaseDiffResult.Status == gopter.PropFalse, "different release sets must be PropFalse, got %+v", releaseDiffResult)
		c.Check(zdcLabelContains(releaseDiffResult, "release"), "label must mention release sets: %v", releaseDiffResult.Labels)

		// 8. Unknown references, unknown/duplicate releases and invalid
		//    shapes are adapter PropErrors carrying implementation and step.
		unknownRefSuite := zdcNewSuite()
		good := unknownRefSuite.newCmd("good", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			return zdcScalar("fine"), nil
		})
		badRef := unknownRefSuite.newCmd("bad-ref", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			return commands.DiffOutput{Value: zdcObject(map[string]commands.DiffValue{
				"x": zdcRef("never-allocated-physical"),
			})}, nil
		})
		unknownRefResult := commands.ReplayDifferential(ctx, unknownRefSuite, 0, good, badRef)
		c.Check(unknownRefResult.Status == gopter.PropError && unknownRefResult.Error != nil,
			"unknown reference must be PropError, got %+v", unknownRefResult)
		c.Check(unknownRefResult.Error != nil && strings.Contains(unknownRefResult.Error.Error(), "alpha"),
			"adapter error must include implementation name: %v", unknownRefResult.Error)
		c.Check(unknownRefResult.Error != nil && strings.Contains(unknownRefResult.Error.Error(), "step 1"),
			"adapter error must include step: %v", unknownRefResult.Error)

		unknownReleaseSuite := zdcNewSuite()
		unknownReleaseCmd := unknownReleaseSuite.newCmd("bad-release", func(_ *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			return commands.DiffOutput{Value: zdcText("ok"), Release: []string{"ghost"}}, nil
		})
		unknownReleaseResult := commands.ReplayDifferential(ctx, unknownReleaseSuite, 0, unknownReleaseCmd)
		c.Check(unknownReleaseResult.Status == gopter.PropError, "unknown release must be PropError, got %+v", unknownReleaseResult)

		duplicateReleaseSuite := zdcNewSuite()
		duplicateReleaseCmd := duplicateReleaseSuite.newCmd("dup-release", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			p := s.alloc()
			return commands.DiffOutput{
				Value:   zdcObject(map[string]commands.DiffValue{"h": zdcCreate(p)}),
				Release: []string{p, p},
			}, nil
		})
		duplicateReleaseResult := commands.ReplayDifferential(ctx, duplicateReleaseSuite, 0, duplicateReleaseCmd)
		c.Check(duplicateReleaseResult.Status == gopter.PropError, "duplicate release must be PropError, got %+v", duplicateReleaseResult)

		invalidShapeSuite := zdcNewSuite()
		invalidShapeCmd := invalidShapeSuite.newCmd("invalid-shape", func(_ *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			return commands.DiffOutput{Value: commands.DiffValue{
				Kind:  commands.DiffScalar,
				Text:  "x",
				Items: []commands.DiffValue{{Kind: commands.DiffScalar, Text: "not allowed"}},
			}}, nil
		})
		invalidShapeResult := commands.ReplayDifferential(ctx, invalidShapeSuite, 0, invalidShapeCmd)
		c.Check(invalidShapeResult.Status == gopter.PropError, "invalid value shape must be PropError, got %+v", invalidShapeResult)

		// 9. A callback error is a PropError naming implementation and step.
		callbackErrSuite := zdcNewSuite()
		callbackErrCmd := callbackErrSuite.newCmd("callback-err", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			s.countCall()
			if s.name == "beta" {
				return commands.DiffOutput{}, errors.New("zdc scripted callback failure")
			}
			return zdcScalar("ok"), nil
		})
		callbackErrResult := commands.ReplayDifferential(ctx, callbackErrSuite, 0, callbackErrCmd)
		c.Check(callbackErrResult.Status == gopter.PropError && callbackErrResult.Error != nil,
			"callback error must be PropError, got %+v", callbackErrResult)
		c.Check(callbackErrResult.Error != nil && strings.Contains(callbackErrResult.Error.Error(), "beta") &&
			strings.Contains(callbackErrResult.Error.Error(), "step 0"),
			"callback error must name beta and step 0: %v", callbackErrResult.Error)
		c.Check(callbackErrSuite.closes == 2, "systems must still be closed after callback error")
	})
}

// ---------------------------------------------------------------------------
// TestDiffTransactions: lifecycle, cancellation and isolation
// ---------------------------------------------------------------------------

func TestDiffTransactions(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ctx := context.Background()

		// Configuration errors.
		var nilSuite *zdcSuite
		nilCtxResult := commands.ReplayDifferential(nil, zdcNewSuite(), 0)
		c.Check(nilCtxResult.Status == gopter.PropError && nilCtxResult.Error != nil, "nil ctx must be PropError")
		nilSetResult := commands.ReplayDifferential(ctx, nil, 0)
		c.Check(nilSetResult.Status == gopter.PropError && nilSetResult.Error != nil, "nil command set must be PropError")
		typedNilResult := commands.ReplayDifferential(ctx, nilSuite, 0)
		c.Check(typedNilResult.Status == gopter.PropError && typedNilResult.Error != nil, "typed nil command set must be PropError")

		oneName := zdcNewSuite("only")
		c.Check(commands.ReplayDifferential(ctx, oneName, 0).Status == gopter.PropError, "one implementation name must be PropError")
		emptyName := zdcNewSuite("alpha", "")
		c.Check(commands.ReplayDifferential(ctx, emptyName, 0).Status == gopter.PropError, "empty implementation name must be PropError")
		dupName := zdcNewSuite("alpha", "alpha")
		c.Check(commands.ReplayDifferential(ctx, dupName, 0).Status == gopter.PropError, "duplicate implementation names must be PropError")

		badPropResult := commands.DifferentialProp(nil, zdcNewSuite()).Check(gopter.DefaultTestParametersWithSeed(1))
		c.Check(badPropResult.Status == gopter.TestError && badPropResult.Error != nil, "DifferentialProp with nil ctx must check as TestError")

		// Already cancelled context: Undecided before any factory.
		canceledCtx, cancel := context.WithCancel(ctx)
		cancel()
		canceledSuite := zdcNewSuite()
		canceledResult := commands.ReplayDifferential(canceledCtx, canceledSuite, 0)
		c.Check(canceledResult.Status == gopter.PropUndecided, "already cancelled ctx must be PropUndecided")
		c.Check(canceledSuite.creates == 0 && canceledSuite.closes == 0, "no factory may run after cancellation")

		// Empty plan on active context is True and owns factories.
		emptySuite := zdcNewSuite("alpha", "beta", "gamma")
		emptyResult := commands.ReplayDifferential(ctx, emptySuite, 0)
		c.Check(emptyResult.Status == gopter.PropTrue, "empty plan must be PropTrue, got %+v", emptyResult)
		c.Check(emptySuite.creates == 3 && emptySuite.closes == 3, "empty plan still owns and destroys all factories")
		c.Check(strings.Join(emptySuite.destroyOrder, ",") == "gamma,beta,alpha",
			"systems must be destroyed in reverse creation order, got %v", emptySuite.destroyOrder)
		emptyCanceledResult := commands.ReplayDifferential(canceledCtx, zdcNewSuite(), 0)
		c.Check(emptyCanceledResult.Status == gopter.PropUndecided, "empty plan must still observe cancellation")

		// Invalid initial precondition: Undecided before factories.
		invalidInitialSuite := zdcNewSuite()
		invalidInitialSuite.InitialPreConditionFunc = func(state commands.State) bool { return state.(int) > 100 }
		invalidInitialResult := commands.ReplayDifferential(ctx, invalidInitialSuite, 0)
		c.Check(invalidInitialResult.Status == gopter.PropUndecided, "invalid initial precondition must be Undecided")
		c.Check(invalidInitialSuite.creates == 0, "invalid initial precondition must not create systems")

		// Invalid command precondition: Undecided without running that command.
		preSuite := zdcNewSuite()
		ran := 0
		preOk := preSuite.newCmd("pre-ok", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			ran++
			return zdcScalar("ok"), nil
		})
		preBlocked := preSuite.newCmd("pre-blocked", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			ran += 100
			return zdcScalar("should-not-run"), nil
		})
		preBlocked.PreConditionFunc = func(commands.State) bool { return false }
		preResult := commands.ReplayDifferential(ctx, preSuite, 0, preOk, preBlocked)
		c.Check(preResult.Status == gopter.PropUndecided, "invalid command precondition must be Undecided, got %+v", preResult)
		c.Check(ran == 2, "only the valid first command may run on both implementations, ran=%d", ran)

		// Unsupported legacy Command is a PropError.
		legacySuite := zdcNewSuite()
		legacy := &commands.ProtoCommand{Name: "legacy"}
		legacyResult := commands.ReplayDifferential(ctx, legacySuite, 0, legacy)
		c.Check(legacyResult.Status == gopter.PropError && legacyResult.Error != nil, "legacy Command must be PropError")

		// Factory failure is PropError and earlier systems are cleaned up.
		factorySuite := zdcNewSuite("alpha", "beta", "gamma")
		factorySuite.failFactoryFor = "beta"
		factoryResult := commands.ReplayDifferential(ctx, factorySuite, 0, factorySuite.newCmd("x", func(*zdcStore, *commands.HandleBindings) (commands.DiffOutput, error) {
			return zdcScalar("x"), nil
		}))
		c.Check(factoryResult.Status == gopter.PropError, "factory failure must be PropError, got %+v", factoryResult)
		c.Check(strings.Join(factorySuite.createOrder, ",") == "alpha,beta", "creation order mismatch: %v", factorySuite.createOrder)
		c.Check(strings.Join(factorySuite.destroyOrder, ",") == "alpha", "only alpha must be destroyed, got %v", factorySuite.destroyOrder)
		c.Check(factorySuite.closes == 1, "exactly one successful system must be closed")

		// Half-round cancellation: alpha callback observes cancellation, beta
		// never runs, nothing commits; a fresh replay is fully isolated.
		halfSuite := zdcNewSuite()
		halfCtx, halfCancel := context.WithCancel(ctx)
		halfCmd := halfSuite.newCmd("half", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			s.countCall()
			if s.name == "alpha" {
				halfCancel()
			}
			return zdcScalar("same"), nil
		})
		halfResult := commands.ReplayDifferential(halfCtx, halfSuite, 0, halfCmd)
		c.Check(halfResult.Status == gopter.PropUndecided, "half-round cancellation must be Undecided, got %+v", halfResult)
		c.Check(halfSuite.calls["alpha"] == 1 && halfSuite.calls["beta"] == 0,
			"beta must not run after alpha cancels, calls=%v", halfSuite.calls)
		c.Check(halfSuite.creates == 2 && halfSuite.closes == 2, "half-round systems must be cleaned up")

		// Rerun with a live context: the stale cancel func only cancels the
		// old context, so the new round completes equally.
		rerunResult := commands.ReplayDifferential(ctx, halfSuite, 0, halfCmd)
		c.Check(rerunResult.Status == gopter.PropTrue, "rerun isolation must yield PropTrue, got %+v", rerunResult)
		c.Check(halfSuite.calls["alpha"] == 2 && halfSuite.calls["beta"] == 1,
			"rerun must execute beta exactly once, calls=%v", halfSuite.calls)
		c.Check(halfSuite.creates == 4 && halfSuite.closes == 4, "rerun must own fresh systems, totals creates=%d closes=%d",
			halfSuite.creates, halfSuite.closes)

		// PostCondition receives the resulting model and each DiffOutput;
		// an unsuccessful postcondition ends with its actual result.
		postSuite := zdcNewSuite()
		postCmd := postSuite.newCmd("post", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			return zdcScalar("observed"), nil
		})
		postCalls := 0
		postCmd.PostConditionFunc = func(state commands.State, result commands.Result) *gopter.PropResult {
			postCalls++
			output, ok := result.(commands.DiffOutput)
			c.Check(ok, "postcondition must receive commands.DiffOutput")
			c.Check(ok && output.Value.Text == "observed", "postcondition output must be the adapter observation")
			c.Check(state.(int) == 0, "default NextState keeps int model at 0")
			return &gopter.PropResult{Status: gopter.PropFalse, Labels: []string{"zdc-postcondition-failed"}}
		}
		postResult := commands.ReplayDifferential(ctx, postSuite, 0, postCmd)
		c.Check(postResult.Status == gopter.PropFalse && zdcLabelContains(postResult, "zdc-postcondition-failed"),
			"postcondition failure must propagate its actual result, got %+v", postResult)
		c.Check(postCalls == 1, "unsuccessful postcondition must stop later impl postconditions, calls=%d", postCalls)
	})
}

// ---------------------------------------------------------------------------
// TestDiffCandidates: real DifferentialProp generation, >=2 accepted
// shrinking rounds, policy-aware candidate acceptance and final replay.
// ---------------------------------------------------------------------------

// zdcPlanCarrier is implemented by the generated actions value through an
// exported method so black-box tests can replay the exact minimized plan.
type zdcPlanCarrier interface {
	DifferentialReplayPlan() (commands.State, []commands.Command)
}

// zdcScriptedCtx exposes a one-shot Err() cancellation used to exercise a
// real PropUndecided shrink candidate without permanently poisoning later
// candidates.
type zdcScriptedCtx struct {
	context.Context
	cancelNextErr bool
}

func (c *zdcScriptedCtx) armCancelOnce() { c.cancelNextErr = true }

func (c *zdcScriptedCtx) Err() error {
	if c.cancelNextErr {
		c.cancelNextErr = false
		return context.Canceled
	}
	return nil
}

type zdcChainCmd struct {
	*commands.ProtoCommand
	stage int
	suite *zdcSuite
}

func (c *zdcChainCmd) RunDifferential(system commands.SystemUnderTest, bindings *commands.HandleBindings) (commands.DiffOutput, error) {
	store := system.(*zdcStore)
	store.countCall()
	if c.stage == 90 {
		c.suite.adapterErrs++
		return commands.DiffOutput{}, errors.New("zdc scripted shrink adapter error")
	}
	return zdcForkOutput(store), nil
}

func zdcChainShrinker(scripted *zdcScriptedCtx, suite *zdcSuite) gopter.Shrinker {
	return func(value interface{}) gopter.Shrink {
		cmd := value.(*zdcChainCmd)
		var stages []int
		switch cmd.stage {
		case 0:
			// Error and cancellation candidates must be skipped before the
			// first accepted PropFalse of this round.
			stages = []int{90, 91, 1}
		case 1:
			stages = []int{90, 2}
		default:
			return gopter.NoShrink
		}
		index := 0
		return func() (interface{}, bool) {
			if index >= len(stages) {
				return nil, false
			}
			stage := stages[index]
			index++
			if stage == 91 {
				scripted.armCancelOnce()
			}
			return &zdcChainCmd{
				ProtoCommand: &commands.ProtoCommand{Name: fmt.Sprintf("chain-%d", stage)},
				stage:        stage,
				suite:        suite,
			}, true
		}
	}
}

func zdcCandidateParameters(seed int64, size int) *gopter.TestParameters {
	params := gopter.DefaultTestParametersWithSeed(seed)
	params.MinSuccessfulTests = 1
	params.MinSize = size
	params.MaxSize = size
	params.MaxShrinkCount = 1000
	params.Workers = 1
	return params
}

func TestDiffCandidates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ctx := context.Background()

		// --- Scenario A: deletion shrinker drives >=2 accepted rounds -----
		genSuite := zdcNewSuite("alpha", "beta")
		forkCmd := genSuite.newCmd("generated-fork", func(s *zdcStore, b *commands.HandleBindings) (commands.DiffOutput, error) {
			s.countCall()
			if _, ok := b.Resolve("0:/x"); ok {
				c.Check(false, "tentative create identity must not appear in committed bindings snapshot")
			}
			return zdcForkOutput(s), nil
		})
		genSuite.GenCommandFunc = func(commands.State) gopter.Gen { return gen.Const(forkCmd) }

		result := commands.DifferentialProp(ctx, genSuite).Check(zdcCandidateParameters(424242, 8))
		c.Check(result.Status == gopter.TestFailed, "generated fork property must fail, status=%v err=%v", result.Status, result.Error)
		c.Check(len(result.Args) == 1, "expected one generated actions arg, got %d", len(result.Args))

		shrinks := -1
		var carrier zdcPlanCarrier
		if len(result.Args) == 1 {
			shrinks = result.Args[0].Shrinks
			var ok bool
			carrier, ok = result.Args[0].Arg.(zdcPlanCarrier)
			c.Check(ok, "shrunk arg must expose DifferentialReplayPlan, got %T", result.Args[0].Arg)
		}
		c.Check(shrinks >= 2, "expected at least two accepted shrinking rounds, got %d", shrinks)
		c.Check(len(result.Labels) > 0 && strings.Contains(result.Labels[0], "/y"),
			"final stored failure must keep differential /y label, got %v", result.Labels)
		c.Check(genSuite.creates == genSuite.closes, "all generated/shrunk trials must be closed balanced")
		c.Check(genSuite.creates >= 1+shrinks, "every accepted round needs fresh factories, creates=%d shrinks=%d",
			genSuite.creates, shrinks)

		// Replay the exact minimized candidate on a fresh suite.
		if carrier != nil {
			initial, plan := carrier.DifferentialReplayPlan()
			c.Check(len(plan) >= 1, "minimized plan must still contain commands")
			replaySuite := zdcNewSuite("alpha", "beta")
			replayResult := commands.ReplayDifferential(ctx, replaySuite, initial, plan...)
			c.Check(replayResult.Status == gopter.PropFalse, "replayed minimized candidate must still be PropFalse, got %+v", replayResult)
			c.Check(zdcLabelContains(replayResult, `path="/y"`), "replay label must keep /y path, got %v", replayResult.Labels)
			c.Check(replaySuite.creates == 2 && replaySuite.closes == 2, "replay must own exactly one fresh pair of systems")
		}

		// --- Scenario B: error/undecided candidates never replace false ----
		scriptedCtx := &zdcScriptedCtx{Context: ctx}
		chainSuite := zdcNewSuite("alpha", "beta")
		chainGen := gopter.Gen(func(params *gopter.GenParameters) *gopter.GenResult {
			root := &zdcChainCmd{
				ProtoCommand: &commands.ProtoCommand{Name: "chain-0"},
				stage:        0,
				suite:        chainSuite,
			}
			return &gopter.GenResult{
				Result:     root,
				Shrinker:   zdcChainShrinker(scriptedCtx, chainSuite),
				ResultType: reflect.TypeOf((*zdcChainCmd)(nil)),
			}
		})
		chainSuite.GenCommandFunc = func(commands.State) gopter.Gen { return chainGen }

		chainResult := commands.DifferentialProp(scriptedCtx, chainSuite).Check(zdcCandidateParameters(99, 1))
		c.Check(chainResult.Status == gopter.TestFailed,
			"scripted chain must end TestFailed (errors/cancellation skipped), status=%v err=%v", chainResult.Status, chainResult.Error)
		c.Check(chainResult.Error == nil, "a skipped adapter error must not become the test error")
		c.Check(len(chainResult.Args) == 1 && chainResult.Args[0].Shrinks == 2,
			"exactly two false candidates must be accepted, got args=%+v", chainResult.Args)
		c.Check(chainSuite.adapterErrs >= 2, "adapter error candidates must actually be executed and skipped, errs=%d",
			chainSuite.adapterErrs)
		c.Check(len(chainResult.Labels) > 0 && strings.Contains(chainResult.Labels[0], "/y"),
			"stored failure must remain the genuine differential mismatch, got %v", chainResult.Labels)
		c.Check(chainSuite.creates == chainSuite.closes, "every chain trial must be closed balanced")

		if len(chainResult.Args) == 1 {
			if chainCarrier, ok := chainResult.Args[0].Arg.(zdcPlanCarrier); ok {
				initial, plan := chainCarrier.DifferentialReplayPlan()
				replayResult := commands.ReplayDifferential(ctx, zdcNewSuite("alpha", "beta"), initial, plan...)
				c.Check(replayResult.Status == gopter.PropFalse && zdcLabelContains(replayResult, "/y"),
					"final chain candidate must replay as the same differential false, got %+v", replayResult)
			} else {
				c.Check(false, "chain shrunk arg must expose DifferentialReplayPlan")
			}
		}

		// --- Scenario C: a first-run adapter error is not minimized into
		//     something else and reports TestError --------------------------
		errorSuite := zdcNewSuite()
		errorCmd := errorSuite.newCmd("always-error", func(s *zdcStore, _ *commands.HandleBindings) (commands.DiffOutput, error) {
			s.countCall()
			return commands.DiffOutput{}, errors.New("zdc first run adapter failure")
		})
		errorSuite.GenCommandFunc = func(commands.State) gopter.Gen { return gen.Const(errorCmd) }
		errorResult := commands.DifferentialProp(ctx, errorSuite).Check(zdcCandidateParameters(7, 2))
		c.Check(errorResult.Status == gopter.TestError && errorResult.Error != nil,
			"first-run adapter error must check as TestError, got %v", errorResult.Status)
		c.Check(errorSuite.calls["alpha"] >= 1, "error command must actually execute before reporting TestError")
	})
}
