// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Command replay demonstrates a deterministic, multi-stage corpus replay.
//
// The program writes a transcript to the path given as its first argument. It
// runs two independent child processes that perform the same replay: one
// without interruption, and one that snapshots partway through and resumes in
// a freshly started process. The final states must agree exactly.
//
// Usage:
//
//	replay <transcript-file>
package main

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"os"
	"os/exec"
	"strings"

	"fuzzreplay.local/checkpoint/internal/fuzz"
)

const (
	checkpointArg  = "checkpoint"
	resumeArg      = "resume"
	uninterruptArg = "uninterrupted"
)

// demoPlan is the fixed replay plan used by every child process.
func demoPlan() fuzz.ReplayPlan {
	entry := func(s string) fuzz.CorpusEntry {
		return fuzz.CorpusEntry{Values: []any{[]byte(s)}, IsSeed: true}
	}
	return fuzz.ReplayPlan{
		BuildID:      "replay-demo-build",
		CoverageSize: 8,
		Stages: [][]fuzz.CorpusEntry{
			{entry("alpha"), entry("bravo")},
			{entry("charlie"), entry("alpha"), entry("delta")},
			{entry("echo")},
		},
	}
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: replay <transcript-file>")
		os.Exit(2)
	}
	var err error
	// Child invocations are "replay <mode> <out> [arg]". The top-level
	// invocation is just "replay <transcript-file>".
	switch os.Args[1] {
	case uninterruptArg:
		err = runFull(os.Args[2])
	case checkpointArg:
		err = runToCheckpoint(os.Args[2])
	case resumeArg:
		err = runResume(os.Args[3], os.Args[2])
	default:
		err = runParent(os.Args[1])
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "replay:", err)
		os.Exit(1)
	}
}

// runParent starts the two independent replays and compares their final state.
func runParent(out string) error {
	self, err := os.Executable()
	if err != nil {
		return err
	}
	dir, err := os.MkdirTemp("", "replay-example")
	if err != nil {
		return err
	}
	defer os.RemoveAll(dir)

	fullState := dir + "/uninterrupted.txt"
	snapPath := dir + "/checkpoint.snapshot"
	resumedState := dir + "/resumed.txt"

	if err := runChild(self, uninterruptArg, fullState, ""); err != nil {
		return fmt.Errorf("uninterrupted replay: %v", err)
	}
	if err := runChild(self, checkpointArg, snapPath, ""); err != nil {
		return fmt.Errorf("checkpoint replay: %v", err)
	}
	if err := runChild(self, resumeArg, resumedState, snapPath); err != nil {
		return fmt.Errorf("resumed replay: %v", err)
	}

	full, err := os.ReadFile(fullState)
	if err != nil {
		return err
	}
	resumed, err := os.ReadFile(resumedState)
	if err != nil {
		return err
	}
	ok := bytes.Equal(full, resumed)
	var b strings.Builder
	fmt.Fprintf(&b, "replay_restart_ok=%t\n", ok)
	fmt.Fprintf(&b, "uninterrupted=%s\n", strings.TrimSpace(string(full)))
	fmt.Fprintf(&b, "resumed=%s\n", strings.TrimSpace(string(resumed)))
	if err := os.WriteFile(out, []byte(b.String()), 0o644); err != nil {
		return err
	}
	if !ok {
		return fmt.Errorf("uninterrupted and resumed final states differ")
	}
	fmt.Print(b.String())
	return nil
}

func runChild(self, mode, out, arg string) error {
	args := []string{mode, out}
	if arg != "" {
		args = append(args, arg)
	}
	cmd := exec.Command(self, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

// runFull performs the whole replay without interruption.
func runFull(out string) error {
	s, err := fuzz.NewReplaySession(demoPlan())
	if err != nil {
		return err
	}
	if err := drive(s, -1, ""); err != nil {
		return err
	}
	return writeState(out, s.State())
}

// runToCheckpoint replays until the middle of the second stage, then writes a
// snapshot. Work has been dispatched but not all feedback has been accepted, so
// the snapshot contains in-flight leases.
func runToCheckpoint(snapPath string) error {
	s, err := fuzz.NewReplaySession(demoPlan())
	if err != nil {
		return err
	}
	// Two batches are accepted first, then a batch is snapshotted in flight.
	if err := drive(s, 2, snapPath); err != nil {
		return err
	}
	return nil
}

// runResume restores a snapshotted replay in a fresh process and finishes it.
func runResume(snapPath, out string) error {
	snap, err := os.ReadFile(snapPath)
	if err != nil {
		return err
	}
	s, err := fuzz.RestoreReplaySession(demoPlan(), snap)
	if err != nil {
		return err
	}
	if err := drive(s, -1, ""); err != nil {
		return err
	}
	return writeState(out, s.State())
}

// drive dispatches work in batches, runs each entry, and accepts feedback in
// reverse order so that out-of-order buffering is exercised. When checkpointAt
// is non-negative, it snapshots before accepting the batch that follows
// checkpointAt accepted results and returns.
func drive(s *fuzz.ReplaySession, checkpointAt int, snapPath string) error {
	total := totalEntries(demoPlan())
	accepted := 0
	for {
		var leases []fuzz.ReplayLease
		for i := 0; i < 2; i++ {
			lease, ok, err := s.Next()
			if err != nil {
				return err
			}
			if !ok {
				break
			}
			leases = append(leases, lease)
		}
		if len(leases) == 0 {
			st := s.State()
			if st.Committed == total {
				return nil
			}
			return fmt.Errorf("replay stalled: committed=%d total=%d", st.Committed, total)
		}
		if checkpointAt >= 0 && accepted >= checkpointAt {
			snap, err := s.Snapshot()
			if err != nil {
				return err
			}
			return os.WriteFile(snapPath, snap, 0o644)
		}
		for i := len(leases) - 1; i >= 0; i-- {
			if _, err := s.Accept(leases[i], execute(leases[i].Entry, demoPlan().CoverageSize)); err != nil {
				return err
			}
			accepted++
		}
	}
}

// execute simulates a worker running one normal corpus value. It returns a
// coverage mask derived only from the entry bytes, so replaying the same value
// in another process yields identical feedback.
func execute(entry fuzz.CorpusEntry, size int) []byte {
	sum := sha256.Sum256(entry.Data)
	cov := make([]byte, size)
	for i := range cov {
		cov[i] = sum[i%len(sum)]
	}
	return cov
}

func totalEntries(plan fuzz.ReplayPlan) uint64 {
	var n uint64
	for _, stage := range plan.Stages {
		n += uint64(len(stage))
	}
	return n
}

func writeState(path string, st fuzz.ReplayState) error {
	line := fmt.Sprintf("stage=%d committed=%d pending=%d corpus_entries=%d coverage=%x",
		st.Stage, st.Committed, st.Pending, st.CorpusEntries, st.Coverage)
	if path == "-" {
		fmt.Println(line)
		return nil
	}
	return os.WriteFile(path, []byte(line+"\n"), 0o644)
}
