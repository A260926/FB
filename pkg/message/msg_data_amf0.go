package message

import (
	"github.com/bluenviron/gortmplib/pkg/amf0"
	"github.com/bluenviron/gortmplib/pkg/rawmessage"
)

// DataAMF0 is a AMF0 data message.
type DataAMF0 struct {
	ChunkStreamID   byte
	MessageStreamID uint32
	Payload         amf0.Data
}

func (m *DataAMF0) unmarshal(raw *rawmessage.Message) error {
	m.ChunkStreamID = raw.ChunkStreamID
	m.MessageStreamID = raw.MessageStreamID

	var err error
	m.Payload, err = amf0.Unmarshal(raw.Body)
	if err != nil {
		return err
	}

	return nil
}

func (m DataAMF0) marshal() (*rawmessage.Message, error) {
	body, err := m.Payload.Marshal()
	if err != nil {
		return nil, err
	}

	return &rawmessage.Message{
		ChunkStreamID:   m.ChunkStreamID,
		Type:            uint8(TypeDataAMF0),
		MessageStreamID: m.MessageStreamID,
		Body:            body,
	}, nil
}
