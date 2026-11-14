package dns

import (
	"encoding/hex"
	"errors"
	"fmt"
	"net"
	"reflect"
	"testing"
	"time"
)

// This file exercises the AXFR/IXFR record-sequence grammar shared by
// Transfer.In and Transfer.InZone, the snapshot application semantics of
// InZone, and the ownership/independence guarantees of its inputs and outputs.
// All network interaction goes through net.Pipe, so no sockets or public DNS
// are required.

const (
	testApex = "zone.test."
	testNS   = "ns1.zone.test."
)

func testSOA(serial uint32) RR {
	return testRR(fmt.Sprintf("%s 60 IN SOA ns.zone.test. host.zone.test. %d 1 2 3 4", testApex, serial))
}

// baseZone is a hand-written snapshot at serial 4294967294, two counts before
// the 32 bit wrap to 1.
func baseZone() []RR {
	return []RR{
		testSOA(4294967294),
		testRR(testApex + " 3600 IN NS " + testNS),
		testRR("x.zone.test. 300 IN A 192.0.2.1"),
		testRR(`txt.zone.test. 300 IN TXT "hello"`),
		testRR(`u.zone.test. 300 IN TYPE997 \# 4 010203ff`),
	}
}

func ixfrQuery(serial uint32) *Msg {
	q := new(Msg)
	q.SetIxfr(testApex, serial, "ns.zone.test.", "host.zone.test.")
	return q
}

func axfrQuery() *Msg {
	q := new(Msg)
	q.SetAxfr(testApex)
	return q
}

// wireKey renders an RR in uncompressed wire form. The TTL is part of the
// header, so two records that differ only in TTL produce different keys.
func wireKey(t *testing.T, rr RR) string {
	t.Helper()
	c := Copy(rr)
	buf := make([]byte, Len(c)*2)
	off, err := PackRR(c, buf, 0, nil, false)
	if err != nil {
		t.Fatalf("pack %s: %v", rr.String(), err)
	}
	return hex.EncodeToString(buf[:off])
}

// assertSnapshot verifies that got and want describe the same record set,
// that the SOA is unique and first, and (implicitly through the wire key) that
// every TTL and unknown RDATA byte matches.
func assertSnapshot(t *testing.T, got, want []RR) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("snapshot size = %d records, want %d\ngot: %v\nwant: %v", len(got), len(want), got, want)
	}
	if _, ok := got[0].(*SOA); !ok {
		t.Fatalf("first record is %T, want *SOA", got[0])
	}
	gset, wset := map[string]int{}, map[string]int{}
	soaCount := 0
	for _, r := range got {
		if _, ok := r.(*SOA); ok {
			soaCount++
		}
		gset[wireKey(t, r)]++
	}
	if soaCount != 1 {
		t.Fatalf("snapshot contains %d SOA records, want exactly 1", soaCount)
	}
	for _, r := range want {
		wset[wireKey(t, r)]++
	}
	if !reflect.DeepEqual(gset, wset) {
		t.Fatalf("snapshot record set mismatch\ngot: %v\nwant: %v", got, want)
	}
}

func snapshotKeys(t *testing.T, rrs []RR) map[string]int {
	t.Helper()
	m := map[string]int{}
	for _, r := range rrs {
		m[wireKey(t, r)]++
	}
	return m
}

// recordingApplier records the callbacks the grammar makes.
type recordingApplier struct {
	axfrSOA *SOA
	newSOA  []uint32
	deletes []string
	adds    []string
}

func (r *recordingApplier) useAxfr(s *SOA) { r.axfrSOA = s }
func (r *recordingApplier) useNewSOA(s *SOA) {
	r.newSOA = append(r.newSOA, s.Serial)
}
func (r *recordingApplier) applyDelete(rr RR) error {
	r.deletes = append(r.deletes, rr.String())
	return nil
}
func (r *recordingApplier) applyAdd(rr RR) error {
	r.adds = append(r.adds, rr.String())
	return nil
}

func feedGrammar(t *testing.T, g *xfrGrammar, msgs [][]RR) error {
	t.Helper()
	for _, msg := range msgs {
		for _, rr := range msg {
			if err := g.step(rr); err != nil {
				return err
			}
		}
	}
	return nil
}

func TestIXFRGrammar(t *testing.T) {
	apex := testApex
	soa := func(serial uint32) *SOA { return testSOA(serial).(*SOA) }
	plain := func(name, body string) RR { return testRR(name + " 300 IN " + body) }

	t.Run("serial_arithmetic", func(t *testing.T) {
		cases := []struct {
			cur, prev uint32
			want      int
			wantErr   bool
		}{
			{1, 1, 0, false},
			{1, 4294967294, 1, false}, // wrap forward across the 32 bit boundary
			{4294967294, 1, -1, false},
			{10, 5, 1, false},
			{5, 10, -1, false},
			{0, 1 << 31, 0, true}, // exactly 2^31 is incomparable
			{1 << 31, 0, 0, true},
		}
		for _, c := range cases {
			got, err := serialRelation(c.cur, c.prev)
			if c.wantErr {
				if err == nil {
					t.Errorf("serialRelation(%d,%d) expected error", c.cur, c.prev)
				}
				continue
			}
			if err != nil || got != c.want {
				t.Errorf("serialRelation(%d,%d) = (%d,%v), want %d", c.cur, c.prev, got, err, c.want)
			}
		}
	})

	t.Run("single_soa_no_change", func(t *testing.T) {
		app := &recordingApplier{}
		g := newXfrGrammar(TypeIXFR, apex, soa(4294967294), false, app)
		if err := feedGrammar(t, g, [][]RR{{testSOA(4294967294)}}); err != nil {
			t.Fatal(err)
		}
		if !g.noChange() || !g.acceptEOF() {
			t.Fatalf("single equal SOA not treated as no-change: state %d", g.state)
		}
		if len(app.adds)+len(app.deletes) != 0 {
			t.Fatalf("no-change answer mutated the snapshot: %+v", app)
		}
	})

	t.Run("single_soa_older_is_still_no_change", func(t *testing.T) {
		app := &recordingApplier{}
		g := newXfrGrammar(TypeIXFR, apex, soa(10), false, app)
		if err := feedGrammar(t, g, [][]RR{{testSOA(9)}}); err != nil {
			t.Fatal(err)
		}
		if !g.noChange() {
			t.Fatal("older single SOA must still be a grammar-level no-change")
		}
	})

	t.Run("axfr_direct_requires_closing_soa", func(t *testing.T) {
		app := &recordingApplier{}
		g := newXfrGrammar(TypeAXFR, apex, nil, false, app)
		err := feedGrammar(t, g, [][]RR{{testSOA(1), plain("x.zone.test.", "A 192.0.2.1"), testSOA(1)}})
		if err != nil {
			t.Fatalf("closed AXFR rejected: %v", err)
		}
		if !g.ended() || app.axfrSOA == nil {
			t.Fatal("closed AXFR not recognized")
		}

		// An unclosed AXFR must not accept EOF.
		g2 := newXfrGrammar(TypeAXFR, apex, nil, false, &recordingApplier{})
		if err := feedGrammar(t, g2, [][]RR{{testSOA(1), plain("x.zone.test.", "A 192.0.2.1")}}); err != nil {
			t.Fatal(err)
		}
		if g2.acceptEOF() {
			t.Fatal("unclosed AXFR accepted EOF")
		}

		// An apex SOA wedged into the middle must not be accepted: a repeated
		// current SOA closes early, so any following record is an error.
		g3 := newXfrGrammar(TypeAXFR, apex, nil, false, &recordingApplier{})
		err = feedGrammar(t, g3, [][]RR{{
			testSOA(1),
			plain("x.zone.test.", "A 192.0.2.1"),
			testSOA(1), // premature closing SOA
			plain("y.zone.test.", "A 192.0.2.2"),
		}})
		if err == nil {
			t.Fatal("AXFR with a trailing record after the closing SOA was accepted")
		}

		// A foreign apex SOA in the middle is a protocol error.
		foreign := testRR(fmt.Sprintf("%s 60 IN SOA ns.zone.test. host.zone.test. 2 1 2 3 4", apex))
		g4 := newXfrGrammar(TypeAXFR, apex, nil, false, &recordingApplier{})
		if err := feedGrammar(t, g4, [][]RR{{testSOA(1), foreign, testSOA(1)}}); err == nil {
			t.Fatal("AXFR with a different middle apex SOA was accepted")
		}
	})

	t.Run("axfr_fallback_inside_ixfr", func(t *testing.T) {
		app := &recordingApplier{}
		g := newXfrGrammar(TypeIXFR, apex, soa(4294967294), false, app)
		msgs := [][]RR{{
			testSOA(1),
			plain("x.zone.test.", "A 192.0.2.7"),
			testSOA(1),
		}}
		if err := feedGrammar(t, g, msgs); err != nil {
			t.Fatalf("AXFR fallback rejected: %v", err)
		}
		if !g.ended() || app.axfrSOA == nil || app.axfrSOA.Serial != 1 {
			t.Fatalf("AXFR fallback not recognized: %+v", app)
		}
		if len(app.deletes) != 0 {
			t.Fatalf("AXFR fallback must not delete against base: %v", app.deletes)
		}
	})

	t.Run("wrap_increment_one_group", func(t *testing.T) {
		app := &recordingApplier{}
		g := newXfrGrammar(TypeIXFR, apex, soa(4294967294), true, app)
		aOld := plain("x.zone.test.", "A 192.0.2.1")
		aNew := plain("x.zone.test.", "A 192.0.2.2")
		msgs := [][]RR{{
			testSOA(1),                // current
			testSOA(4294967294), aOld, // old SOA + delete
			testSOA(1), aNew, // new SOA + add
			testSOA(1), // closing current SOA
		}}
		if err := feedGrammar(t, g, msgs); err != nil {
			t.Fatalf("wrap IXFR rejected: %v", err)
		}
		if !g.ended() {
			t.Fatal("wrap IXFR did not close")
		}
		if !reflect.DeepEqual(app.newSOA, []uint32{1}) || len(app.deletes) != 1 || len(app.adds) != 1 {
			t.Fatalf("unexpected callbacks: %+v", app)
		}
	})

	t.Run("multiple_groups_segmentation_invariant", func(t *testing.T) {
		a1 := plain("x.zone.test.", "A 192.0.2.1")
		a2 := plain("x.zone.test.", "A 192.0.2.2")
		a3 := plain("x.zone.test.", "A 192.0.2.3")
		stream := []RR{
			testSOA(2),
			testSOA(4294967294), a1, testSOA(1), a2,
			testSOA(1), a2, testSOA(2), a3,
			testSOA(2),
		}

		// Every RR in its own message.
		perRR := make([][]RR, len(stream))
		for i, rr := range stream {
			perRR[i] = []RR{rr}
		}
		app1 := &recordingApplier{}
		g1 := newXfrGrammar(TypeIXFR, apex, soa(4294967294), true, app1)
		if err := feedGrammar(t, g1, perRR); err != nil {
			t.Fatalf("per-RR segmentation rejected: %v", err)
		}

		// Several groups packed into a single message.
		app2 := &recordingApplier{}
		g2 := newXfrGrammar(TypeIXFR, apex, soa(4294967294), true, app2)
		if err := feedGrammar(t, g2, [][]RR{stream}); err != nil {
			t.Fatalf("single-message segmentation rejected: %v", err)
		}

		if !g1.ended() || !g2.ended() {
			t.Fatal("both segmentations must reach the closing SOA")
		}
		if !reflect.DeepEqual(app1.newSOA, app2.newSOA) ||
			!reflect.DeepEqual(app1.deletes, app2.deletes) ||
			!reflect.DeepEqual(app1.adds, app2.adds) {
			t.Fatalf("segmentation changed interpretation:\n%+v\n%+v", app1, app2)
		}
		if !reflect.DeepEqual(app1.newSOA, []uint32{1, 2}) {
			t.Fatalf("expected two forward groups, got %v", app1.newSOA)
		}
	})

	t.Run("chaining_must_connect_versions", func(t *testing.T) {
		a1 := plain("x.zone.test.", "A 192.0.2.1")
		// Second group's old SOA is serial 9, but the first group ended at 1.
		stream := []RR{
			testSOA(2),
			testSOA(4294967294), a1, testSOA(1),
			testSOA(9), testSOA(2),
			testSOA(2),
		}
		g := newXfrGrammar(TypeIXFR, apex, soa(4294967294), true, &recordingApplier{})
		if err := feedGrammar(t, g, [][]RR{stream}); err == nil {
			t.Fatal("group chain skipping a version was accepted")
		}
	})

	t.Run("strict_first_group_requires_full_base_soa", func(t *testing.T) {
		// Old SOA keeps the serial but changes another field.
		bad := testRR(fmt.Sprintf("%s 60 IN SOA ns.zone.test. host.zone.test. 4294967294 99 2 3 4", apex))
		stream := []RR{testSOA(1), bad, testSOA(1), testSOA(1)}
		strict := newXfrGrammar(TypeIXFR, apex, soa(4294967294), true, &recordingApplier{})
		if err := feedGrammar(t, strict, [][]RR{stream}); err == nil {
			t.Fatal("strict mode accepted a mismatched base SOA")
		}
		// Lenient mode (Transfer.In) only compares serials.
		lenient := newXfrGrammar(TypeIXFR, apex, soa(4294967294), false, &recordingApplier{})
		if err := feedGrammar(t, lenient, [][]RR{stream}); err != nil {
			t.Fatalf("lenient mode rejected serial-only match: %v", err)
		}
	})

	t.Run("non_apex_or_class_soa_is_protocol_error", func(t *testing.T) {
		foreign := testRR("other.test. 60 IN SOA ns.other.test. host.other.test. 1 1 2 3 4")
		g := newXfrGrammar(TypeIXFR, apex, soa(4294967294), true, &recordingApplier{})
		err := feedGrammar(t, g, [][]RR{{testSOA(1), foreign}})
		if err == nil {
			t.Fatal("non-apex SOA accepted as a marker")
		}

		chaos := testRR(fmt.Sprintf("%s 60 CH SOA ns.zone.test. host.zone.test. 1 1 2 3 4", apex))
		g2 := newXfrGrammar(TypeIXFR, apex, soa(4294967294), true, &recordingApplier{})
		if err := feedGrammar(t, g2, [][]RR{{testSOA(1), chaos}}); err == nil {
			t.Fatal("non-IN class SOA accepted as a marker")
		}
	})

	t.Run("bad_closure_and_half_groups", func(t *testing.T) {
		a1 := plain("x.zone.test.", "A 192.0.2.1")
		// The last group ends at serial 7, but the closing SOA still claims
		// the current version announced by the leading SOA (serial 2).
		badClose := []RR{
			testSOA(2),
			testSOA(4294967294), a1, testSOA(1), plain("y.zone.test.", "A 192.0.2.9"),
			testSOA(1), testSOA(7),
			testSOA(2),
		}
		g := newXfrGrammar(TypeIXFR, apex, soa(4294967294), true, &recordingApplier{})
		if err := feedGrammar(t, g, [][]RR{badClose}); err == nil {
			t.Fatal("wrong closing SOA accepted")
		}

		// Half group: deletion section never reaches a new SOA.
		half := []RR{testSOA(1), testSOA(4294967294), a1}
		g2 := newXfrGrammar(TypeIXFR, apex, soa(4294967294), true, &recordingApplier{})
		if err := feedGrammar(t, g2, [][]RR{half}); err != nil {
			t.Fatal(err)
		}
		if g2.acceptEOF() {
			t.Fatal("half change group accepted EOF")
		}

		// Backwards serial inside a group.
		back := []RR{testSOA(4294967294), testSOA(1), testSOA(4294967294)}
		g3 := newXfrGrammar(TypeIXFR, apex, soa(1), true, &recordingApplier{})
		if err := feedGrammar(t, g3, [][]RR{back}); err == nil {
			t.Fatal("backwards serial accepted")
		}
	})

	t.Run("empty_answer_has_no_leading_soa", func(t *testing.T) {
		g := newXfrGrammar(TypeAXFR, apex, nil, false, &recordingApplier{})
		if err := g.step(plain("x.zone.test.", "A 192.0.2.1")); !errors.Is(err, ErrSoa) {
			t.Fatalf("leading non-SOA error = %v, want ErrSoa", err)
		}
	})
}

// pipeServer wires one net.Pipe end to a tiny DNS-over-TCP server. The client
// end is returned together with a cleanup that waits for the server goroutine.
func pipeServer(t *testing.T, handle func(sc *Conn, req *Msg)) (net.Conn, func()) {
	return pipeServerWithProvider(t, nil, handle)
}

// pipeServerWithProvider is pipeServer with a TSIG provider on the server
// connection, needed only to parse a signed request.
func pipeServerWithProvider(t *testing.T, provider TsigProvider, handle func(sc *Conn, req *Msg)) (net.Conn, func()) {
	t.Helper()
	client, server := net.Pipe()
	done := make(chan struct{})
	go func() {
		defer close(done)
		defer server.Close()
		sc := &Conn{Conn: server, TsigProvider: provider}
		req, err := sc.ReadMsg()
		if err != nil {
			return
		}
		handle(sc, req)
	}()
	cleanup := func() {
		client.Close()
		<-done
	}
	return client, cleanup
}

func pipeReply(sc *Conn, req *Msg, answer []RR) error {
	m := new(Msg)
	m.SetReply(req)
	m.Answer = answer
	return sc.WriteMsg(m)
}

func pipeTransfer(client net.Conn) *Transfer {
	return &Transfer{Conn: &Conn{Conn: client}, ReadTimeout: 5 * time.Second}
}

func TestZoneTransferApply(t *testing.T) {
	aOld := testRR("x.zone.test. 300 IN A 192.0.2.1")
	aNew := testRR("x.zone.test. 300 IN A 192.0.2.2")
	aThree := testRR("x.zone.test. 300 IN A 192.0.2.3")
	txtOld := testRR(`txt.zone.test. 300 IN TXT "hello"`)
	txtNew := testRR(`txt.zone.test. 300 IN TXT "Hello"`)

	expectedAfterWrap := []RR{
		testSOA(1),
		testRR(testApex + " 3600 IN NS " + testNS),
		aNew,
		txtNew,
		testRR(`u.zone.test. 300 IN TYPE997 \# 4 010203ff`),
	}

	t.Run("in_channel_wraps_across_tcp_messages", func(t *testing.T) {
		// Reproduces the original bug through the real Transfer.In channel:
		// the delta only arrives in the second length-prefixed TCP message.
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			if err := pipeReply(sc, req, []RR{testSOA(1)}); err != nil {
				return
			}
			_ = pipeReply(sc, req, []RR{testSOA(4294967294), testSOA(1), aNew, testSOA(1)})
		})
		defer cleanup()

		tr := pipeTransfer(client)
		ch, err := tr.In(ixfrQuery(4294967294), "unused")
		if err != nil {
			t.Fatal(err)
		}
		n := 0
		var lastErr error
		for env := range ch {
			if env.Error != nil {
				lastErr = env.Error
			}
			n += len(env.RR)
		}
		if lastErr != nil {
			t.Fatalf("envelope error: %v", lastErr)
		}
		if n != 5 {
			t.Fatalf("received %d RRs, want 5 (the delta must not be dropped)", n)
		}
	})

	t.Run("inzone_wrap_increment", func(t *testing.T) {
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			stream := []RR{
				testSOA(1),
				testSOA(4294967294), aOld, txtOld,
				testSOA(1), aNew, txtNew,
				testSOA(1),
			}
			_ = pipeReply(sc, req, stream)
		})
		defer cleanup()

		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", baseZone())
		if err != nil {
			t.Fatalf("InZone wrap: %v", err)
		}
		assertSnapshot(t, got, expectedAfterWrap)
	})

	t.Run("inzone_one_rr_per_message", func(t *testing.T) {
		stream := []RR{
			testSOA(1),
			testSOA(4294967294), aOld,
			testSOA(1), aNew,
			testSOA(1),
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			for _, rr := range stream {
				if err := pipeReply(sc, req, []RR{rr}); err != nil {
					return
				}
			}
		})
		defer cleanup()

		base := []RR{testSOA(4294967294), aOld}
		want := []RR{testSOA(1), aNew}
		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", base)
		if err != nil {
			t.Fatalf("per-RR InZone: %v", err)
		}
		assertSnapshot(t, got, want)
	})

	t.Run("inzone_multiple_groups_one_message", func(t *testing.T) {
		stream := []RR{
			testSOA(2),
			testSOA(4294967294), aOld, testSOA(1), aNew,
			testSOA(1), aNew, testSOA(2), aThree,
			testSOA(2),
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, stream)
		})
		defer cleanup()

		base := []RR{testSOA(4294967294), aOld}
		want := []RR{testSOA(2), aThree}
		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", base)
		if err != nil {
			t.Fatalf("multi-group InZone: %v", err)
		}
		assertSnapshot(t, got, want)
	})

	t.Run("inzone_second_group_withdraws_first_add", func(t *testing.T) {
		extra := testRR("added.zone.test. 300 IN A 198.51.100.10")
		stream := []RR{
			testSOA(2),
			testSOA(4294967294), testSOA(1), extra,
			testSOA(1), extra, testSOA(2),
			testSOA(2),
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, stream)
		})
		defer cleanup()

		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294)})
		if err != nil {
			t.Fatalf("withdraw InZone: %v", err)
		}
		assertSnapshot(t, got, []RR{testSOA(2)})
	})

	t.Run("inzone_single_soa_equal_returns_base_copy", func(t *testing.T) {
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, []RR{testSOA(4294967294)})
		})
		defer cleanup()

		base := baseZone()
		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", base)
		if err != nil {
			t.Fatalf("no-change InZone: %v", err)
		}
		assertSnapshot(t, got, base)
		if &got[0] == &base[0] {
			t.Fatal("no-change result aliases the base slice")
		}
	})

	t.Run("inzone_single_soa_older_keeps_local", func(t *testing.T) {
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, []RR{testSOA(3)})
		})
		defer cleanup()

		base := []RR{testSOA(5), aOld}
		got, err := pipeTransfer(client).InZone(ixfrQuery(5), "unused", base)
		if err != nil {
			t.Fatalf("older SOA InZone: %v", err)
		}
		assertSnapshot(t, got, base)
		if got[0].(*SOA).Serial != 5 {
			t.Fatalf("local serial overwritten by stale server SOA: %d", got[0].(*SOA).Serial)
		}
	})

	t.Run("inzone_single_soa_incomparable_errors", func(t *testing.T) {
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, []RR{testSOA(1 << 31)})
		})
		defer cleanup()

		if _, err := pipeTransfer(client).InZone(ixfrQuery(0), "unused", []RR{testSOA(0)}); err == nil {
			t.Fatal("incomparable single SOA did not error")
		}
	})

	t.Run("inzone_single_soa_newer_without_delta_errors", func(t *testing.T) {
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, []RR{testSOA(1)})
		})
		defer cleanup()

		if _, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294)}); err == nil {
			t.Fatal("newer single SOA without delta did not error")
		}
	})

	t.Run("inzone_axfr_fallback_replaces_snapshot", func(t *testing.T) {
		fallback := []RR{
			testSOA(1),
			testRR(testApex + " 3600 IN NS ns2.zone.test."),
			testRR("fresh.zone.test. 300 IN A 203.0.113.9"),
			testSOA(1),
		}
		// The snapshot keeps only the single apex SOA.
		want := []RR{fallback[0], fallback[1], fallback[2]}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, fallback)
		})
		defer cleanup()

		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", baseZone())
		if err != nil {
			t.Fatalf("AXFR fallback InZone: %v", err)
		}
		assertSnapshot(t, got, want)
	})

	t.Run("inzone_direct_axfr", func(t *testing.T) {
		zone := []RR{
			testSOA(7),
			testRR(testApex + " 3600 IN NS " + testNS),
			testRR("only.zone.test. 300 IN A 198.51.100.2"),
			testSOA(7),
		}
		want := []RR{zone[0], zone[1], zone[2]}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, zone)
		})
		defer cleanup()

		got, err := pipeTransfer(client).InZone(axfrQuery(), "unused", nil)
		if err != nil {
			t.Fatalf("direct AXFR InZone: %v", err)
		}
		assertSnapshot(t, got, want)
	})

	t.Run("inzone_direct_axfr_unclosed_eof_errors", func(t *testing.T) {
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, []RR{testSOA(7), testRR("only.zone.test. 300 IN A 198.51.100.2")})
			// Close without the trailing SOA.
		})
		defer cleanup()

		if _, err := pipeTransfer(client).InZone(axfrQuery(), "unused", nil); err == nil {
			t.Fatal("unclosed AXFR accepted")
		}
	})

	t.Run("inzone_ttl_change_needs_delete_and_add", func(t *testing.T) {
		oldTTL := testRR("ttl.zone.test. 100 IN A 198.51.100.7")
		newTTL := testRR("ttl.zone.test. 200 IN A 198.51.100.7")
		stream := []RR{
			testSOA(1), testSOA(4294967294), oldTTL, testSOA(1), newTTL, testSOA(1),
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, stream)
		})
		defer cleanup()

		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294), oldTTL})
		if err != nil {
			t.Fatalf("TTL change InZone: %v", err)
		}
		assertSnapshot(t, got, []RR{testSOA(1), newTTL})
	})

	t.Run("inzone_delete_with_wrong_ttl_errors", func(t *testing.T) {
		oldTTL := testRR("ttl.zone.test. 100 IN A 198.51.100.7")
		wrongTTL := testRR("ttl.zone.test. 199 IN A 198.51.100.7")
		stream := []RR{
			testSOA(1), testSOA(4294967294), wrongTTL, testSOA(1), testSOA(1),
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, stream)
		})
		defer cleanup()

		if _, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294), oldTTL}); err == nil {
			t.Fatal("delete with mismatched TTL did not error")
		}
	})

	t.Run("inzone_uppercase_owner_delete", func(t *testing.T) {
		lower := testRR("lower.zone.test. 300 IN A 203.0.113.5")
		upper := testRR("LOWER.Zone.Test. 300 IN A 203.0.113.5")
		stream := []RR{
			testSOA(1), testSOA(4294967294), upper, testSOA(1), testSOA(1),
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, stream)
		})
		defer cleanup()

		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294), lower})
		if err != nil {
			t.Fatalf("uppercase owner delete: %v", err)
		}
		assertSnapshot(t, got, []RR{testSOA(1)})
	})

	t.Run("inzone_txt_case_is_significant", func(t *testing.T) {
		lo := testRR(`case.zone.test. 300 IN TXT "abc"`)
		hi := testRR(`case.zone.test. 300 IN TXT "ABC"`)

		// Adding an identical TXT identity must fail.
		dupStream := []RR{
			testSOA(1), testSOA(4294967294), testSOA(1), lo, testSOA(1),
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, dupStream)
		})
		if _, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294), lo}); err == nil {
			t.Fatal("duplicate TXT identity add did not error")
		}
		cleanup()

		// Changing only the TXT bytes via delete+add must succeed and persist.
		changeStream := []RR{
			testSOA(1), testSOA(4294967294), lo, testSOA(1), hi, testSOA(1),
		}
		client2, cleanup2 := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, changeStream)
		})
		defer cleanup2()
		got, err := pipeTransfer(client2).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294), lo})
		if err != nil {
			t.Fatalf("TXT case change: %v", err)
		}
		assertSnapshot(t, got, []RR{testSOA(1), hi})
	})

	t.Run("inzone_unknown_rfc3597_bytes_preserved", func(t *testing.T) {
		oldUnk := testRR(`u.zone.test. 300 IN TYPE997 \# 4 010203ff`)
		newUnk := testRR(`u.zone.test. 300 IN TYPE997 \# 4 deadbeef`)
		stream := []RR{
			testSOA(1), testSOA(4294967294), oldUnk, testSOA(1), newUnk, testSOA(1),
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, stream)
		})
		defer cleanup()

		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294), oldUnk})
		if err != nil {
			t.Fatalf("RFC3597 change: %v", err)
		}
		var found *RFC3597
		for _, rr := range got {
			if r, ok := rr.(*RFC3597); ok {
				found = r
			}
		}
		if found == nil {
			t.Fatal("RFC3597 record missing from snapshot")
		}
		if found.Rdata != "deadbeef" {
			t.Fatalf("RFC3597 rdata = %q, want deadbeef", found.Rdata)
		}
		assertSnapshot(t, got, []RR{testSOA(1), newUnk})
	})

	t.Run("inzone_delete_missing_and_add_duplicate_error", func(t *testing.T) {
		ghost := testRR("ghost.zone.test. 300 IN A 203.0.113.66")
		delMissing := []RR{
			testSOA(1), testSOA(4294967294), ghost, testSOA(1), testSOA(1),
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, delMissing)
		})
		if _, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294)}); err == nil {
			t.Fatal("delete of a missing record did not error")
		}
		cleanup()

		existing := testRR("dup.zone.test. 300 IN A 203.0.113.7")
		addDup := []RR{
			testSOA(1), testSOA(4294967294), testSOA(1), existing, testSOA(1),
		}
		client2, cleanup2 := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, addDup)
		})
		defer cleanup2()
		if _, err := pipeTransfer(client2).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294), existing}); err == nil {
			t.Fatal("add of a duplicate identity did not error")
		}
	})

	t.Run("inzone_midstream_read_error", func(t *testing.T) {
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			// Only the leading and old SOA; the server then hangs up.
			_ = pipeReply(sc, req, []RR{testSOA(1), testSOA(4294967294)})
		})
		defer cleanup()

		if _, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", baseZone()); err == nil {
			t.Fatal("truncated transfer did not error")
		}
	})

	t.Run("inzone_bad_closure_error", func(t *testing.T) {
		stream := []RR{
			testSOA(1),
			testSOA(4294967294), aOld, testSOA(1), aNew,
			testSOA(2), // closing SOA does not match the current serial 1
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, stream)
		})
		defer cleanup()

		if _, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294), aOld}); err == nil {
			t.Fatal("bad closing SOA did not error")
		}
	})

	t.Run("inzone_tsig_failure_aborts", func(t *testing.T) {
		provider := tsigSecretProvider(map[string]string{CanonicalName(testApex): "c2VjcmV0"})
		client, cleanup := pipeServerWithProvider(t, provider, func(sc *Conn, req *Msg) {
			// The server can read the signed request, but never signs the
			// reply, so the client must reject it during TSIG verification.
			_ = pipeReply(sc, req, []RR{testSOA(4294967294)})
		})
		defer cleanup()

		tr := pipeTransfer(client)
		tr.TsigProvider = provider
		q := ixfrQuery(4294967294)
		q.SetTsig(CanonicalName(testApex), HmacSHA256, 300, time.Now().Unix())
		if _, err := tr.InZone(q, "unused", []RR{testSOA(4294967294)}); !errors.Is(err, ErrNoSig) {
			t.Fatalf("TSIG failure error = %v, want ErrNoSig", err)
		}
	})

	t.Run("base_validation_rejects_bad_snapshots", func(t *testing.T) {
		good := baseZone()
		badCases := map[string][]RR{
			"no_soa":             {testRR("x.zone.test. 300 IN A 192.0.2.1")},
			"two_soa":            {testSOA(1), testSOA(1)},
			"duplicate_identity": {testSOA(1), aOld, aOld},
			"outside_zone":       {testSOA(1), testRR("other.test. 300 IN A 192.0.2.2")},
			"wrong_class":        {testSOA(1), testRR("x.zone.test. 300 CH A 192.0.2.2")},
			"conflicting_ttl": {
				testSOA(1),
				testRR("x.zone.test. 100 IN A 192.0.2.1"),
				testRR("x.zone.test. 200 IN A 192.0.2.9"),
			},
		}
		for name, bad := range badCases {
			t.Run(name, func(t *testing.T) {
				if _, _, err := validateBase(testApex, bad); err == nil {
					t.Fatalf("base %q accepted", name)
				}
			})
		}
		if _, _, err := validateBase(testApex, good); err != nil {
			t.Fatalf("good base rejected: %v", err)
		}
		if _, _, err := validateBase(testApex, nil); err == nil {
			t.Fatal("empty base accepted")
		}
	})

	t.Run("query_serial_must_match_base", func(t *testing.T) {
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, []RR{testSOA(1)})
		})
		defer cleanup()

		if _, err := pipeTransfer(client).InZone(ixfrQuery(4294967293), "unused", baseZone()); err == nil {
			t.Fatal("query/base serial mismatch did not error")
		}
	})
}

func TestZoneTransferOwnership(t *testing.T) {
	aOld := testRR("x.zone.test. 300 IN A 192.0.2.1")
	aNew := testRR("x.zone.test. 300 IN A 192.0.2.2")

	successfulStream := []RR{
		testSOA(1),
		testSOA(4294967294), aOld,
		testSOA(1), aNew,
		testSOA(1),
	}

	t.Run("inputs_unmodified_on_success", func(t *testing.T) {
		base := []RR{testSOA(4294967294), aOld, testRR(`txt.zone.test. 300 IN TXT "keep"`)}
		baseBefore := snapshotKeys(t, base)
		q := ixfrQuery(4294967294)
		qBefore := q.Copy()

		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, successfulStream)
		})
		defer cleanup()

		got, err := pipeTransfer(client).InZone(q, "unused", base)
		if err != nil {
			t.Fatalf("InZone: %v", err)
		}
		if !reflect.DeepEqual(snapshotKeys(t, base), baseBefore) {
			t.Fatal("base snapshot was modified")
		}
		if q.Question[0] != qBefore.Question[0] || q.Ns[0].(*SOA).Serial != qBefore.Ns[0].(*SOA).Serial {
			t.Fatal("query message was modified")
		}
		if len(got) == 0 {
			t.Fatal("empty result")
		}
	})

	t.Run("result_independent_of_inputs", func(t *testing.T) {
		base := []RR{testSOA(4294967294), aOld}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, successfulStream)
		})
		defer cleanup()

		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", base)
		if err != nil {
			t.Fatalf("InZone: %v", err)
		}
		assertSnapshot(t, got, []RR{testSOA(1), aNew})

		// No result object may be the same pointer as a base object.
		basePtrs := map[RR]bool{}
		for _, r := range base {
			basePtrs[r] = true
		}
		for _, r := range got {
			if basePtrs[r] {
				t.Fatalf("result RR %s aliases a base record object", r.String())
			}
		}

		// Mutating the result must not reach base.
		got[1].Header().Ttl = 999
		if base[1].Header().Ttl != 300 {
			t.Fatal("mutating a result TTL changed the base snapshot")
		}

		// Mutating base must not reach the result.
		base[0].Header().Ttl = 7
		if got[0].Header().Ttl != 60 {
			t.Fatal("mutating a base TTL changed the result")
		}
	})

	t.Run("txt_rdata_not_shared", func(t *testing.T) {
		txt := testRR(`txt.zone.test. 300 IN TXT "seed"`)
		stream := []RR{
			testSOA(1), testSOA(4294967294), testSOA(1), txt, testSOA(1),
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, stream)
		})
		defer cleanup()

		base := []RR{testSOA(4294967294)}
		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", base)
		if err != nil {
			t.Fatalf("InZone: %v", err)
		}
		var gotTxt *TXT
		for _, rr := range got {
			if x, ok := rr.(*TXT); ok {
				gotTxt = x
			}
		}
		if gotTxt == nil || gotTxt.Txt[0] != "seed" {
			t.Fatalf("TXT not transferred: %v", got)
		}
		gotTxt.Txt[0] = "changed"
		if txt.(*TXT).Txt[0] != "seed" {
			t.Fatal("TXT rdata slice is shared between input and result")
		}
	})

	t.Run("rfc3597_rdata_not_shared", func(t *testing.T) {
		unk := testRR(`u.zone.test. 300 IN TYPE997 \# 4 010203ff`)
		stream := []RR{
			testSOA(1), testSOA(4294967294), testSOA(1), unk, testSOA(1),
		}
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			_ = pipeReply(sc, req, stream)
		})
		defer cleanup()

		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294)})
		if err != nil {
			t.Fatalf("InZone: %v", err)
		}
		var gotUnk, baseUnk *RFC3597
		for _, rr := range got {
			if x, ok := rr.(*RFC3597); ok {
				gotUnk = x
			}
		}
		baseUnk = unk.(*RFC3597)
		if gotUnk == nil {
			t.Fatal("RFC3597 missing from result")
		}
		if gotUnk == baseUnk {
			t.Fatal("RFC3597 pointer is shared between input and result")
		}
		if gotUnk.Rdata != baseUnk.Rdata {
			t.Fatalf("RFC3597 rdata = %q, want %q", gotUnk.Rdata, baseUnk.Rdata)
		}
	})

	t.Run("inputs_unmodified_on_failure", func(t *testing.T) {
		base := []RR{testSOA(4294967294), aOld}
		baseBefore := snapshotKeys(t, base)
		q := ixfrQuery(4294967294)

		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			// Truncated, invalid delta.
			_ = pipeReply(sc, req, []RR{testSOA(1), testSOA(4294967294)})
		})
		defer cleanup()

		if got, err := pipeTransfer(client).InZone(q, "unused", base); err == nil {
			t.Fatalf("expected failure, got %v", got)
		}
		if !reflect.DeepEqual(snapshotKeys(t, base), baseBefore) {
			t.Fatal("base snapshot modified by a failed transfer")
		}
		if q.Ns[0].(*SOA).Serial != 4294967294 {
			t.Fatal("query modified by a failed transfer")
		}
	})

	t.Run("no_partial_result_on_failure", func(t *testing.T) {
		client, cleanup := pipeServer(t, func(sc *Conn, req *Msg) {
			stream := []RR{
				testSOA(1),
				testSOA(4294967294), aOld, testSOA(1), aNew,
				testSOA(9), // invalid closing SOA
			}
			_ = pipeReply(sc, req, stream)
		})
		defer cleanup()

		got, err := pipeTransfer(client).InZone(ixfrQuery(4294967294), "unused", []RR{testSOA(4294967294), aOld})
		if err == nil {
			t.Fatal("expected a closure error")
		}
		if got != nil {
			t.Fatalf("failed transfer published a partial snapshot: %v", got)
		}
	})
}
