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
	"io"
)

func readPacket(r io.Reader) (packetType, byte, []byte, error) {
	buf := make([]byte, 2)
	if _, err := io.ReadFull(r, buf); err != nil {
		return 0, 0, nil, err
	}
	pktType := packetType(buf[0] & 0xF0)
	pktFlag := buf[0] & 0x0F
	var remainingLength int
	for shift := uint(0); ; shift += 7 {
		remainingLength |= (int(buf[1]) & 0x7F) << shift
		if buf[1]&0x80 == 0 {
			break
		}
		if _, err := io.ReadFull(r, buf[1:]); err != nil {
			return 0, 0, nil, err
		}
	}
	contents := make([]byte, remainingLength)
	if _, err := io.ReadFull(r, contents); err != nil {
		return 0, 0, nil, err
	}
	return pktType, pktFlag, contents, nil
}

func (c *BaseClient) serve() error {
	c.mu.Lock()
	sig := c.sig
	c.mu.Unlock()
	return c.serveGeneration(sig, c.Transport)
}

// serveGeneration runs the receive loop of one transport generation. The
// generation's signaller and transport are passed explicitly so that a late
// cleanup of an old receive loop can never touch the resources of a newer
// connection.
func (c *BaseClient) serveGeneration(sig *signaller, transport io.ReadWriteCloser) error {
	r := transport
	if sig == nil {
		// Used before Connect; there is no outbound namespace to manage.
		sig = &signaller{}
	}
	// Leases of this transport generation are only discarded when the
	// receive loop terminates.
	defer cleanupOutbound(sig)

	subBuffer := make(map[uint16]*Message)
	for {
		pktType, pktFlag, contents, err := readPacket(r)
		if err != nil {
			return err
		}
		// fmt.Printf("%s: %v\n", pktType, contents)

		switch pktType {
		case packetConnAck:
			connAck, err := (&pktConnAck{}).Parse(pktFlag, contents)
			if err != nil {
				// Client must close connection if packet is invalid.
				return err
			}
			select {
			case sig.ConnAck() <- connAck:
			default:
			}
		case packetPublish:
			publish, err := (&pktPublish{}).Parse(pktFlag, contents)
			if err != nil {
				return err
			}
			switch publish.Message.QoS {
			case QoS0:
				c.mu.RLock()
				handler := c.handler
				c.mu.RUnlock()
				if handler != nil {
					handler.Serve(publish.Message)
				}
			case QoS1:
				// Ownership of the message is now transferred to the receiver.
				c.mu.RLock()
				handler := c.handler
				c.mu.RUnlock()
				if handler != nil {
					handler.Serve(publish.Message)
				}
				pktPubAck := (&pktPubAck{ID: publish.Message.ID}).Pack()
				if err := c.write(pktPubAck); err != nil {
					return wrapError(err, "sending PUBACK")
				}
			case QoS2:
				pktPubRec := (&pktPubRec{ID: publish.Message.ID}).Pack()
				if err := c.write(pktPubRec); err != nil {
					return wrapError(err, "sending PUBREC")
				}
				subBuffer[publish.Message.ID] = publish.Message
			}
		case packetPubAck:
			pubAck, err := (&pktPubAck{}).Parse(pktFlag, contents)
			if err != nil {
				return err
			}
			sig.settlePubAck(pubAck)
		case packetPubRec:
			pubRec, err := (&pktPubRec{}).Parse(pktFlag, contents)
			if err != nil {
				return err
			}
			sig.settlePubRec(pubRec)
		case packetPubRel:
			pubRel, err := (&pktPubRel{}).Parse(pktFlag, contents)
			if err != nil {
				return err
			}
			if msg, ok := subBuffer[pubRel.ID]; ok {
				// Ownership of the message is now transferred to the receiver.
				c.mu.RLock()
				handler := c.handler
				c.mu.RUnlock()
				if handler != nil {
					handler.Serve(msg)
				}
				delete(subBuffer, pubRel.ID)

				pktPubComp := (&pktPubComp{ID: pubRel.ID}).Pack()
				if err := c.write(pktPubComp); err != nil {
					return wrapError(err, "sending PUBCOMP")
				}
			}
		case packetPubComp:
			pubComp, err := (&pktPubComp{}).Parse(pktFlag, contents)
			if err != nil {
				return err
			}
			sig.settlePubComp(pubComp)
		case packetSubAck:
			subAck, err := (&pktSubAck{}).Parse(pktFlag, contents)
			if err != nil {
				return err
			}
			sig.settleSubAck(subAck)
		case packetUnsubAck:
			unsubAck, err := (&pktUnsubAck{}).Parse(pktFlag, contents)
			if err != nil {
				return err
			}
			sig.settleUnsubAck(unsubAck)
		case packetPingResp:
			pingResp, err := (&pktPingResp{}).Parse(pktFlag, contents)
			if err != nil {
				return err
			}
			select {
			case sig.PingResp() <- pingResp:
			default:
			}
		default:
			// must close connection if the client encountered protocol violation.
			return wrapErrorf(ErrInvalidPacket, "serving incoming packet %x", int(pktType))
		}
	}
}
