// Copyright 2019 The mqtt-go authors.
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
)

type pktUnsubscribe struct {
	ID     uint16
	Topics []string
}

func (p *pktUnsubscribe) Pack() []byte {
	payload := make([]byte, 0, packetBufferCap)
	for _, sub := range p.Topics {
		payload = appendString(payload, sub)
	}

	return pack(
		byte(packetUnsubscribe|packetFromClient),
		packUint16(p.ID),
		payload,
	)
}

// Unsubscribe topics.
func (c *BaseClient) Unsubscribe(ctx context.Context, subs ...string) error {
	return unsubscribeImpl(ctx, c, subs...)
}

func unsubscribeImpl(ctx context.Context, c *BaseClient, subs ...string) error {
	op := newOutboundOp(outboundUnsubscribe, QoS0)
	return unsubscribeOp(ctx, c, op, subs)
}

func unsubscribeOp(ctx context.Context, c *BaseClient, op *outboundOp, subs []string) error {
	c.muConnecting.RLock()
	defer c.muConnecting.RUnlock()

	if op.isDone() {
		// A late UNSUBACK completed this operation while the previous
		// attempt was giving up.
		return nil
	}

	if err := op.acquire(c); err != nil {
		return err
	}
	if op.isDone() {
		return nil
	}
	id := op.currentID()

	retryUnsubscribe := func(ctx context.Context, cli *BaseClient) error {
		return unsubscribeOp(ctx, cli, op, subs)
	}

	pkt := (&pktUnsubscribe{ID: id, Topics: subs}).Pack()
	if err := c.write(pkt); err != nil {
		// A partially written packet has an unknown boundary; the transport
		// cannot be reused.
		op.abortTransport()
		return wrapErrorWithRetry(err, retryUnsubscribe, "sending UNSUBSCRIBE")
	}
	select {
	case <-c.connClosed:
		return wrapErrorWithRetry(ErrClosedTransport, retryUnsubscribe, "waiting UNSUBACK")
	case <-ctx.Done():
		return wrapErrorWithRetry(ctx.Err(), retryUnsubscribe, "waiting UNSUBACK")
	case <-op.unsubAckChannel():
	}
	return nil
}
