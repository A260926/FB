// Copyright 2015 Muir Manders.  All rights reserved.
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file.

package goftp

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"net"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// loopFTPServer is a small, deliberately limited FTP peer implemented with
// only the standard library. It is enough to exercise the resource
// lifecycle of prepared passive sockets and active-mode listeners:
//
//   - paths containing "denied" get a complete 550 rejection before any
//     125/150 reply;
//   - "/nodial" (active mode) gets a 150 but the server never connects back,
//     so the client's Accept fails;
//   - "/stall" gets a 150, partial data, then a final reply delayed past the
//     client timeout, simulating a lost reply boundary;
//   - everything else completes a normal 150/data/226 transfer.
//
// It records every control command, every data port it observes, every data
// connection it accepts/dials, every active PORT address and every STOR
// payload, so tests can assert on real resource state rather than pool
// counters alone.
type loopFTPServer struct {
	listener net.Listener
	active   bool

	stallDelay time.Duration

	mu              sync.Mutex
	nextSessionID   int
	liveSessions    int
	maxLiveSessions int
	totalSessions   int
	commands        []string
	dataPorts       []string
	dataConns       chan net.Conn
	activeAddrs     chan string
	storedData      chan string
}

const mlsdListing = "type=file;size=3;modify=20200101000000;unix.mode=0644; a.txt\r\n" +
	"type=file;size=4;modify=20200102000000;unix.mode=0644; b.txt\r\n"

func startLoopFTPServer(t *testing.T, active bool) *loopFTPServer {
	t.Helper()

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %s", err)
	}

	srv := &loopFTPServer{
		listener:    ln,
		active:      active,
		stallDelay:  time.Second,
		dataConns:   make(chan net.Conn, 32),
		activeAddrs: make(chan string, 32),
		storedData:  make(chan string, 32),
	}

	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go srv.serve(conn)
		}
	}()

	t.Cleanup(func() { ln.Close() })
	return srv
}

func (s *loopFTPServer) addr() string {
	return s.listener.Addr().String()
}

func (s *loopFTPServer) record(id int, line string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.commands = append(s.commands, fmt.Sprintf("#%d %s", id, line))
}

func (s *loopFTPServer) snapshot() (total, maxLive int, cmds []string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.totalSessions, s.maxLiveSessions, append([]string(nil), s.commands...)
}

// waitForSettled blocks until exactly wantTotal sessions have been seen and
// only one remains alive. A server-side goroutine notices the client closing
// an evicted control connection asynchronously, so concurrency assertions on
// the server side must wait for the old session to wind down; the hard cap
// itself is asserted on the client via numOpenConns.
func (s *loopFTPServer) waitForSettled(t *testing.T, wantTotal int) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		s.mu.Lock()
		total, live := s.totalSessions, s.liveSessions
		s.mu.Unlock()
		if total == wantTotal && live == 1 {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("server sessions never settled: %+v", s)
}

func (s *loopFTPServer) serve(conn net.Conn) {
	s.mu.Lock()
	s.nextSessionID++
	id := s.nextSessionID
	s.totalSessions++
	s.liveSessions++
	if s.liveSessions > s.maxLiveSessions {
		s.maxLiveSessions = s.liveSessions
	}
	s.mu.Unlock()

	defer func() {
		conn.Close()
		s.mu.Lock()
		s.liveSessions--
		s.mu.Unlock()
	}()

	fmt.Fprint(conn, "220 ready\r\n")

	sc := bufio.NewScanner(conn)
	var passiveConn chan net.Conn
	var activeAddr string

	for sc.Scan() {
		line := sc.Text()
		s.record(id, line)

		cmd := strings.SplitN(line, " ", 2)[0]
		switch cmd {
		case "USER":
			fmt.Fprint(conn, "230 ok\r\n")
		case "FEAT":
			fmt.Fprint(conn, "211-Features\r\n UTF8\r\n211 End\r\n")
		case "TYPE", "SYST", "NOOP":
			fmt.Fprint(conn, "200 ok\r\n")
		case "QUIT":
			fmt.Fprint(conn, "221 bye\r\n")
			return
		case "EPSV":
			ln, err := net.Listen("tcp", "127.0.0.1:0")
			if err != nil {
				fmt.Fprint(conn, "425 cannot open data connection\r\n")
				continue
			}
			port := ln.Addr().(*net.TCPAddr).Port
			s.mu.Lock()
			s.dataPorts = append(s.dataPorts, strconv.Itoa(port))
			s.mu.Unlock()

			passiveConn = make(chan net.Conn, 1)
			go func() {
				dc, err := ln.Accept()
				ln.Close()
				if err != nil {
					return
				}
				s.dataConns <- dc
				passiveConn <- dc
			}()

			fmt.Fprintf(conn, "229 entering passive mode (|||%d|)\r\n", port)
		case "PORT":
			parts := strings.Split(strings.TrimPrefix(line, "PORT "), ",")
			if len(parts) != 6 {
				fmt.Fprint(conn, "501 bad PORT\r\n")
				continue
			}
			p1, _ := strconv.Atoi(parts[4])
			p2, _ := strconv.Atoi(parts[5])
			activeAddr = net.JoinHostPort(strings.Join(parts[0:4], "."), strconv.Itoa(p1<<8+p2))
			s.activeAddrs <- activeAddr
			s.mu.Lock()
			s.dataPorts = append(s.dataPorts, activeAddr)
			s.mu.Unlock()
			fmt.Fprint(conn, "200 ok\r\n")
		case "EPRT":
			parts := strings.Split(line, "|")
			if len(parts) != 5 {
				fmt.Fprint(conn, "501 bad EPRT\r\n")
				continue
			}
			activeAddr = net.JoinHostPort(parts[2], parts[3])
			s.activeAddrs <- activeAddr
			fmt.Fprint(conn, "200 ok\r\n")
		case "RETR", "STOR", "MLSD", "LIST":
			if done := s.handleTransfer(conn, line, cmd, passiveConn, &activeAddr); done {
				return
			}
		default:
			fmt.Fprint(conn, "200 ok\r\n")
		}
	}
}

// handleTransfer returns true if the control session should terminate.
func (s *loopFTPServer) handleTransfer(control net.Conn, line, cmd string, passiveConn chan net.Conn, activeAddr *string) bool {
	arg := ""
	if parts := strings.SplitN(line, " ", 2); len(parts) == 2 {
		arg = parts[1]
	}

	// Complete rejection before the 125/150 boundary: the control connection
	// remains in sync and reusable.
	if strings.Contains(arg, "denied") {
		fmt.Fprint(control, "550 refused\r\n")
		return false
	}

	fmt.Fprint(control, "150 starting transfer\r\n")

	// Active mode without a dial-back: the client's listener Accept must fail.
	if strings.Contains(arg, "nodial") {
		return false
	}

	var dc net.Conn
	if s.active {
		if *activeAddr == "" {
			fmt.Fprint(control, "425 no active address\r\n")
			return false
		}
		var err error
		dc, err = net.DialTimeout("tcp", *activeAddr, 2*time.Second)
		if err != nil {
			return false
		}
	} else {
		dc = <-passiveConn
	}
	defer dc.Close()

	if strings.Contains(arg, "stall") {
		// Hand over a truncated stream and delay the final reply past the
		// client's deadline: the reply boundary cannot be recovered by
		// guessing, so the client must discard this control connection.
		fmt.Fprint(dc, "partial")
		dc.Close()
		time.Sleep(s.stallDelay)
		fmt.Fprint(control, "226 late completion\r\n")
		return false
	}

	switch cmd {
	case "RETR":
		if strings.Contains(arg, "aborted") {
			fmt.Fprint(dc, "x")
		} else {
			fmt.Fprint(dc, "ok-data")
		}
	case "MLSD", "LIST":
		fmt.Fprint(dc, mlsdListing)
	case "STOR":
		got, err := io.ReadAll(dc)
		if err == nil {
			s.storedData <- string(got)
		}
	}
	dc.Close()
	if strings.Contains(arg, "aborted") {
		// A completed-but-unsuccessful final reply after 150: the client must
		// not reuse the control connection.
		fmt.Fprint(control, "426 connection closed, transfer aborted\r\n")
	} else {
		fmt.Fprint(control, "226 done\r\n")
	}
	return false
}

func newLeaseTestClient(t *testing.T, addr string, active bool, timeout time.Duration) *Client {
	t.Helper()

	config := Config{
		User:               "anonymous",
		Password:           "anonymous",
		ConnectionsPerHost: 1,
		Timeout:            timeout,
	}
	if active {
		config.ActiveTransfers = true
		config.ActiveListenAddr = "127.0.0.1:0"
	}

	c, err := DialConfig(config, addr)
	if err != nil {
		t.Fatalf("dial: %s", err)
	}
	t.Cleanup(func() { c.Close() })
	return c
}

// assertEOFSoon fails the test if a read from conn does not surface a clean
// EOF promptly, i.e. the peer did not close the socket.
func assertEOFSoon(t *testing.T, conn net.Conn) {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	var buf [1]byte
	n, err := conn.Read(buf[:])
	if n != 0 || err != io.EOF {
		t.Fatalf("expected EOF on released data connection, got n=%d err=%v", n, err)
	}
}

// assertListenerClosed fails if a port still accepts connections.
func assertListenerClosed(t *testing.T, addr string) {
	t.Helper()
	conn, err := net.DialTimeout("tcp", addr, time.Second)
	if err == nil {
		conn.Close()
		t.Fatalf("active listener at %s is still accepting connections", addr)
	}
}

func TestPreparedPassive(t *testing.T) {
	t.Run("rejected_retr_releases_socket_and_control_conn_reused", func(t *testing.T) {
		srv := startLoopFTPServer(t, false)
		c := newLeaseTestClient(t, srv.addr(), false, 2*time.Second)

		var buf bytes.Buffer
		err := c.Retrieve("/denied", &buf)
		fe, ok := err.(Error)
		if !ok || fe.Code() != 550 {
			t.Fatalf("expected 550 error, got %v", err)
		}

		// The server accepted the dialed data socket; the client must have
		// closed it after the 550 rather than leaving it for Client.Close.
		assertEOFSoon(t, <-srv.dataConns)

		buf.Reset()
		if err := c.Retrieve("/ok", &buf); err != nil {
			t.Fatalf("follow-up retrieve: %s", err)
		}
		if buf.String() != "ok-data" {
			t.Fatalf("successful transfer truncated: %q", buf.String())
		}

		total, _, _ := srv.snapshot()
		if total != 1 {
			t.Fatalf("rejection must not cost the control connection, got %d sessions", total)
		}
	})

	t.Run("rejected_mlsd_and_store_released_then_content_reads_succeed", func(t *testing.T) {
		srv := startLoopFTPServer(t, false)
		c := newLeaseTestClient(t, srv.addr(), false, 2*time.Second)

		if _, err := c.ReadDir("/denied"); err == nil {
			t.Fatal("expected MLSD rejection error")
		} else if fe, ok := err.(Error); !ok || fe.Code() != 550 {
			t.Fatalf("expected 550, got %v", err)
		}
		assertEOFSoon(t, <-srv.dataConns)

		entries, err := c.ReadDir("/dir")
		if err != nil {
			t.Fatalf("ReadDir: %s", err)
		}
		if len(entries) != 2 || entries[0].Name() != "a.txt" || entries[1].Name() != "b.txt" {
			t.Fatalf("unexpected directory contents: %+v", entries)
		}

		if err := c.Store("/up", strings.NewReader("upload-bytes")); err != nil {
			t.Fatalf("Store: %s", err)
		}
		if got := <-srv.storedData; got != "upload-bytes" {
			t.Fatalf("server received %q", got)
		}

		if err := c.Store("/denied", strings.NewReader("nope")); err == nil {
			t.Fatal("expected STOR rejection error")
		} else if fe, ok := err.(Error); !ok || fe.Code() != 550 {
			t.Fatalf("expected 550, got %v", err)
		}
		assertEOFSoon(t, <-srv.dataConns)

		total, _, _ := srv.snapshot()
		if total != 1 {
			t.Fatalf("control connection should survive rejections, got %d sessions", total)
		}
	})

	t.Run("raw_conn_closed_without_calling_getter_releases_socket", func(t *testing.T) {
		srv := startLoopFTPServer(t, false)
		c := newLeaseTestClient(t, srv.addr(), false, 2*time.Second)

		raw, err := c.OpenRawConn()
		if err != nil {
			t.Fatal(err)
		}
		if _, err := raw.PrepareDataConn(); err != nil {
			t.Fatal(err)
		}
		// Deliberately never invoke getter; closing the RawConn must release
		// the prepared passive socket.
		dataConn := <-srv.dataConns
		dataConn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
		var buf [1]byte
		if _, err := dataConn.Read(buf[:]); err == nil {
			t.Fatal("data connection unexpectedly had data")
		} else if ne, ok := err.(net.Error); !ok || !ne.Timeout() {
			t.Fatalf("expected pre-close read timeout, got %v", err)
		}

		if err := raw.Close(); err != nil {
			t.Fatal(err)
		}
		assertEOFSoon(t, dataConn)

		// Idempotent safety: a second release through Close must not touch any
		// other connection; the pooled client still works.
		var out bytes.Buffer
		if err := c.Retrieve("/ok", &out); err != nil {
			t.Fatalf("pooled transfer after raw close: %s", err)
		}
		if out.String() != "ok-data" {
			t.Fatalf("transfer after raw close truncated: %q", out.String())
		}
	})
}

func TestPreparedActive(t *testing.T) {
	t.Run("rejected_retr_closes_listener_and_control_conn_reused", func(t *testing.T) {
		srv := startLoopFTPServer(t, true)
		c := newLeaseTestClient(t, srv.addr(), true, 2*time.Second)

		var buf bytes.Buffer
		if err := c.Retrieve("/denied", &buf); err == nil {
			t.Fatal("expected rejection error")
		} else if fe, ok := err.(Error); !ok || fe.Code() != 550 {
			t.Fatalf("expected 550, got %v", err)
		}

		assertListenerClosed(t, <-srv.activeAddrs)

		buf.Reset()
		if err := c.Retrieve("/ok", &buf); err != nil {
			t.Fatalf("follow-up retrieve: %s", err)
		}
		if buf.String() != "ok-data" {
			t.Fatalf("active transfer truncated: %q", buf.String())
		}

		total, _, _ := srv.snapshot()
		if total != 1 {
			t.Fatalf("expected one reused control session, got %d", total)
		}
	})

	t.Run("rejected_mlsd_closes_listener_then_listing_succeeds", func(t *testing.T) {
		srv := startLoopFTPServer(t, true)
		c := newLeaseTestClient(t, srv.addr(), true, 2*time.Second)

		if _, err := c.ReadDir("/denied"); err == nil {
			t.Fatal("expected MLSD rejection")
		} else if fe, ok := err.(Error); !ok || fe.Code() != 550 {
			t.Fatalf("expected 550, got %v", err)
		}
		assertListenerClosed(t, <-srv.activeAddrs)

		entries, err := c.ReadDir("/dir")
		if err != nil {
			t.Fatalf("ReadDir: %s", err)
		}
		if len(entries) != 2 || entries[0].Name() != "a.txt" || entries[1].Name() != "b.txt" {
			t.Fatalf("unexpected listing: %+v", entries)
		}
	})

	t.Run("accept_failure_after_150_evicts_control_connection", func(t *testing.T) {
		srv := startLoopFTPServer(t, true)
		c := newLeaseTestClient(t, srv.addr(), true, 300*time.Millisecond)

		var buf bytes.Buffer
		err := c.Retrieve("/nodial", &buf)
		if err == nil {
			t.Fatal("expected accept failure error")
		}
		// The prepared listener must be closed even though the getter failed.
		assertListenerClosed(t, <-srv.activeAddrs)

		buf.Reset()
		if err := c.Retrieve("/ok", &buf); err != nil {
			t.Fatalf("retrieve after eviction: %s", err)
		}
		if buf.String() != "ok-data" {
			t.Fatalf("post-eviction transfer content wrong: %q", buf.String())
		}

		srv.waitForSettled(t, 2)
		total, _, cmds := srv.snapshot()
		if total != 2 {
			t.Fatalf("expected a replacement control connection, got %d", total)
		}
		if got := c.numOpenConns(); got != 1 {
			t.Fatalf("connection cap exceeded, pool reports %d open conns", got)
		}
		// The new session must have read its own greeting/command sequence;
		// the failed session's missing 226 cannot bleed into it.
		if !strings.Contains(strings.Join(cmds, "\n"), "#2 USER") {
			t.Fatalf("replacement session did not log in: %v", cmds)
		}
	})
}

func TestPreparedPool(t *testing.T) {
	t.Run("reject_and_success_sequence_holds_single_connection", func(t *testing.T) {
		srv := startLoopFTPServer(t, false)
		c := newLeaseTestClient(t, srv.addr(), false, 2*time.Second)

		var buf bytes.Buffer
		if err := c.Retrieve("/denied", &buf); err == nil {
			t.Fatal("expected 550")
		}
		assertEOFSoon(t, <-srv.dataConns)

		if err := c.Retrieve("/ok", &buf); err != nil {
			t.Fatalf("retrieve: %s", err)
		}
		if buf.String() != "ok-data" {
			t.Fatalf("bad content: %q", buf.String())
		}

		if _, err := c.ReadDir("/denied"); err == nil {
			t.Fatal("expected 550 on MLSD")
		}
		assertEOFSoon(t, <-srv.dataConns)

		entries, err := c.ReadDir("/dir")
		if err != nil {
			t.Fatalf("readdir: %s", err)
		}
		if len(entries) != 2 {
			t.Fatalf("unexpected entries: %+v", entries)
		}

		buf.Reset()
		if err := c.Retrieve("/ok", &buf); err != nil {
			t.Fatalf("final retrieve: %s", err)
		}
		if buf.String() != "ok-data" {
			t.Fatalf("final content wrong: %q", buf.String())
		}

		total, maxLive, _ := srv.snapshot()
		if total != 1 || maxLive != 1 {
			t.Fatalf("expected exactly one persistent session, got total=%d max=%d", total, maxLive)
		}
		if got := c.numOpenConns(); got != 1 {
			t.Fatalf("pool counter inconsistent with reality: %d", got)
		}
	})

	t.Run("post150_failure_rotates_connection_without_exceeding_cap", func(t *testing.T) {
		srv := startLoopFTPServer(t, false)
		c := newLeaseTestClient(t, srv.addr(), false, 300*time.Millisecond)

		var buf bytes.Buffer

		// 1. rejected before 150: connection stays pooled.
		if err := c.Retrieve("/denied", &buf); err == nil {
			t.Fatal("expected 550")
		}
		assertEOFSoon(t, <-srv.dataConns)

		// 2. normal success on the original connection.
		if err := c.Retrieve("/ok", &buf); err != nil {
			t.Fatalf("first success: %s", err)
		}
		if buf.String() != "ok-data" {
			t.Fatalf("bad content: %q", buf.String())
		}

		// 3. failure after 150 with an unrecoverable reply boundary.
		buf.Reset()
		if err := c.Retrieve("/stall", &buf); err == nil {
			t.Fatal("expected transfer failure after 150")
		}

		// 4. replacement connection must serve the next real read correctly;
		//    the late 226 from the old session must not be its reply.
		buf.Reset()
		if err := c.Retrieve("/ok", &buf); err != nil {
			t.Fatalf("retrieve after rotation: %s", err)
		}
		if buf.String() != "ok-data" {
			t.Fatalf("content after rotation wrong: %q", buf.String())
		}

		// Let the stale session's late 226 land (or fail against the closed
		// socket), then prove the next command still reads its own reply.
		time.Sleep(1200 * time.Millisecond)
		buf.Reset()
		if err := c.Retrieve("/ok", &buf); err != nil {
			t.Fatalf("retrieve after late reply: %s", err)
		}
		if buf.String() != "ok-data" {
			t.Fatalf("content after late reply wrong: %q", buf.String())
		}

		srv.waitForSettled(t, 2)
		total, _, cmds := srv.snapshot()
		if total != 2 {
			t.Fatalf("expected one rotated connection, got %d", total)
		}
		if got := c.numOpenConns(); got != 1 {
			t.Fatalf("ConnectionsPerHost=1 violated, pool reports %d open conns", got)
		}
		if len(c.freeConnCh) != 1 {
			t.Fatalf("idle queue length = %d, want 1", len(c.freeConnCh))
		}
		if !strings.Contains(strings.Join(cmds, "\n"), "#2 RETR /ok") {
			t.Fatalf("replacement session never served the follow-up RETR: %v", cmds)
		}
	})

	t.Run("non_success_final_reply_evicts_and_close_stays_consistent", func(t *testing.T) {
		srv := startLoopFTPServer(t, false)
		c := newLeaseTestClient(t, srv.addr(), false, 2*time.Second)

		// ReadDir surfaces the FTP reply code directly (no resume wrapping).
		_, err := c.ReadDir("/aborted")
		if err == nil {
			t.Fatal("expected transfer failure")
		}
		fe, ok := err.(Error)
		if !ok || fe.Code() != 426 {
			t.Fatalf("expected 426 error with its code preserved, got %v", err)
		}
		if !fe.Temporary() {
			t.Fatalf("4xx completion reply must remain Temporary: %v", err)
		}
		// The error must not have been swallowed or turned into success by
		// cleanup bookkeeping.
		if _, ok := err.(Error); !ok {
			t.Fatalf("public Error type lost: %T", err)
		}

		var buf bytes.Buffer
		if err := c.Retrieve("/ok", &buf); err != nil {
			t.Fatalf("retrieve after eviction: %s", err)
		}
		if buf.String() != "ok-data" {
			t.Fatalf("content after eviction wrong: %q", buf.String())
		}

		total, _, _ := srv.snapshot()
		if total != 2 {
			t.Fatalf("426 after 150 must rotate the control connection, got %d sessions", total)
		}

		// A further pre-150 rejection must still clean up its own socket.
		buf.Reset()
		if err := c.Retrieve("/denied", &buf); err == nil {
			t.Fatal("expected 550")
		}
		assertEOFSoon(t, <-srv.dataConns)

		if got := c.numOpenConns(); got != 1 {
			t.Fatalf("pool counter inconsistent with reality before Close: %d", got)
		}
		if len(c.freeConnCh) != 1 {
			t.Fatalf("idle queue length = %d, want 1", len(c.freeConnCh))
		}

		// Client.Close must match the actual close state: afterwards the
		// server-side sessions wind down and the client reports closed.
		if err := c.Close(); err != nil {
			t.Fatalf("close: %s", err)
		}
		if got := c.numOpenConns(); got != 0 {
			t.Fatalf("pool counter after Close = %d, want 0", got)
		}
		if err := c.Close(); err == nil {
			t.Fatal("second Close must report the client was already closed")
		}
	})
}
