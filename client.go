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
	"errors"
	"io"
	"sync"
	"time"
)

// ErrNotConnected is returned if a function is called before Connect.
var ErrNotConnected = errors.New("not connected")

// BaseClient is a low layer MQTT client.
// Zero values with valid underlying Transport is a valid BaseClient.
type BaseClient struct {
	// Transport is an underlying connection. Typically net.Conn.
	Transport io.ReadWriteCloser
	// ConnState is called if the connection state is changed.
	ConnState func(ConnState, error)

	// MaxPayloadLen is a maximum allowed length of message payload.
	// 0 means unlimited. (It will panic if exceeds protocol maximum message length (256MB).)
	MaxPayloadLen int

	handler      Handler
	sig          *signaller
	mu           sync.RWMutex
	connState    ConnState
	err          error
	muErr        sync.RWMutex
	connClosed   chan struct{}
	muConnecting sync.RWMutex
	muWrite      sync.Mutex
	idLast       uint32

	muStats sync.RWMutex
	stats   BaseStats
}

// BaseStats stores base client statistics.
type BaseStats struct {
	// Recent ping delay.
	PingDelayRecent time.Duration
	// Maximum ping delay.
	PingDelayMax time.Duration
	// Minimum ping delay.
	PingDelayMin time.Duration
	// Count of ping error.
	CountPingError int
}

func (c *BaseClient) storePingDelay(d time.Duration) {
	c.muStats.Lock()
	c.stats.PingDelayRecent = d
	if c.stats.PingDelayMax < d {
		c.stats.PingDelayMax = d
	}
	if c.stats.PingDelayMin > d || c.stats.PingDelayMin == 0 {
		c.stats.PingDelayMin = d
	}
	c.muStats.Unlock()
}

func (c *BaseClient) storePingError() {
	c.muStats.Lock()
	c.stats.PingDelayRecent = 0
	c.stats.CountPingError++
	c.muStats.Unlock()
}

// Stats returns base client stats.
func (c *BaseClient) Stats() BaseStats {
	c.muStats.RLock()
	defer c.muStats.RUnlock()
	return c.stats
}

// Handle registers the message handler.
func (c *BaseClient) Handle(handler Handler) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.handler = handler
}

func (c *BaseClient) write(b []byte) error {
	l := len(b)
	c.muWrite.Lock()
	defer c.muWrite.Unlock()
	for i := 0; i < l; {
		n, err := c.Transport.Write(b[i : l-i])
		if err != nil {
			return err
		}
		i += n
	}
	return nil
}

func (c *BaseClient) signaller() (*signaller, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.sig == nil {
		return nil, ErrNotConnected
	}
	return c.sig, nil
}

type signaller struct {
	chConnAck  chan *pktConnAck
	chPingResp chan *pktPingResp

	// used is the outbound packet identifier namespace of one transport
	// generation. Identifier 0 is never registered.
	used map[uint16]*outboundOp

	mu sync.RWMutex
}

func (s *signaller) ConnAck() chan *pktConnAck {
	s.mu.RLock()
	defer s.mu.RUnlock()

	return s.chConnAck
}
func (s *signaller) PingResp() chan *pktPingResp {
	s.mu.RLock()
	defer s.mu.RUnlock()

	return s.chPingResp
}

// settlePubAck finalizes a PUBACK. The identifier is released only after the
// acknowledgement has been matched to the operation that still owns it.
func (s *signaller) settlePubAck(p *pktPubAck) {
	s.mu.Lock()
	o := s.used[p.ID]
	var ch chan *pktPubAck
	if o != nil {
		o.mu.Lock()
		if o.kind == outboundPublish && o.phase == phasePubAck && o.chPubAck != nil {
			ch = o.chPubAck
			o.phase = phaseDone
			o.released = true
			o.done = true
			o.reg = nil
			delete(s.used, p.ID)
		}
		o.mu.Unlock()
	}
	s.mu.Unlock()

	if ch != nil {
		select {
		case ch <- p:
		default:
		}
	}
}

// settlePubRec advances a QoS2 publish to the PUBREL/PUBCOMP phase. Ownership
// of the identifier is deliberately kept until PUBCOMP.
func (s *signaller) settlePubRec(p *pktPubRec) {
	s.mu.Lock()
	o := s.used[p.ID]
	var ch chan *pktPubRec
	if o != nil {
		o.mu.Lock()
		if o.kind == outboundPublish && o.phase == phasePubRec && o.chPubRec != nil {
			ch = o.chPubRec
			o.phase = phasePubComp
			o.pubRel = true
		}
		o.mu.Unlock()
	}
	s.mu.Unlock()

	if ch != nil {
		select {
		case ch <- p:
		default:
		}
	}
}

// settlePubComp finalizes a QoS2 publish.
func (s *signaller) settlePubComp(p *pktPubComp) {
	s.mu.Lock()
	o := s.used[p.ID]
	var ch chan *pktPubComp
	if o != nil {
		o.mu.Lock()
		if o.kind == outboundPublish && o.phase == phasePubComp && o.chPubComp != nil {
			ch = o.chPubComp
			o.phase = phaseDone
			o.released = true
			o.done = true
			o.reg = nil
			delete(s.used, p.ID)
		}
		o.mu.Unlock()
	}
	s.mu.Unlock()

	if ch != nil {
		select {
		case ch <- p:
		default:
		}
	}
}

// settleSubAck finalizes a SUBSCRIBE.
func (s *signaller) settleSubAck(p *pktSubAck) {
	s.mu.Lock()
	o := s.used[p.ID]
	var ch chan *pktSubAck
	if o != nil {
		o.mu.Lock()
		if o.kind == outboundSubscribe && o.phase == phaseSubAck && o.chSubAck != nil {
			ch = o.chSubAck
			o.phase = phaseDone
			o.released = true
			o.done = true
			o.reg = nil
			delete(s.used, p.ID)
		}
		o.mu.Unlock()
	}
	s.mu.Unlock()

	if ch != nil {
		select {
		case ch <- p:
		default:
		}
	}
}

// settleUnsubAck finalizes an UNSUBSCRIBE.
func (s *signaller) settleUnsubAck(p *pktUnsubAck) {
	s.mu.Lock()
	o := s.used[p.ID]
	var ch chan *pktUnsubAck
	if o != nil {
		o.mu.Lock()
		if o.kind == outboundUnsubscribe && o.phase == phaseUnsubAck && o.chUnsubAck != nil {
			ch = o.chUnsubAck
			o.phase = phaseDone
			o.released = true
			o.done = true
			o.reg = nil
			delete(s.used, p.ID)
		}
		o.mu.Unlock()
	}
	s.mu.Unlock()

	if ch != nil {
		select {
		case ch <- p:
		default:
		}
	}
}
