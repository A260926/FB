package vnc_test

import (
	"log"
	"net"

	vnc "github.com/mitchellh/go-vnc"
)

// Driving a desktop form over VNC: update the remote clipboard and then
// "paste" into the focused field with Ctrl+V. With ExtendedClipboard
// enabled the clipboard text is negotiated as UTF-8, so non-Latin-1 text
// (such as Chinese) can be entered.
func Example_extendedClipboardPaste() {
	conn, err := net.Dial("tcp", "127.0.0.1:5900")
	if err != nil {
		log.Fatal(err)
	}

	cc, err := vnc.Client(conn, &vnc.ClientConfig{ExtendedClipboard: true})
	if err != nil {
		log.Fatal(err)
	}
	defer cc.Close()

	// Update the remote clipboard. Once the extended clipboard is
	// negotiated this announces ownership and provides the text when the
	// server requests it.
	if err := cc.CutText("你好，VNC"); err != nil {
		log.Fatal(err)
	}

	// Send Ctrl+V to paste the clipboard into the focused field.
	const (
		keyControlLeft = 0xffe3 // XK_Control_L
		keyV           = 0x0076 // XK_v
	)
	for _, ev := range []struct {
		key  uint32
		down bool
	}{
		{keyControlLeft, true},
		{keyV, true},
		{keyV, false},
		{keyControlLeft, false},
	} {
		if err := cc.KeyEvent(ev.key, ev.down); err != nil {
			log.Fatal(err)
		}
	}
}
