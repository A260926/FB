// Copyright 2026 The mqtt-go authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package mqtt

import (
	"context"
	"encoding/binary"
	"errors"
	"net"
	"sync"
	"testing"
	"time"
)

// testBroker is a net.Pipe peer speaking raw MQTT bytes. A background reader
// decodes every client packet so that writes never block on the test
// goroutine; tests observe identifiers and drive confirmations through it.
type testBroker struct {
	t  *testing.T
	ca net.Conn
	cb net.Conn

	chPkt  chan brokerPacket
	closed chan struct{}
}

type brokerPacket struct {
	kind packetType
	body []byte
}

func newTestBroker(t *testing.T) (*testBroker, *BaseClient) {
	t.Helper()
	ca, cb := net.Pipe()
	b := &testBroker{
		t: t, ca: ca, cb: cb,
		chPkt:  make(chan brokerPacket, 64),
		closed: make(chan struct{}),
	}
	go b.readLoop()
	return b, &BaseClient{Transport: cb}
}

func (b *testBroker) readLoop() {
	defer close(b.closed)
	for {
		kind, _, body, err := readPacket(b.ca)
		if err != nil {
			return
		}
		b.chPkt <- brokerPacket{kind: kind, body: body}
	}
}

func (b *testBroker) close() {
	_ = b.ca.Close()
	_ = b.cb.Close()
}

// acceptConnect reads CONNECT and replies with an accepting CONNACK.
func (b *testBroker) acceptConnect() {
	b.t.Helper()
	p := b.next()
	if p.kind != packetConnect {
		b.t.Fatalf("expected CONNECT, got %v", p.kind)
	}
	b.write([]byte{byte(packetConnAck), 0x02, 0x00, 0x00})
}

func (b *testBroker) write(pkt []byte) {
	b.t.Helper()
	if _, err := b.ca.Write(pkt); err != nil {
		b.t.Errorf("broker write: %v", err)
	}
}

// id returns the packet identifier of a SUBSCRIBE/UNSUBSCRIBE/PUBLISH/PUBREL packet.
func (p brokerPacket) id() uint16 {
	switch p.kind {
	case packetSubscribe, packetUnsubscribe, packetPubRel:
		return binary.BigEndian.Uint16(p.body[:2])
	case packetPublish:
		// Skip the topic string.
		n := int(binary.BigEndian.Uint16(p.body[:2]))
		off := 2 + n
		return binary.BigEndian.Uint16(p.body[off : off+2])
	}
	return 0
}

func (p brokerPacket) topic() string {
	switch p.kind {
	case packetSubscribe:
		n := int(binary.BigEndian.Uint16(p.body[2:4]))
		return string(p.body[4 : 4+n])
	case packetUnsubscribe:
		n := int(binary.BigEndian.Uint16(p.body[2:4]))
		return string(p.body[4 : 4+n])
	case packetPublish:
		n := int(binary.BigEndian.Uint16(p.body[:2]))
		return string(p.body[2 : 2+n])
	}
	return ""
}

// next waits for the next packet observed by the reader.
func (b *testBroker) next() brokerPacket {
	b.t.Helper()
	select {
	case p := <-b.chPkt:
		return p
	case <-time.After(2 * time.Second):
		b.t.Fatal("timeout waiting for packet")
		return brokerPacket{}
	}
}

// expectNoPacket asserts that no packet reaches the broker. The reader is
// still running, so any bytes written by the client are visible here.
func (b *testBroker) expectNoPacket(d time.Duration) {
	b.t.Helper()
	select {
	case p := <-b.chPkt:
		b.t.Fatalf("unexpected packet written to the transport: %v", p.kind)
	case <-time.After(d):
	}
}

func subAckPacket(id uint16) []byte {
	return []byte{byte(packetSubAck), 0x03, byte(id >> 8), byte(id), 0x00}
}
func unsubAckPacket(id uint16) []byte {
	return []byte{byte(packetUnsubAck), 0x02, byte(id >> 8), byte(id)}
}
func pubAckPacket(id uint16) []byte {
	return []byte{byte(packetPubAck), 0x02, byte(id >> 8), byte(id)}
}
func pubRecPacket(id uint16) []byte {
	return []byte{byte(packetPubRec), 0x02, byte(id >> 8), byte(id)}
}
func pubCompPacket(id uint16) []byte {
	return []byte{byte(packetPubComp), 0x02, byte(id >> 8), byte(id)}
}

func connectClient(t *testing.T, cli *BaseClient, b *testBroker) context.Context {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	t.Cleanup(cancel)

	done := make(chan error, 1)
	go func() {
		_, err := cli.Connect(ctx, "test")
		done <- err
	}()
	b.acceptConnect()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Connect: %v", err)
		}
	case <-ctx.Done():
		t.Fatal("Connect timeout")
	}
	return ctx
}

// runSubscription starts a Subscribe in the background and returns its
// identifier and result channel.
func runSubscription(
	t *testing.T, ctx context.Context, cli *BaseClient, b *testBroker, topic string,
) (uint16, chan error) {
	t.Helper()
	chErr := make(chan error, 1)
	go func() {
		_, err := cli.Subscribe(ctx, Subscription{Topic: topic, QoS: QoS0})
		chErr <- err
	}()
	p := b.next()
	if p.kind != packetSubscribe {
		t.Fatalf("expected SUBSCRIBE, got %v", p.kind)
	}
	return p.id(), chErr
}

func waitErr(t *testing.T, ch chan error, what string) error {
	t.Helper()
	select {
	case err := <-ch:
		return err
	case <-time.After(2 * time.Second):
		t.Fatalf("timeout waiting for %s", what)
		return nil
	}
}

func expectPending(t *testing.T, ch chan error, what string) {
	t.Helper()
	select {
	case err := <-ch:
		t.Fatalf("%s unexpectedly completed: %v", what, err)
	case <-time.After(100 * time.Millisecond):
	}
}

func TestOutboundIDNamespace(t *testing.T) {
	t.Run("WrapSkipsInFlightSubscribe", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx := connectClient(t, cli, b)

		// Align the cursor so the next automatic allocation is 1.
		cli.idLast = 0
		firstID, first := runSubscription(t, ctx, cli, b, "a")
		if firstID != 1 {
			t.Fatalf("first SUBSCRIBE id: want 1, got %d", firstID)
		}

		// Force the cursor to wrap onto 1 again; it is still in flight.
		cli.idLast = 0xFFFF
		secondID, second := runSubscription(t, ctx, cli, b, "b")
		if secondID == firstID {
			t.Fatalf("wrapped allocation reused in-flight id %d", firstID)
		}
		if secondID != 2 {
			t.Fatalf("second SUBSCRIBE id: want 2 (skipping in-flight 1), got %d", secondID)
		}

		// Only the second identifier's SUBACK is sent. The first listener
		// must keep waiting.
		b.write(subAckPacket(secondID))
		if err := waitErr(t, second, "second SUBSCRIBE"); err != nil {
			t.Fatalf("second SUBSCRIBE: %v", err)
		}
		expectPending(t, first, "first SUBSCRIBE")

		b.write(subAckPacket(firstID))
		if err := waitErr(t, first, "first SUBSCRIBE"); err != nil {
			t.Fatalf("first SUBSCRIBE: %v", err)
		}
	})

	t.Run("CrossKindConflictAndExplicitID", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx := connectClient(t, cli, b)

		cli.idLast = 0
		subID, sub := runSubscription(t, ctx, cli, b, "a")
		if subID != 1 {
			t.Fatalf("SUBSCRIBE id: want 1, got %d", subID)
		}

		// A PUBLISH explicitly asking for the identifier owned by the in-flight
		// SUBSCRIBE must fail atomically and write nothing.
		errCh := make(chan error, 1)
		go func() {
			errCh <- cli.Publish(ctx, &Message{
				Topic: "p", ID: subID, QoS: QoS1,
			})
		}()
		err := waitErr(t, errCh, "explicit publish")
		if !errors.Is(err, ErrPacketIdentifierInUse) {
			t.Fatalf("want ErrPacketIdentifierInUse, got: %v", err)
		}
		b.expectNoPacket(50 * time.Millisecond)

		// The same identifier is also rejected for UNSUBSCRIBE when explicit
		// ids are not used; here the automatic allocation must avoid it
		// while the SUBSCRIBE is still in flight.
		unsubErr := make(chan error, 1)
		go func() {
			unsubErr <- cli.Unsubscribe(ctx, "a")
		}()
		unsubPkt := b.next()
		if unsubPkt.kind != packetUnsubscribe {
			t.Fatalf("expected UNSUBSCRIBE, got %v", unsubPkt.kind)
		}
		if unsubPkt.id() == subID {
			t.Fatalf("automatic UNSUBSCRIBE reused in-flight id %d", subID)
		}
		b.write(unsubAckPacket(unsubPkt.id()))
		if err := waitErr(t, unsubErr, "UNSUBSCRIBE"); err != nil {
			t.Fatalf("UNSUBSCRIBE: %v", err)
		}

		// But a QoS0 PUBLISH does not participate in the namespace: an explicit
		// ID is only a wire value there, and it must not conflict.
		zeroErr := make(chan error, 1)
		go func() {
			zeroErr <- cli.Publish(ctx, &Message{Topic: "p", ID: subID, QoS: QoS0})
		}()
		p := b.next()
		if p.kind != packetPublish || p.topic() != "p" {
			t.Fatalf("expected QoS0 PUBLISH, got %v %q", p.kind, p.topic())
		}
		if err := waitErr(t, zeroErr, "QoS0 publish"); err != nil {
			t.Fatalf("QoS0 publish with explicit id: %v", err)
		}

		b.write(subAckPacket(subID))
		if err := waitErr(t, sub, "SUBSCRIBE"); err != nil {
			t.Fatalf("SUBSCRIBE: %v", err)
		}
	})

	t.Run("TwoConcurrentAllocatorsAndIndependentClients", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx := connectClient(t, cli, b)

		b2, cli2 := newTestBroker(t)
		defer b2.close()
		ctx2 := connectClient(t, cli2, b2)

		// Both clients start from the same cursor value. Identifiers are
		// per-transport, so each must get the same first id.
		cli.idLast = 100
		cli2.idLast = 100

		chErr := make(chan error, 4)
		go func() {
			_, err := cli.Subscribe(ctx, Subscription{Topic: "a", QoS: QoS0})
			chErr <- err
		}()
		go func() {
			_, err := cli2.Subscribe(ctx2, Subscription{Topic: "a", QoS: QoS0})
			chErr <- err
		}()
		p1 := b.next()
		p2 := b2.next()
		if p1.id() != p2.id() {
			t.Fatalf("identifiers of independent clients must not be coordinated: %d vs %d",
				p1.id(), p2.id())
		}

		// Two allocations racing on one transport must never collide.
		cli.idLast = 0xFFFF
		got := make(map[uint16]bool)
		var wg sync.WaitGroup
		for i := 0; i < 2; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				chErr <- cli.Unsubscribe(ctx, "x")
			}()
		}
		for i := 0; i < 2; i++ {
			p := b.next()
			if p.kind != packetUnsubscribe {
				t.Fatalf("expected UNSUBSCRIBE, got %v", p.kind)
			}
			if got[p.id()] {
				t.Errorf("concurrent allocators produced duplicate id %d", p.id())
			}
			got[p.id()] = true
			b.write(unsubAckPacket(p.id()))
		}
		wg.Wait()

		b.write(subAckPacket(p1.id()))
		b2.write(subAckPacket(p2.id()))
		for i := 0; i < 4; i++ {
			if err := waitErr(t, chErr, "concurrent op"); err != nil {
				t.Fatalf("concurrent op: %v", err)
			}
		}
	})

	t.Run("Exhaustion", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx := connectClient(t, cli, b)

		// Prepare the full state in-package: every identifier is owned by an
		// in-flight operation of this transport.
		sig := cli.sig
		sig.mu.Lock()
		sig.used = make(map[uint16]*outboundOp, maxPacketIdentifier)
		for id := 1; id <= maxPacketIdentifier; id++ {
			op := newOutboundOp(outboundSubscribe, QoS0)
			op.id = uint16(id)
			op.reg = sig
			op.phase = phaseSubAck
			sig.used[uint16(id)] = op
		}
		sig.mu.Unlock()

		_, err := cli.Subscribe(ctx, Subscription{Topic: "a"})
		if err == nil {
			t.Fatal("expected an error when exhausted")
		}
		if !errors.Is(err, ErrPacketIdentifiersExhausted) {
			t.Fatalf("want ErrPacketIdentifiersExhausted, got: %v", err)
		}
		b.expectNoPacket(50 * time.Millisecond)

		// Unsubscribe shares the namespace and must report the same identity.
		err = cli.Unsubscribe(ctx, "a")
		if !errors.Is(err, ErrPacketIdentifiersExhausted) {
			t.Fatalf("want ErrPacketIdentifiersExhausted, got: %v", err)
		}
		b.expectNoPacket(50 * time.Millisecond)

		// A QoS0 publish neither consumes nor observes the namespace.
		pubErr := make(chan error, 1)
		go func() {
			pubErr <- cli.Publish(ctx, &Message{Topic: "a", QoS: QoS0})
		}()
		p := b.next()
		if p.kind != packetPublish {
			t.Fatalf("expected PUBLISH, got %v", p.kind)
		}
		if err := waitErr(t, pubErr, "QoS0 publish"); err != nil {
			t.Fatalf("QoS0 publish: %v", err)
		}
	})
}

func TestOutboundIDPhases(t *testing.T) {
	t.Run("QoS2HeldUntilPubComp", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx := connectClient(t, cli, b)

		cli.idLast = 0
		errCh := make(chan error, 1)
		go func() {
			errCh <- cli.Publish(ctx, &Message{Topic: "p", QoS: QoS2})
		}()
		p := b.next()
		if p.kind != packetPublish {
			t.Fatalf("expected PUBLISH, got %v", p.kind)
		}
		pubID := p.id()

		// PUBREC moves the operation to PUBREL; the identifier must still
		// be owned, not handed to the SUBSCRIBE below.
		b.write(pubRecPacket(pubID))
		p = b.next()
		if p.kind != packetPubRel || p.id() != pubID {
			t.Fatalf("expected PUBREL id %d, got %v id %d", pubID, p.kind, p.id())
		}

		cli.idLast = uint32(pubID) - 1 // cursor would wrap onto pubID
		subID, sub := runSubscription(t, ctx, cli, b, "s")
		if subID == pubID {
			t.Fatalf("QoS2 id %d was recycled during PUBREL/PUBCOMP", pubID)
		}

		// PUBCOMP completes only the publish; the SUBSCRIBE stays pending.
		b.write(pubCompPacket(pubID))
		if err := waitErr(t, errCh, "QoS2 publish"); err != nil {
			t.Fatalf("QoS2 publish: %v", err)
		}
		expectPending(t, sub, "SUBSCRIBE")

		b.write(subAckPacket(subID))
		if err := waitErr(t, sub, "SUBSCRIBE"); err != nil {
			t.Fatalf("SUBSCRIBE: %v", err)
		}
	})

	t.Run("ReleaseOnlyAtTerminalAck", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx := connectClient(t, cli, b)

		cli.idLast = 0
		subID, sub := runSubscription(t, ctx, cli, b, "a")

		// A duplicate SUBACK must not release anything twice or disturb
		// another kind's waiting entry.
		b.write(subAckPacket(subID))
		if err := waitErr(t, sub, "SUBSCRIBE"); err != nil {
			t.Fatalf("SUBSCRIBE: %v", err)
		}

		cli.idLast = uint32(subID) - 1
		pubErr := make(chan error, 1)
		go func() {
			pubErr <- cli.Publish(ctx, &Message{Topic: "p", QoS: QoS1, ID: subID})
		}()
		p := b.next()
		if p.kind != packetPublish || p.id() != subID {
			t.Fatalf("expected reuse of released id %d, got %v id %d", subID, p.kind, p.id())
		}

		// Duplicate and unknown confirmations must not complete this publish.
		b.write(subAckPacket(subID))
		b.write(pubAckPacket(4242))
		expectPending(t, pubErr, "PUBLISH after duplicate SUBACK")

		b.write(pubAckPacket(subID))
		if err := waitErr(t, pubErr, "PUBLISH"); err != nil {
			t.Fatalf("PUBLISH: %v", err)
		}
	})

	t.Run("LateAckSettlesOnlyOldOperation", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx := connectClient(t, cli, b)

		cli.idLast = 0
		firstID, first := runSubscription(t, ctx, cli, b, "a")
		if firstID != 1 {
			t.Fatalf("first id: want 1, got %d", firstID)
		}

		// Caller gives up, but the server-side request is still outstanding.
		cancelCtx, cancel := context.WithCancel(ctx)
		cancelResult := make(chan error, 1)
		go func() {
			_, err := cli.Subscribe(cancelCtx, Subscription{Topic: "b", QoS: QoS0})
			cancelResult <- err
		}()
		secondID := b.next().id()
		if secondID == firstID {
			t.Fatalf("second SUBSCRIBE reused in-flight id %d", firstID)
		}
		cancel()
		if err := waitErr(t, cancelResult, "cancelled SUBSCRIBE"); !errors.Is(err, context.Canceled) {
			t.Fatalf("want context.Canceled, got: %v", err)
		}

		// The cancelled operation still owns secondID; a retry of the
		// original request must not strand it.
		b.write(subAckPacket(firstID))
		if err := waitErr(t, first, "first SUBSCRIBE"); err != nil {
			t.Fatalf("first SUBSCRIBE: %v", err)
		}

		// A late SUBACK for the cancelled operation only settles that
		// operation. The next allocate must skip the still-owned secondID.
		cli.idLast = uint32(secondID) - 1
		thirdID, third := runSubscription(t, ctx, cli, b, "c")
		if thirdID == secondID {
			t.Fatalf("id %d still owned by the cancelled operation was recycled", secondID)
		}
		b.write(unsubAckPacket(secondID)) // wrong kind, must be ignored
		expectPending(t, third, "third SUBSCRIBE")

		b.write(subAckPacket(secondID))
		expectPending(t, third, "third SUBSCRIBE after settling old op")
		b.write(subAckPacket(thirdID))
		if err := waitErr(t, third, "third SUBSCRIBE"); err != nil {
			t.Fatalf("third SUBSCRIBE: %v", err)
		}

		// Only now may secondID be handed out again.
		cli.idLast = uint32(secondID) - 1
		var reID uint16
		chErr := make(chan error, 1)
		go func() {
			chErr <- cli.Unsubscribe(ctx, "d")
		}()
		p := b.next()
		reID = p.id()
		if reID != secondID {
			t.Fatalf("settled id %d should be reusable, got %d", secondID, reID)
		}
		b.write(unsubAckPacket(reID))
		if err := waitErr(t, chErr, "UNSUBSCRIBE"); err != nil {
			t.Fatalf("UNSUBSCRIBE: %v", err)
		}
	})

	t.Run("ReconnectGenerationIsolation", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx := connectClient(t, cli, b)

		// Leave an unacknowledged request behind on the old generation. The
		// caller gives up (releasing the connection lock) but the lease
		// stays with the old transport until its receive loop ends.
		cli.idLast = 0
		short, cancel := context.WithTimeout(ctx, 100*time.Millisecond)
		defer cancel()
		_, err := cli.Subscribe(short, Subscription{Topic: "a", QoS: QoS0})
		if !errors.Is(err, context.DeadlineExceeded) {
			t.Fatalf("want deadline exceeded, got: %v", err)
		}
		oldPkt := b.next()
		if oldPkt.kind != packetSubscribe {
			t.Fatalf("expected SUBSCRIBE, got %v", oldPkt.kind)
		}
		oldID := oldPkt.id()

		// Replace the transport and open a new generation on the same
		// BaseClient while the old receive loop is still draining.
		ca2, cb2 := net.Pipe()
		defer ca2.Close()
		defer cb2.Close()
		cli.Transport = cb2
		b2 := &testBroker{
			t: t, ca: ca2, cb: cb2,
			chPkt:  make(chan brokerPacket, 16),
			closed: make(chan struct{}),
		}
		go b2.readLoop()
		connectDone := make(chan error, 1)
		go func() {
			_, err := cli.Connect(ctx, "test2")
			connectDone <- err
		}()
		b2.acceptConnect()
		if err := waitErr(t, connectDone, "reconnect"); err != nil {
			t.Fatalf("reconnect: %v", err)
		}

		// The new generation has its own namespace; the old lease does not
		// interfere with it.
		cli.idLast = 0
		newID, newSub := runSubscription(t, ctx, cli, b2, "b")
		if newID != 1 {
			t.Fatalf("new generation first id: want 1, got %d", newID)
		}

		// End the old receive loop so its deferred cleanup runs while the
		// new generation is live. It must only release the old lease.
		b.close()
		select {
		case <-b.closed:
		case <-time.After(2 * time.Second):
			t.Fatal("old receive loop did not terminate")
		}

		cli.idLast = 0
		thirdID, third := runSubscription(t, ctx, cli, b2, "c")
		if thirdID == newID {
			t.Fatalf("new generation id %d was released by old generation cleanup", newID)
		}

		// Both new-generation operations settle normally after the old
		// cleanup; the old identifier is irrelevant to this namespace.
		b2.write(subAckPacket(newID))
		if err := waitErr(t, newSub, "new SUBSCRIBE"); err != nil {
			t.Fatalf("new SUBSCRIBE: %v", err)
		}
		expectPending(t, third, "third SUBSCRIBE")
		b2.write(subAckPacket(thirdID))
		if err := waitErr(t, third, "third SUBSCRIBE"); err != nil {
			t.Fatalf("third SUBSCRIBE: %v", err)
		}
		_ = oldID
	})
}

func TestOutboundIDRetry(t *testing.T) {
	t.Run("SameTransportRetryReusesOwnLease", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx := connectClient(t, cli, b)

		cli.idLast = 0
		// First attempt's context times out while the request is outstanding.
		short, cancel := context.WithTimeout(ctx, 100*time.Millisecond)
		defer cancel()
		_, err := cli.Subscribe(short, Subscription{Topic: "a", QoS: QoS0})
		if !errors.Is(err, context.DeadlineExceeded) {
			t.Fatalf("want deadline exceeded, got: %v", err)
		}
		firstID := b.next().id()

		retryErr, ok := err.(ErrorWithRetry)
		if !ok {
			t.Fatalf("error does not implement ErrorWithRetry: %v", err)
		}

		// Retry on the same transport must reuse the same identifier and the
		// same protocol phase, not collide with its own lease.
		chErr := make(chan error, 1)
		go func() {
			chErr <- retryErr.Retry(ctx, cli)
		}()
		p := b.next()
		if p.kind != packetSubscribe {
			t.Fatalf("expected retried SUBSCRIBE, got %v", p.kind)
		}
		if p.id() != firstID {
			t.Fatalf("retry on the same transport must reuse id %d, got %d", firstID, p.id())
		}
		b.write(subAckPacket(firstID))
		if err := waitErr(t, chErr, "retried SUBSCRIBE"); err != nil {
			t.Fatalf("retried SUBSCRIBE: %v", err)
		}
	})

	t.Run("NewTransportReservesFreshID", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx := connectClient(t, cli, b)

		cli.idLast = 0
		short, cancel := context.WithTimeout(ctx, 100*time.Millisecond)
		defer cancel()
		_, err := cli.Subscribe(short, Subscription{Topic: "a", QoS: QoS0})
		if !errors.Is(err, context.DeadlineExceeded) {
			t.Fatalf("want deadline exceeded, got: %v", err)
		}
		oldID := b.next().id()
		retryErr, ok := err.(ErrorWithRetry)
		if !ok {
			t.Fatalf("error does not implement ErrorWithRetry: %v", err)
		}

		// A different BaseClient has an independent namespace. Occupy the
		// old wire id there so the retry must reserve a different one.
		ca2, cb2 := net.Pipe()
		defer ca2.Close()
		defer cb2.Close()
		b2 := &testBroker{
			t: t, ca: ca2, cb: cb2,
			chPkt:  make(chan brokerPacket, 16),
			closed: make(chan struct{}),
		}
		go b2.readLoop()
		cli2 := &BaseClient{Transport: cb2}
		ctx2 := connectClient(t, cli2, b2)

		cli2.idLast = 0
		heldID, held := runSubscription(t, ctx2, cli2, b2, "held")
		if heldID != oldID {
			t.Fatalf("preparation: occupied id %d, want %d", heldID, oldID)
		}

		// Retry through the new client and the retry entry point.
		chErr := make(chan error, 1)
		go func() {
			chErr <- retryErr.Retry(ctx2, cli2)
		}()
		p := b2.next()
		if p.kind != packetSubscribe {
			t.Fatalf("expected retried SUBSCRIBE, got %v", p.kind)
		}
		if p.id() == heldID {
			t.Fatalf("retry on a new transport collided with existing id %d", heldID)
		}
		newID := p.id()

		// The new transport's own SUBACK settles the retried operation only.
		b2.write(subAckPacket(newID))
		if err := waitErr(t, chErr, "retried SUBSCRIBE"); err != nil {
			t.Fatalf("retried SUBSCRIBE: %v", err)
		}
		expectPending(t, held, "pre-existing SUBSCRIBE")
		b2.write(subAckPacket(heldID))
		if err := waitErr(t, held, "pre-existing SUBSCRIBE"); err != nil {
			t.Fatalf("pre-existing SUBSCRIBE: %v", err)
		}
	})

	t.Run("QoS2PhaseRetryResumesAtPubRel", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx := connectClient(t, cli, b)

		cli.idLast = 0
		// The publish reaches the PUBCOMP wait, then the caller times out
		// while PUBREL is already on the wire.
		short, cancel := context.WithTimeout(ctx, 250*time.Millisecond)
		defer cancel()
		errCh := make(chan error, 1)
		go func() {
			errCh <- cli.Publish(short, &Message{Topic: "p", QoS: QoS2})
		}()
		p := b.next()
		if p.kind != packetPublish {
			t.Fatalf("expected PUBLISH, got %v", p.kind)
		}
		id := p.id()
		b.write(pubRecPacket(id))
		p = b.next()
		if p.kind != packetPubRel || p.id() != id {
			t.Fatalf("expected PUBREL id %d, got %v id %d", id, p.kind, p.id())
		}
		err := waitErr(t, errCh, "QoS2 publish")
		if !errors.Is(err, context.DeadlineExceeded) {
			t.Fatalf("want deadline exceeded, got: %v", err)
		}

		retryErr, ok := err.(ErrorWithRetry)
		if !ok {
			t.Fatalf("error does not implement ErrorWithRetry: %v", err)
		}

		// Retrying on the same transport must resume the PUBREL/PUBCOMP
		// phase with its own identifier; it must neither restart the
		// PUBLISH nor collide with its own lease.
		chErr := make(chan error, 1)
		go func() {
			chErr <- retryErr.Retry(ctx, cli)
		}()
		p = b.next()
		if p.kind != packetPubRel || p.id() != id {
			t.Fatalf("retry must resume PUBREL id %d, got %v id %d", id, p.kind, p.id())
		}
		b.write(pubCompPacket(id))
		if err := waitErr(t, chErr, "retried PUBLISH"); err != nil {
			t.Fatalf("retried PUBLISH: %v", err)
		}

		// Explicit conflicting id on a real call reports the conflict and
		// writes nothing.
		cli.idLast = 0
		holdErr := make(chan error, 1)
		go func() {
			holdErr <- cli.Publish(ctx, &Message{Topic: "q", QoS: QoS1, ID: 7})
		}()
		p = b.next()
		if p.kind != packetPublish || p.id() != 7 {
			t.Fatalf("explicit id: want PUBLISH 7, got %v id %d", p.kind, p.id())
		}
		conflictErr := make(chan error, 1)
		go func() {
			conflictErr <- cli.Publish(ctx, &Message{Topic: "r", QoS: QoS1, ID: 7})
		}()
		if err := waitErr(t, conflictErr, "conflicting explicit publish"); !errors.Is(err, ErrPacketIdentifierInUse) {
			t.Fatalf("want ErrPacketIdentifierInUse, got: %v", err)
		}
		b.expectNoPacket(50 * time.Millisecond)
		b.write(pubAckPacket(7))
		if err := waitErr(t, holdErr, "first explicit publish"); err != nil {
			t.Fatalf("first explicit publish: %v", err)
		}
	})

	t.Run("RetryClientQueueEntryPoint", func(t *testing.T) {
		b, cli := newTestBroker(t)
		defer b.close()
		ctx, cancel := context.WithTimeout(context.Background(), 4*time.Second)
		defer cancel()

		retryCli := &RetryClient{ResponseTimeout: 120 * time.Millisecond}
		retryCli.SetClient(ctx, cli)
		connectDone := make(chan error, 1)
		go func() {
			_, err := retryCli.Connect(ctx, "test")
			connectDone <- err
		}()
		b.acceptConnect()
		if err := waitErr(t, connectDone, "RetryClient.Connect"); err != nil {
			t.Fatalf("RetryClient.Connect: %v", err)
		}

		// The queue worker issues the PUBLISH and the response timeout
		// turns it into an ErrorWithRetry kept in the retry queue.
		if err := retryCli.Publish(ctx, &Message{Topic: "p", QoS: QoS1}); err != nil {
			t.Fatalf("RetryClient.Publish: %v", err)
		}
		p := b.next()
		if p.kind != packetPublish {
			t.Fatalf("expected PUBLISH, got %v", p.kind)
		}
		firstID := p.id()

		// A response timeout makes the worker close the old transport and
		// wait for a new client, as after a reconnect. Waiting for Done()
		// synchronizes with the worker, which has queued the retry by then.
		select {
		case <-cli.Done():
		case <-ctx.Done():
			t.Fatal("timeout waiting for the response timeout to close the client")
		}

		// A response timeout makes RetryClient close the old transport and
		// wait for a new one, as a reconnect would. Install a fresh
		// BaseClient and occupy firstID there, so the retried operation must
		// reserve a different identifier in the new namespace.
		ca2, cb2 := net.Pipe()
		defer ca2.Close()
		defer cb2.Close()
		b2 := &testBroker{
			t: t, ca: ca2, cb: cb2,
			chPkt:  make(chan brokerPacket, 16),
			closed: make(chan struct{}),
		}
		go b2.readLoop()
		cli2 := &BaseClient{Transport: cb2}
		retryCli.SetClient(ctx, cli2)
		reconnectDone := make(chan error, 1)
		go func() {
			_, err := retryCli.Connect(ctx, "test2")
			reconnectDone <- err
		}()
		b2.acceptConnect()
		if err := waitErr(t, reconnectDone, "RetryClient.Connect"); err != nil {
			t.Fatalf("RetryClient.Connect: %v", err)
		}

		cli2.idLast = uint32(firstID) - 1
		heldID, held := runSubscription(t, ctx, cli2, b2, "held")
		if heldID != firstID {
			t.Fatalf("preparation: occupied id %d, want %d", heldID, firstID)
		}

		// The real Retry entry point re-runs the queued operation on the new
		// transport, reserving a fresh id there.
		retryCli.Retry(ctx)
		p = b2.next()
		if p.kind != packetPublish {
			t.Fatalf("expected retried PUBLISH, got %v", p.kind)
		}
		if p.id() == heldID {
			t.Fatalf("retried operation collided with in-use id %d", heldID)
		}
		b2.write(pubAckPacket(p.id()))
		expectPending(t, held, "pre-existing SUBSCRIBE")
		b2.write(subAckPacket(heldID))
		if err := waitErr(t, held, "pre-existing SUBSCRIBE"); err != nil {
			t.Fatalf("pre-existing SUBSCRIBE: %v", err)
		}
	})
}
