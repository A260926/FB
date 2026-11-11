package wav

const (
	AudioFormatPCM       = 1
	AudioFormatIEEEFloat = 3
	AudioFormatALaw      = 6
	AudioFormatMULaw     = 7
)

type WavFormat struct {
	AudioFormat   uint16
	NumChannels   uint16
	SampleRate    uint32
	ByteRate      uint32
	BlockAlign    uint16
	BitsPerSample uint16
}

type WavData struct {
	stream *audioStream
	Size   uint32
	pos    uint32
}

func (d *WavData) Read(p []byte) (n int, err error) {
	n, err = d.stream.Read(p)
	d.pos += uint32(n)

	return
}

type Sample struct {
	Values [2]int
}
