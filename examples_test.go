package wav

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"log"
	"os"

	"github.com/go-audio/audio"
	"github.com/go-audio/riff"
)

// playlistExampleWAV builds a 16-bit mono WAV whose data holds four
// frames (10, 20, 30, 40) plus a cue chunk and a playlist chunk. The
// playlist plays frames [2,4) twice, then frame 0 once.
func playlistExampleWAV() []byte {
	chunk := func(id [4]byte, payload []byte) []byte {
		out := append([]byte{}, id[:]...)
		var size [4]byte
		binary.LittleEndian.PutUint32(size[:], uint32(len(payload)))
		out = append(out, size[:]...)
		out = append(out, payload...)
		return out
	}

	fmtPayload := make([]byte, 16)
	binary.LittleEndian.PutUint16(fmtPayload[0:], 1) // PCM
	binary.LittleEndian.PutUint16(fmtPayload[2:], 1) // mono
	binary.LittleEndian.PutUint32(fmtPayload[4:], 48000)
	binary.LittleEndian.PutUint32(fmtPayload[8:], 96000)
	binary.LittleEndian.PutUint16(fmtPayload[12:], 2)
	binary.LittleEndian.PutUint16(fmtPayload[14:], 16)

	dataPayload := make([]byte, 8)
	for i, s := range []int16{10, 20, 30, 40} {
		binary.LittleEndian.PutUint16(dataPayload[i*2:], uint16(s))
	}

	cuePayload := make([]byte, 4+2*24)
	binary.LittleEndian.PutUint32(cuePayload[0:], 2)
	cue := func(offset int, id, sampleOffset uint32) {
		binary.LittleEndian.PutUint32(cuePayload[offset:], id)
		copy(cuePayload[offset+8:offset+12], riff.DataFormatID[:])
		binary.LittleEndian.PutUint32(cuePayload[offset+20:], sampleOffset)
	}
	cue(4, 1, 2)
	cue(28, 2, 0)

	plstPayload := make([]byte, 4+2*12)
	binary.LittleEndian.PutUint32(plstPayload[0:], 2)
	seg := func(offset int, id, length, count uint32) {
		binary.LittleEndian.PutUint32(plstPayload[offset:], id)
		binary.LittleEndian.PutUint32(plstPayload[offset+4:], length)
		binary.LittleEndian.PutUint32(plstPayload[offset+8:], count)
	}
	seg(4, 1, 2, 2)
	seg(16, 2, 1, 1)

	body := append(chunk(riff.FmtID, fmtPayload), chunk(riff.DataFormatID, dataPayload)...)
	body = append(body, chunk(CIDCue, cuePayload)...)
	body = append(body, chunk(cidPlaylist, plstPayload)...)

	out := []byte{'R', 'I', 'F', 'F'}
	var size [4]byte
	binary.LittleEndian.PutUint32(size[:], uint32(4+len(body)))
	out = append(out, size[:]...)
	out = append(out, 'W', 'A', 'V', 'E')
	return append(out, body...)
}

func ExampleDecoder_UsePlaylist() {
	d := NewDecoder(bytes.NewReader(playlistExampleWAV()))
	// Opt in before the first read; the playlist and every referenced
	// range are validated before the first sample is returned.
	d.UsePlaylist = true

	buf := &audio.IntBuffer{Data: make([]int, 16)}
	var out []int
	for {
		n, err := d.PCMBuffer(buf)
		if err != nil {
			log.Fatal(err)
		}
		if n == 0 {
			break
		}
		out = append(out, buf.Data[:n]...)
	}
	fmt.Println(out)
	// Output: [30 40 30 40 10]
}

func ExampleDecoder_SeekPlaylistFrame() {
	d := NewDecoder(bytes.NewReader(playlistExampleWAV()))
	d.UsePlaylist = true

	// Jump straight to logical frame 3, inside the second repeat.
	if err := d.SeekPlaylistFrame(3); err != nil {
		log.Fatal(err)
	}
	buf := &audio.IntBuffer{Data: make([]int, 16)}
	n, err := d.PCMBuffer(buf)
	if err != nil {
		log.Fatal(err)
	}
	fmt.Println(buf.Data[:n])
	// Output: [40 10]
}

func ExampleDecoder_Duration() {
	f, err := os.Open("fixtures/kick.wav")
	if err != nil {
		log.Fatal(err)
	}
	defer f.Close()
	dur, err := NewDecoder(f).Duration()
	if err != nil {
		log.Fatal(err)
	}
	fmt.Printf("%s duration: %s\n", f.Name(), dur)
	// Output: fixtures/kick.wav duration: 204.172335ms
}

func ExampleDecoder_IsValidFile() {
	f, err := os.Open("fixtures/kick.wav")
	if err != nil {
		log.Fatal(err)
	}
	defer f.Close()
	fmt.Printf("is this file valid: %t", NewDecoder(f).IsValidFile())
	// Output: is this file valid: true
}

func ExampleEncoder_Write() {
	f, err := os.Open("fixtures/kick.wav")
	if err != nil {
		panic(fmt.Sprintf("couldn't open audio file - %v", err))
	}

	// Decode the original audio file
	// and collect audio content and information.
	d := NewDecoder(f)
	buf, err := d.FullPCMBuffer()
	if err != nil {
		panic(err)
	}
	f.Close()
	fmt.Println("Old file ->", d)

	// Destination file
	out, err := os.Create("testOutput/kick.wav")
	if err != nil {
		panic(fmt.Sprintf("couldn't create output file - %v", err))
	}

	// setup the encoder and write all the frames
	e := NewEncoder(out,
		buf.Format.SampleRate,
		int(d.BitDepth),
		buf.Format.NumChannels,
		int(d.WavAudioFormat))
	if err = e.Write(buf); err != nil {
		panic(err)
	}
	// close the encoder to make sure the headers are properly
	// set and the data is flushed.
	if err = e.Close(); err != nil {
		panic(err)
	}
	out.Close()

	// reopen to confirm things worked well
	out, err = os.Open("testOutput/kick.wav")
	if err != nil {
		panic(err)
	}
	d2 := NewDecoder(out)
	d2.ReadInfo()
	fmt.Println("New file ->", d2)
	out.Close()
	os.Remove(out.Name())

	// Output:
	// Old file -> Format: WAVE - 1 channels @ 22050 / 16 bits - Duration: 0.204172 seconds
	// New file -> Format: WAVE - 1 channels @ 22050 / 16 bits - Duration: 0.204172 seconds
}

func ExampleDecoder_ReadMetadata() {
	f, err := os.Open("fixtures/listinfo.wav")
	if err != nil {
		log.Fatal(err)
	}
	defer f.Close()
	d := NewDecoder(f)
	d.ReadMetadata()
	if d.Err() != nil {
		log.Fatal(err)
	}
	fmt.Printf("%#v\n", d.Metadata)
	// Output:
	// &wav.Metadata{SamplerInfo:(*wav.SamplerInfo)(nil), Artist:"artist", Comments:"my comment", Copyright:"", CreationDate:"2017", Engineer:"", Technician:"", Genre:"genre", Keywords:"", Medium:"", Title:"track title", Product:"album title", Subject:"", Software:"", Source:"", Location:"", TrackNbr:"42", CuePoints:[]*wav.CuePoint(nil)}
}
