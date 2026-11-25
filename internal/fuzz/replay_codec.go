// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package fuzz

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"reflect"
)

// Snapshot encoding.
//
// A replay snapshot is a self-contained, deterministic byte string. It has no
// pointers, timestamps, or host paths. Layout (all integers big-endian):
//
//	magic            8 bytes "GOREPLY\x01"
//	build ID         uint32 length + bytes
//	coverage size    uint32
//	type hash        32 bytes
//	corpus hash      32 bytes
//	epoch            uint64
//	committed        uint64
//	dispatched       uint64
//	corpus entries   uint64
//	coverage         uint32 length + bytes (length == coverage size)
//	feedback count   uint32
//	  each record:
//	    sequence     uint64
//	    coverage     uint32 length + bytes (length == coverage size)
//	checksum         32 bytes (SHA-256 of every preceding byte)
//
// The checksum is verified first, so truncation, corruption, and trailing
// bytes are rejected before any semantic validation is attempted.

const replaySnapshotVersion = "GOREPLY\x01"

type replayFeedbackRecord struct {
	seq      uint64
	coverage []byte
}

type replaySnapshot struct {
	buildID       string
	coverageSize  int
	typeHash      [sha256.Size]byte
	corpusHash    [sha256.Size]byte
	epoch         uint64
	committed     uint64
	dispatched    uint64
	corpusEntries uint64
	coverage      []byte
	feedback      []replayFeedbackRecord
}

func encodeReplaySnapshot(st *replaySnapshot) ([]byte, error) {
	var b bytes.Buffer
	b.WriteString(replaySnapshotVersion)
	writeReplayString(&b, st.buildID)
	writeReplayUint32(&b, uint32(st.coverageSize))
	b.Write(st.typeHash[:])
	b.Write(st.corpusHash[:])
	writeReplayUint64(&b, st.epoch)
	writeReplayUint64(&b, st.committed)
	writeReplayUint64(&b, st.dispatched)
	writeReplayUint64(&b, st.corpusEntries)
	writeReplayBytes(&b, st.coverage)
	writeReplayUint32(&b, uint32(len(st.feedback)))
	for _, rec := range st.feedback {
		writeReplayUint64(&b, rec.seq)
		writeReplayBytes(&b, rec.coverage)
	}
	sum := sha256.Sum256(b.Bytes())
	b.Write(sum[:])
	return b.Bytes(), nil
}

func decodeReplaySnapshot(data []byte, totalWorks uint64, coverageSize int) (*replaySnapshot, error) {
	const checksumSize = sha256.Size
	if len(data) < len(replaySnapshotVersion)+checksumSize {
		return nil, errors.New("replay: snapshot is truncated")
	}
	payload := data[:len(data)-checksumSize]
	want := sha256.Sum256(payload)
	if !bytes.Equal(want[:], data[len(data)-checksumSize:]) {
		return nil, errors.New("replay: snapshot checksum mismatch")
	}

	r := &replayReader{data: payload, off: 0}
	magic, err := r.bytes(len(replaySnapshotVersion))
	if err != nil {
		return nil, fmt.Errorf("replay: snapshot is truncated")
	}
	if !bytes.Equal(magic, []byte(replaySnapshotVersion)) {
		return nil, errors.New("replay: unsupported snapshot version")
	}
	st := &replaySnapshot{}
	if st.buildID, err = r.string(); err != nil {
		return nil, err
	}
	covSize, err := r.uint32()
	if err != nil {
		return nil, err
	}
	st.coverageSize = int(covSize)
	if st.typeHash, err = r.hash(); err != nil {
		return nil, err
	}
	if st.corpusHash, err = r.hash(); err != nil {
		return nil, err
	}
	if st.epoch, err = r.uint64(); err != nil {
		return nil, err
	}
	if st.committed, err = r.uint64(); err != nil {
		return nil, err
	}
	if st.dispatched, err = r.uint64(); err != nil {
		return nil, err
	}
	if st.corpusEntries, err = r.uint64(); err != nil {
		return nil, err
	}
	covLen, err := r.uint32()
	if err != nil {
		return nil, err
	}
	if uint64(covLen) != uint64(st.coverageSize) || st.coverageSize != coverageSize {
		return nil, errors.New("replay: snapshot coverage length is inconsistent")
	}
	if st.coverage, err = r.bytes(int(covLen)); err != nil {
		return nil, err
	}
	count, err := r.uint32()
	if err != nil {
		return nil, err
	}
	if uint64(count) > uint64(len(payload)) {
		return nil, errors.New("replay: snapshot feedback count is invalid")
	}
	if count > 0 {
		st.feedback = make([]replayFeedbackRecord, 0, count)
	}
	for i := uint32(0); i < count; i++ {
		seq, err := r.uint64()
		if err != nil {
			return nil, err
		}
		if seq == 0 || seq > totalWorks {
			return nil, fmt.Errorf("replay: snapshot feedback for unknown sequence %d", seq)
		}
		recLen, err := r.uint32()
		if err != nil {
			return nil, err
		}
		if uint64(recLen) != uint64(st.coverageSize) {
			return nil, fmt.Errorf("replay: snapshot feedback for sequence %d has wrong coverage length", seq)
		}
		cov, err := r.bytes(int(recLen))
		if err != nil {
			return nil, err
		}
		st.feedback = append(st.feedback, replayFeedbackRecord{seq: seq, coverage: cov})
	}
	if r.off != len(payload) {
		return nil, errors.New("replay: snapshot has trailing data")
	}
	return st, nil
}

// replayTypeHash identifies the ordered parameter types of the plan.
func replayTypeHash(types []reflect.Type) [sha256.Size]byte {
	var b bytes.Buffer
	writeReplayUint32(&b, uint32(len(types)))
	for _, t := range types {
		if t == nil {
			writeReplayString(&b, "<nil>")
			continue
		}
		writeReplayString(&b, t.String())
	}
	return sha256.Sum256(b.Bytes())
}

// replayCorpusHash identifies the complete staged corpus: the number of
// stages, each stage's entry count, and each entry's canonical bytes.
func replayCorpusHash(stageEnd []int, works []replayWork) [sha256.Size]byte {
	var b bytes.Buffer
	writeReplayUint32(&b, uint32(len(stageEnd)))
	start := 0
	for _, end := range stageEnd {
		writeReplayUint32(&b, uint32(end-start))
		for i := start; i < end; i++ {
			writeReplayBytes(&b, works[i].data)
		}
		start = end
	}
	return sha256.Sum256(b.Bytes())
}

func writeReplayUint32(b *bytes.Buffer, v uint32) {
	var buf [4]byte
	binary.BigEndian.PutUint32(buf[:], v)
	b.Write(buf[:])
}

func writeReplayUint64(b *bytes.Buffer, v uint64) {
	var buf [8]byte
	binary.BigEndian.PutUint64(buf[:], v)
	b.Write(buf[:])
}

func writeReplayString(b *bytes.Buffer, s string) {
	writeReplayUint32(b, uint32(len(s)))
	b.WriteString(s)
}

func writeReplayBytes(b *bytes.Buffer, p []byte) {
	writeReplayUint32(b, uint32(len(p)))
	b.Write(p)
}

type replayReader struct {
	data []byte
	off  int
}

var errReplayTruncated = errors.New("replay: snapshot is truncated")

func (r *replayReader) bytes(n int) ([]byte, error) {
	if n < 0 || n > len(r.data)-r.off {
		return nil, errReplayTruncated
	}
	p := r.data[r.off : r.off+n]
	r.off += n
	return append([]byte(nil), p...), nil
}

func (r *replayReader) uint32() (uint32, error) {
	p, err := r.bytes(4)
	if err != nil {
		return 0, err
	}
	return binary.BigEndian.Uint32(p), nil
}

func (r *replayReader) uint64() (uint64, error) {
	p, err := r.bytes(8)
	if err != nil {
		return 0, err
	}
	return binary.BigEndian.Uint64(p), nil
}

func (r *replayReader) hash() ([sha256.Size]byte, error) {
	var h [sha256.Size]byte
	p, err := r.bytes(sha256.Size)
	if err != nil {
		return h, err
	}
	copy(h[:], p)
	return h, nil
}

func (r *replayReader) string() (string, error) {
	n, err := r.uint32()
	if err != nil {
		return "", err
	}
	p, err := r.bytes(int(n))
	if err != nil {
		return "", err
	}
	return string(p), nil
}
