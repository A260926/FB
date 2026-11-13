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
	"errors"
)

type publishFlag byte

const (
	publishFlagRetain  publishFlag = 0x01
	publishFlagQoS0    publishFlag = 0x00
	publishFlagQoS1    publishFlag = 0x02
	publishFlagQoS2    publishFlag = 0x04
	publishFlagQoSMask publishFlag = 0x06
	publishFlagDup     publishFlag = 0x08
)

// ErrPayloadLenExceeded means the payload length is too large.
var ErrPayloadLenExceeded = errors.New("payload length exceeded")

// ErrInvalidQoS means the QoS value is not allowed.
var ErrInvalidQoS = errors.New("invalid QoS")

type pktPublish struct {
	Message *Message
}

func (p *pktPublish) Parse(flag byte, contents []byte) (*pktPublish, error) {
	p.Message = &Message{
		Dup:    (publishFlag(flag) & publishFlagDup) != 0,
		Retain: (publishFlag(flag) & publishFlagRetain) != 0,
	}
	switch publishFlag(flag) & publishFlagQoSMask {
	case publishFlagQoS0:
		p.Message.QoS = QoS0
	case publishFlagQoS1:
		p.Message.QoS = QoS1
	case publishFlagQoS2:
		p.Message.QoS = QoS2
	default:
		return nil, wrapErrorf(
			ErrInvalidPacket, "parsing PUBLISH: QoS %d", int(publishFlag(flag)&publishFlagQoSMask),
		)
	}

	var n, nID int
	var err error
	n, p.Message.Topic, err = unpackString(contents)
	if err != nil {
		return nil, err
	}
	if p.Message.QoS != QoS0 {
		if len(contents)-n < 2 {
			return nil, wrapError(ErrInvalidPacketLength, "parsing PUBLISH")
		}
		nID, p.Message.ID = unpackUint16(contents[n:])
	}
	p.Message.Payload = contents[n+nID:]

	return p, nil
}

func (p *pktPublish) Pack() []byte {
	pktHeader := packetPublish.b()

	if p.Message.Retain {
		pktHeader |= byte(publishFlagRetain)
	}
	switch p.Message.QoS {
	case QoS0:
		pktHeader |= byte(publishFlagQoS0)
	case QoS1:
		pktHeader |= byte(publishFlagQoS1)
	case QoS2:
		pktHeader |= byte(publishFlagQoS2)
	default:
		panic("invalid QoS")
	}
	if p.Message.Dup {
		pktHeader |= byte(publishFlagDup)
	}

	header := make([]byte, 0, packetBufferCap)
	header = appendString(header, p.Message.Topic)
	if p.Message.QoS != QoS0 {
		header = appendUint16(header, p.Message.ID)
	}

	return pack(
		pktHeader,
		header,
		p.Message.Payload,
	)
}

// ValidateMessage validates given message.
func (c *BaseClient) ValidateMessage(message *Message) error {
	if c.MaxPayloadLen != 0 && len(message.Payload) >= c.MaxPayloadLen {
		return wrapErrorf(ErrPayloadLenExceeded, "payload length %d", len(message.Payload))
	}
	if message.QoS > QoS2 {
		return wrapErrorf(ErrInvalidQoS, "QoS%d", int(message.QoS))
	}
	return nil
}

// Publish a message to the broker.
// ID field of the message is filled if zero.
func (c *BaseClient) Publish(ctx context.Context, message *Message) error {
	if err := c.ValidateMessage(message); err != nil {
		return err
	}

	op := newOutboundOp(outboundPublish, message.QoS)
	if message.QoS > QoS0 && message.ID != 0 {
		// An explicitly specified identifier must be reserved atomically.
		// It is never silently replaced by another one.
		op.explicit = true
		op.requested = message.ID
	}
	return publishWithOp(ctx, c, op, message, false)
}

// publishWithOp acquires the connection lock for one publish attempt.
func publishWithOp(ctx context.Context, c *BaseClient, op *outboundOp, message *Message, dup bool) error {
	c.muConnecting.RLock()
	defer c.muConnecting.RUnlock()

	return publishOp(ctx, c, op, message, dup)
}

// publishOp runs one attempt of a logical publish operation. The same op is
// carried across retries, so the identifier and the protocol phase of the
// operation are reused on the original transport and re-reserved on a new one.
// c.muConnecting must be read-locked by the caller.
func publishOp(ctx context.Context, c *BaseClient, op *outboundOp, message *Message, dup bool) error {
	// A late terminal acknowledgement may have completed this operation
	// while the caller was giving up.
	if op.isDone() {
		return nil
	}

	message.Dup = dup

	if op.qos > QoS0 {
		if err := op.acquire(c); err != nil {
			return err
		}
		message.ID = op.currentID()
		if op.isDone() {
			// A late terminal acknowledgement completed the operation while
			// the previous attempt was giving up.
			return nil
		}
		if op.qos == QoS2 && op.phaseSnapshot() == phasePubComp {
			// PUBREC was already consumed before the retry; the operation
			// must continue from PUBREL, not restart PUBLISH.
			op.resumePubComp()
			return publishPubRel(ctx, c, op, message)
		}
	} else {
		if _, err := c.signaller(); err != nil {
			return err
		}
		if message.ID == 0 {
			// Message.ID is still filled for QoS0, but QoS0 never owns an
			// identifier in the shared namespace.
			message.ID = c.newID()
		}
	}

	retryPublish := func(ctx context.Context, cli *BaseClient) error {
		return publishWithOp(ctx, cli, op, message, true)
	}

	pkt := (&pktPublish{Message: message}).Pack()
	if err := c.write(pkt); err != nil {
		// A partially written packet has an unknown boundary; the transport
		// cannot be reused. Closing it also settles the leases of this
		// generation.
		op.abortTransport()
		if op.qos > QoS0 {
			return wrapErrorWithRetry(err, retryPublish, "sending PUBLISH")
		}
		return wrapError(err, "sending PUBLISH")
	}

	if message.QoS > QoS0 && op.isDone() {
		// The terminal acknowledgement of a retried operation may have
		// arrived before this duplicate write.
		return nil
	}

	switch op.qos {
	case QoS1:
		select {
		case <-c.connClosed:
			return wrapErrorWithRetry(ErrClosedTransport, retryPublish, "waiting PUBACK")
		case <-ctx.Done():
			return wrapErrorWithRetry(ctx.Err(), retryPublish, "waiting PUBACK")
		case <-op.pubAckChannel():
		}
	case QoS2:
		select {
		case <-c.connClosed:
			return wrapErrorWithRetry(ErrClosedTransport, retryPublish, "waiting PUBREC")
		case <-ctx.Done():
			return wrapErrorWithRetry(ctx.Err(), retryPublish, "waiting PUBREC")
		case <-op.pubRecChannel():
		}
		op.resumePubComp()
		return publishPubRel(ctx, c, op, message)
	}
	return nil
}

// publishPubRel sends the PUBREL of a QoS2 publish and waits for PUBCOMP.
// The identifier stays owned by op throughout this phase.
// c.muConnecting must be read-locked by the caller.
func publishPubRel(ctx context.Context, c *BaseClient, op *outboundOp, message *Message) error {
	retry := func(ctx context.Context, cli *BaseClient) error {
		return publishWithOp(ctx, cli, op, message, true)
	}

	if err := op.acquire(c); err != nil {
		return err
	}
	if op.isDone() {
		return nil
	}
	op.resumePubComp()
	message.ID = op.currentID()

	pktPubRel := (&pktPubRel{ID: message.ID}).Pack()
	if err := c.write(pktPubRel); err != nil {
		op.abortTransport()
		return wrapErrorWithRetry(err, retry, "sending PUBREL")
	}
	select {
	case <-c.connClosed:
		return wrapErrorWithRetry(ErrClosedTransport, retry, "waiting PUBCOMP")
	case <-ctx.Done():
		return wrapErrorWithRetry(ctx.Err(), retry, "waiting PUBCOMP")
	case <-op.pubCompChannel():
	}
	return nil
}
