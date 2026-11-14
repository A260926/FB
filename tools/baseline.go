package main

import (
	"bytes"
	"fmt"
	"github.com/bluenviron/gortmplib/pkg/rawmessage"
	"strings"
)

func read(b []byte) (*rawmessage.Message, error) {
	r := &rawmessage.Reader{BR: bytes.NewReader(b)}
	r.Initialize()
	return r.Read()
}
func main() {
	// Same type-0 header and one-byte audio body, with two distinct basic headers.
	h := []byte{0, 0, 7, 0, 0, 1, 8, 1, 0, 0, 0, 0xaf}
	ordinary := append([]byte{5}, h...)
	m, e := read(ordinary)
	if e != nil || m.ChunkStreamID != 5 || !bytes.Equal(m.Body, []byte{0xaf}) {
		panic("ordinary message failed")
	}
	fmt.Println("BASELINE_RTMP_SHORT_ID_OK")
	extended := append([]byte{0, 0}, h...)
	_, e = read(extended)
	if e == nil || !strings.Contains(e.Error(), "extended chunk stream IDs") {
		panic("unexpected extended-ID behavior")
	}
	fmt.Println("BASELINE_RTMP_EXTENDED_ID_UNSUPPORTED")
}
