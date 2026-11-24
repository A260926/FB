// Command handlediff compares two honest in-memory handle stores that assign
// physical handles with different numbering schemes. Two logical handles are
// created through commands.ReplayDifferential; physical numbering differences
// normalize to the same command-position/tree-path identities, so the stores
// are considered equal.
package main

import (
	"context"
	"fmt"
	"os"

	"github.com/leanovate/gopter"
	"github.com/leanovate/gopter/commands"
	"github.com/leanovate/gopter/gen"
)

// handleObject is one benign live handle in a store.
type handleObject struct {
	physical string
}

// handleStore is an honest in-memory handle allocator with its own physical
// numbering and prefix.
type handleStore struct {
	name   string
	prefix string
	next   int
	live   map[string]*handleObject
}

func newHandleStore(name, prefix string, first int) *handleStore {
	return &handleStore{
		name:   name,
		prefix: prefix,
		next:   first,
		live:   map[string]*handleObject{},
	}
}

func (s *handleStore) alloc() string {
	physical := fmt.Sprintf("%s%d", s.prefix, s.next)
	s.next++
	s.live[physical] = &handleObject{physical: physical}
	return physical
}

// createHandleCommand creates one handle and reports it under the "handle"
// object field, giving it logical identity "<command-index>:/handle".
type createHandleCommand struct {
	*commands.ProtoCommand
}

func (c *createHandleCommand) RunDifferential(system commands.SystemUnderTest, _ *commands.HandleBindings) (commands.DiffOutput, error) {
	store := system.(*handleStore)
	physical := store.alloc()
	return commands.DiffOutput{
		Value: commands.DiffValue{
			Kind: commands.DiffObject,
			Fields: map[string]commands.DiffValue{
				"handle": {Kind: commands.DiffCreate, Text: physical},
			},
		},
	}, nil
}

type handleDiffSuite struct {
	*commands.ProtoCommands
}

func (s *handleDiffSuite) ImplementationNames() []string {
	return []string{"small-numbering", "large-numbering"}
}

func (s *handleDiffSuite) CloneState(state commands.State) commands.State {
	return state.(int)
}

func (s *handleDiffSuite) NewImplementation(name string, initial commands.State) (commands.SystemUnderTest, error) {
	if initial.(int) != 0 {
		return nil, fmt.Errorf("unexpected initial state %v", initial)
	}
	switch name {
	case "small-numbering":
		// h1, h2
		return newHandleStore(name, "h", 1), nil
	case "large-numbering":
		// serial-1001, serial-1002
		return newHandleStore(name, "serial-", 1001), nil
	default:
		return nil, fmt.Errorf("unknown implementation %q", name)
	}
}

func (s *handleDiffSuite) DestroyImplementation(_ string, _ commands.SystemUnderTest) {}

func main() {
	create := &createHandleCommand{
		ProtoCommand: &commands.ProtoCommand{Name: "create-handle"},
	}
	suite := &handleDiffSuite{
		ProtoCommands: &commands.ProtoCommands{
			InitialStateGen: gen.Const(0),
			GenCommandFunc:  func(commands.State) gopter.Gen { return gen.Const(create) },
		},
	}

	result := commands.ReplayDifferential(
		context.Background(),
		suite,
		0,
		create,
		create,
	)
	if result == nil || result.Status != gopter.PropTrue {
		fmt.Fprintf(os.Stderr, "handlediff: differential replay failed: %+v\n", result)
		os.Exit(1)
	}

	fmt.Println("equal handles=2")
}
