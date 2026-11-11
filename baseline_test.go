package ftp
import (
	"fmt"
	"net"
	"net/textproto"
	"testing"
	"time"
)
func TestWalkerStopBaseline(t *testing.T) {
	a, b := net.Pipe()
	defer a.Close()
	defer b.Close()
	a.SetDeadline(time.Now().Add(3 * time.Second))
	go func() {
		p := textproto.NewConn(b)
		defer p.Close()
		for {
			s, e := p.ReadLine()
			if e != nil { return }
			switch s {
			case "EPSV":
				p.PrintfLine("229 Entering Extended Passive Mode (|||1234|)")
			case "LIST /denied":
				p.PrintfLine("550 directory unavailable")
			default:
				p.PrintfLine("500 unexpected")
			}
		}
	}()
	c := &ServerConn{conn: textproto.NewConn(a), netConn: a, host: "127.0.0.1", options: &dialOptions{dialFunc: func(string, string) (net.Conn, error) { x, y := net.Pipe(); y.Close(); return x, nil }}}
	w := &Walker{serverConn: c, root: "/", cur: &item{path: "/denied", entry: &Entry{Type: EntryTypeFolder}}, descend: true, stack: []*item{{path: "/good.txt", entry: &Entry{Name: "good.txt", Type: EntryTypeFile}}}}
	if w.Next() {
		t.Fatal("unexpected traversal after denial")
	}
	e, ok := w.Err().(*textproto.Error)
	if !ok || e.Code != 550 {
		t.Fatalf("error %v", w.Err())
	}
	if len(w.stack) != 1 {
		t.Fatal("sibling state lost")
	}
	t.Log(fmt.Sprintf("BASELINE_FTP_DENIED_STOPS_WITH_SIBLING=%d", len(w.stack)))
}
