// Package rawmessage contains a RTMP raw message reader/writer.
package rawmessage

import (
	"time"
)

// Message is a raw message.
type Message struct {
	// ChunkStreamID is the chunk stream ID. Valid values range from 2 to 65599.
	ChunkStreamID   uint32
	Timestamp       time.Duration
	Type            uint8
	MessageStreamID uint32
	Body            []byte
}

func (m *Message) clone() *Message {
	return &Message{
		ChunkStreamID:   m.ChunkStreamID,
		Timestamp:       m.Timestamp,
		Type:            m.Type,
		MessageStreamID: m.MessageStreamID,
		Body:            m.Body,
	}
}
