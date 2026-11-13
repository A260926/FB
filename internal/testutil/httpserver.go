package testutil

import (
	"context"
	"io"
	"net"
	"net/http"
	"sync"
	"testing"
)

// PipeListener is a net.Listener whose connections never touch the network:
// DialContext creates an in-memory net.Pipe and hand it to Accept. Enabling it
// lets tests run a genuine http.Server and http.Client in-process without
// binding a port, which sandboxed environments may forbid.
type PipeListener struct {
	addr  pipeAddr
	conns chan net.Conn
	done  chan struct{}
	once  sync.Once
}

type pipeAddr string

func (a pipeAddr) Network() string { return "pipe" }
func (a pipeAddr) String() string  { return string(a) }

// NewPipeListener returns a listener reporting addr as its address.
func NewPipeListener(addr string) *PipeListener {
	return &PipeListener{
		addr:  pipeAddr(addr),
		conns: make(chan net.Conn, 64),
		done:  make(chan struct{}),
	}
}

// Accept returns the next dialed connection.
func (l *PipeListener) Accept() (net.Conn, error) {
	select {
	case c := <-l.conns:
		return c, nil
	case <-l.done:
		return nil, net.ErrClosed
	}
}

// Close stops the listener, unblocking Accept and DialContext.
func (l *PipeListener) Close() error {
	l.once.Do(func() { close(l.done) })
	return nil
}

// Addr returns the listener address.
func (l *PipeListener) Addr() net.Addr { return l.addr }

// DialContext connects to this listener regardless of the requested address,
// so any number of logical hosts can be served by one in-memory server.
func (l *PipeListener) DialContext(ctx context.Context, _, _ string) (net.Conn, error) {
	server, client := net.Pipe()
	select {
	case l.conns <- server:
		return client, nil
	case <-l.done:
		server.Close()
		return nil, net.ErrClosed
	case <-ctx.Done():
		server.Close()
		return nil, ctx.Err()
	}
}

// MemoryServer is an in-process HTTP server. Multiple host names can be used
// against it (see URL); the handler sees them as distinct Host values, so a
// test can model several independent sites sharing one transport.
type MemoryServer struct {
	Listener *PipeListener
	Client   *http.Client
	server   *http.Server
}

// NewMemoryServer starts handler on an in-memory listener and returns a client
// wired to it. Cleanup closes both.
func NewMemoryServer(t testing.TB, handler http.Handler) *MemoryServer {
	t.Helper()
	l := NewPipeListener("memory")
	srv := &http.Server{Handler: handler}
	transport := &http.Transport{DialContext: l.DialContext}
	ms := &MemoryServer{
		Listener: l,
		Client:   &http.Client{Transport: transport},
		server:   srv,
	}
	go func() {
		_ = srv.Serve(l) // returns net.ErrClosed on cleanup
	}()
	t.Cleanup(func() {
		_ = srv.Close()
		_ = l.Close()
		transport.CloseIdleConnections()
	})
	return ms
}

// URL returns an absolute URL for host (which may be any logical site name)
// and path.
func (ms *MemoryServer) URL(host, path string) string {
	return "http://" + host + path
}

// PanicReader fails the test if it is read at all. It proves that a call
// rejected its arguments before touching the input.
type PanicReader struct {
	TB testing.TB
}

func (r PanicReader) Read([]byte) (int, error) {
	r.TB.Helper()
	r.TB.Fatalf("input was read before the call validated its arguments")
	return 0, io.EOF
}
