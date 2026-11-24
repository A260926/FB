// Copyright 2015 go-fuzz project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package main

import (
	"encoding/json"
	"os"
	"path/filepath"

	. "github.com/dvyukov/go-fuzz/go-fuzz-defs"
)

// ownershipExampleSteps is how many real worker steps the trace records.
const ownershipExampleSteps = 5

// RunOwnershipExample drives the production Worker.OwnershipStep path
// against the benign testee named by GOFUZZ_BENIGN_BIN and writes an
// OwnershipView trace to dir/trace.json. Every invocation gets a fresh
// output directory supplied by the caller.
func RunOwnershipExample(dir string) error {
	bin := os.Getenv("GOFUZZ_BENIGN_BIN")
	if bin == "" {
		return errOwnershipExampleBin
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}

	hub := newExampleHub()
	worker := &Worker{hub: hub, mutator: newMutator()}
	worker.coverBin = newTestBinary(bin, func() {}, &worker.stats, 0)
	defer worker.coverBin.close()

	// Initial triage of the seed corpus, exactly as the normal worker
	// would do before fuzzing starts.
	seed := []byte("A")
	worker.triageInput(CoordinatorInput{
		Data:      seed,
		Type:      execCorpus,
		Minimized: true,
		Smashed:   true,
	})
	inp := <-hub.newInputC
	seedCover := make([]byte, CoverSize)
	updateMaxCover(seedCover, inp.cover)
	hub.ro.Store(&ROData{
		corpus:       []Input{inp},
		corpusCover:  seedCover,
		badInputs:    make(map[Sig]struct{}),
		suppressions: make(map[Sig]struct{}),
	})
	hub.maxCover.Store(makeCopy(seedCover))
	hub.corpusSigs[hash(inp.data)] = struct{}{}

	if err := hub.EnableOwnership(); err != nil {
		return err
	}

	// Real worker steps: one mutation, one target call and three triage
	// observations each, confirmed through the shared admission path.
	for i := 0; i < ownershipExampleSteps; i++ {
		if _, err := worker.OwnershipStep(); err != nil {
			return err
		}
	}

	view := hub.OwnershipSnapshot()
	trace, err := json.MarshalIndent(view, "", "\t")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, "trace.json"), trace, 0o644)
}

func newExampleHub() *Hub {
	hub := &Hub{
		corpusSigs:  make(map[Sig]struct{}),
		triageC:     make(chan CoordinatorInput, 16),
		newInputC:   make(chan Input, 16),
		newCrasherC: make(chan NewCrasherArgs, 16),
		syncC:       make(chan Stats, 16),
	}
	hub.ro.Store(&ROData{
		corpusCover:  make([]byte, CoverSize),
		badInputs:    make(map[Sig]struct{}),
		suppressions: make(map[Sig]struct{}),
	})
	hub.maxCover.Store(make([]byte, CoverSize))
	return hub
}
