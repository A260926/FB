package ftp

import (
	"errors"
	"net"
	"net/textproto"
	"strings"
	"sync"
	"testing"
	"time"
)

// TestWalkBranchErrorPolicy covers WalkWithOptions / WalkOptions.OnError.
//
// The scenario server is built on net.Pipe (the sandbox forbids binding local
// ports), but it speaks the real FTP control protocol and the client uses the
// real ServerConn.List code path with a scripted dialFunc for data
// connections, so EPSV/PASV, LIST/MLSD and NOOP command sequences are real.
func TestWalkBranchErrorPolicy(t *testing.T) {
	t.Run("full_tree_skip_550_and_continue", testFullTreeSkip550)
	t.Run("callback_rejects_550", testCallbackRejects550)
	t.Run("error_421_is_not_skippable", test421NotSkippable)
	t.Run("data_read_and_close_failures", testDataReadAndCloseFailures)
	t.Run("skip_dir_sends_no_listing", testSkipDirSendsNoListing)
	t.Run("root_failure_can_be_skipped", testRootFailureCanBeSkipped)
	t.Run("repeated_next_after_termination", testRepeatedNextAfterTermination)
	t.Run("nil_options_keep_legacy_behavior", testNilOptionsKeepLegacyBehavior)
	t.Run("only_pure_550_is_skippable", testOnlyPure550IsSkippable)
}

type listAction int

const (
	listOK listAction = iota
	list550
	list421
	listDataFail
)

type pipeConfig struct {
	mlsd        bool
	disableEPSV bool
	// lists maps the LIST/MLSD argument to the scripted outcome.
	lists map[string]listAction
	// payloads maps the LIST/MLSD argument to the raw data connection payload.
	payloads map[string]string
	// failDataAtDial, when non-zero, wraps the nth (1-based) data connection
	// dialed by the client so that reads fail with readErr and Close fails
	// with closeErr.
	failDataAtDial int
	readErr        error
	closeErr       error
}

type pipeServer struct {
	t   *testing.T
	cfg pipeConfig

	mu       sync.Mutex
	commands []string

	controlClient net.Conn
	client        *ServerConn
	dataSrv       chan net.Conn
	done          chan struct{}
	dialCount     int
}

func newPipeServer(t *testing.T, cfg pipeConfig) *pipeServer {
	t.Helper()

	controlClient, controlServer := net.Pipe()
	if err := controlClient.SetDeadline(time.Now().Add(15 * time.Second)); err != nil {
		t.Fatalf("set control deadline: %v", err)
	}
	if err := controlServer.SetDeadline(time.Now().Add(15 * time.Second)); err != nil {
		t.Fatalf("set control deadline: %v", err)
	}

	s := &pipeServer{
		t:             t,
		cfg:           cfg,
		controlClient: controlClient,
		dataSrv:       make(chan net.Conn, 32),
		done:          make(chan struct{}),
	}

	go s.serve(controlServer)

	dialFunc := func(_, _ string) (net.Conn, error) {
		cli, srv := net.Pipe()
		s.dataSrv <- srv
		s.dialCount++
		if s.cfg.failDataAtDial == s.dialCount {
			return &errReadConn{Conn: cli, readErr: s.cfg.readErr, closeErr: s.cfg.closeErr}, nil
		}
		return cli, nil
	}

	c := &ServerConn{
		conn:          textproto.NewConn(controlClient),
		netConn:       controlClient,
		host:          "127.0.0.1",
		options:       &dialOptions{dialFunc: dialFunc, location: time.UTC},
		mlstSupported: cfg.mlsd,
	}
	if cfg.disableEPSV {
		c.skipEPSV = true
	}
	s.client = c

	t.Cleanup(func() {
		_ = controlClient.Close()
		_ = controlServer.Close()
		<-s.done
	})

	return s
}

func (s *pipeServer) serve(control net.Conn) {
	defer close(s.done)

	p := textproto.NewConn(control)
	defer p.Close()

	for {
		line, err := p.ReadLine()
		if err != nil {
			return
		}

		s.mu.Lock()
		s.commands = append(s.commands, line)
		s.mu.Unlock()

		cmd, arg, _ := strings.Cut(line, " ")
		switch cmd {
		case "EPSV":
			_ = p.PrintfLine("229 Entering Extended Passive Mode (|||1234|)")
		case "PASV":
			_ = p.PrintfLine("227 Entering Passive Mode (127,0,0,1,4,210).")
		case "NOOP":
			_ = p.PrintfLine("200 NOOP ok.")
		case "LIST", "MLSD":
			if !s.handleList(p, arg) {
				return
			}
		case "QUIT":
			_ = p.PrintfLine("221 Goodbye.")
			return
		default:
			_ = p.PrintfLine("500 Unknown command %s.", cmd)
		}
	}
}

// handleDataConn returns the server side of the data connection that the
// client opened before sending the transfer command.
func (s *pipeServer) handleDataConn() net.Conn {
	select {
	case conn := <-s.dataSrv:
		return conn
	case <-time.After(5 * time.Second):
		s.t.Error("timed out waiting for the client data connection")
		return nil
	}
}

func (s *pipeServer) handleList(p *textproto.Conn, arg string) bool {
	action := s.cfg.lists[arg]

	switch action {
	case list550:
		if dc := s.handleDataConn(); dc != nil {
			_ = dc.Close()
		}
		_ = p.PrintfLine("550 %s: directory unavailable", arg)
	case list421:
		if dc := s.handleDataConn(); dc != nil {
			_ = dc.Close()
		}
		_ = p.PrintfLine("421 Service not available, closing control connection.")
	case listDataFail:
		_ = p.PrintfLine("150 Opening ASCII mode data connection for file list")
		if dc := s.handleDataConn(); dc != nil {
			_ = dc.Close()
		}
		_ = p.PrintfLine("226 Transfer complete")
	case listOK:
		_ = p.PrintfLine("150 Opening ASCII mode data connection for file list")
		dc := s.handleDataConn()
		if dc != nil {
			_, _ = dc.Write([]byte(s.cfg.payloads[arg]))
			_ = dc.Close()
		}
		_ = p.PrintfLine("226 Transfer complete")
	default:
		if dc := s.handleDataConn(); dc != nil {
			_ = dc.Close()
		}
		_ = p.PrintfLine("550 %s: unscripted path", arg)
	}

	return true
}

func (s *pipeServer) recordedCommands() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]string, len(s.commands))
	copy(out, s.commands)
	return out
}

type failureRecord struct {
	path string
	err  error
}

// errReadConn fails every Read with readErr and every Close with closeErr.
type errReadConn struct {
	net.Conn
	readErr  error
	closeErr error
}

func (e *errReadConn) Read([]byte) (int, error) { return 0, e.readErr }

func (e *errReadConn) Close() error {
	_ = e.Conn.Close()
	return e.closeErr
}

// testFullTreeSkip550 walks a complete parent listing containing a normal
// directory, two 550 directories and a later sibling file. Both failing
// branches must be notified exactly once and skipped, the sibling entries
// must be delivered in the walker's existing order, and a NOOP issued from
// the callback must get its own 200 reply.
func testFullTreeSkip550(t *testing.T) {
	s := newPipeServer(t, pipeConfig{
		lists: map[string]listAction{
			"/":        listOK,
			"/okdir":   listOK,
			"/denied":  list550,
			"/denied2": list550,
		},
		payloads: map[string]string{
			// Remote listing order: normal dir, denied dir, denied dir, sibling file.
			"/": "drwxr-xr-x   1 ftp      wheel           0 Jan 29 10:29 okdir\r\n" +
				"drwxr-xr-x   1 ftp      wheel           0 Jan 29 10:29 denied\r\n" +
				"drwxr-xr-x   1 ftp      wheel           0 Jan 29 10:29 denied2\r\n" +
				"-rw-r--r--   1 ftp      wheel           5 Jan 29 10:29 after.txt\r\n",
			"/okdir": "-rw-r--r--   1 ftp      wheel           9 Jan 29 10:29 inner.txt\r\n",
		},
	})

	var failures []failureRecord
	w := s.client.WalkWithOptions("/", &WalkOptions{
		OnError: func(dir string, err error) bool {
			var tpErr *textproto.Error
			if !errors.As(err, &tpErr) || tpErr.Code != 550 {
				t.Errorf("callback got non-550 error for %s: %v", dir, err)
			}
			failures = append(failures, failureRecord{path: dir, err: err})

			// The control connection must be fully usable at this point:
			// this NOOP gets its own 200 and the next listing must not
			// consume its reply.
			if err := s.client.NoOp(); err != nil {
				t.Errorf("NoOp from callback failed: %v", err)
			}
			return true
		},
	})

	var gotPaths []string
	gotTypes := map[string]EntryType{}
	for w.Next() {
		gotPaths = append(gotPaths, w.Path())
		gotTypes[w.Path()] = w.Stat().Type
	}
	if err := w.Err(); err != nil {
		t.Fatalf("walk ended with error after skipping 550 branches: %v", err)
	}

	wantPaths := []string{
		"/after.txt",
		"/denied2",
		"/denied",
		"/okdir",
		"/okdir/inner.txt",
	}
	if len(gotPaths) != len(wantPaths) {
		t.Fatalf("visited %v, want %v", gotPaths, wantPaths)
	}
	seen := map[string]bool{}
	for i, want := range wantPaths {
		if gotPaths[i] != want {
			t.Fatalf("visited %v, want %v", gotPaths, wantPaths)
		}
		if seen[want] {
			t.Fatalf("path %q delivered more than once", want)
		}
		seen[want] = true
	}
	if gotTypes["/denied"] != EntryTypeFolder || gotTypes["/after.txt"] != EntryTypeFile {
		t.Fatalf("unexpected entry types: %v", gotTypes)
	}

	// Each failing branch is notified once, with its full path and the raw error.
	wantFailures := []string{"/denied2", "/denied"}
	if len(failures) != len(wantFailures) {
		t.Fatalf("failures = %v, want %v", failures, wantFailures)
	}
	for i, want := range wantFailures {
		if failures[i].path != want {
			t.Fatalf("failure paths = %v, want %v", failures, wantFailures)
		}
		var tpErr *textproto.Error
		if !errors.As(failures[i].err, &tpErr) {
			t.Fatalf("callback did not receive the raw textproto error: %T", failures[i].err)
		}
	}

	wantCommands := []string{
		"EPSV", "LIST /",
		"EPSV", "LIST /denied2", "NOOP",
		"EPSV", "LIST /denied", "NOOP",
		"EPSV", "LIST /okdir",
	}
	if got := s.recordedCommands(); !equalStrings(got, wantCommands) {
		t.Fatalf("commands = %v, want %v", got, wantCommands)
	}
	if countVerb(s.recordedCommands(), "EPSV") != 4 {
		t.Fatalf("expected one EPSV per listing attempt, got %v", s.recordedCommands())
	}
}

// testCallbackRejects550 proves that a false callback result stops the walk,
// keeps the original 550 error in Err, leaves queued siblings untouched and
// does not notify twice.
func testCallbackRejects550(t *testing.T) {
	s := newPipeServer(t, pipeConfig{
		lists: map[string]listAction{
			"/":       listOK,
			"/denied": list550,
		},
		payloads: map[string]string{
			"/": "-rw-r--r--   1 ftp      wheel           1 Jan 29 10:29 a.txt\r\n" +
				"drwxr-xr-x   1 ftp      wheel           0 Jan 29 10:29 denied\r\n" +
				"-rw-r--r--   1 ftp      wheel           1 Jan 29 10:29 b.txt\r\n",
		},
	})

	notifications := 0
	w := s.client.WalkWithOptions("/", &WalkOptions{
		OnError: func(dir string, err error) bool {
			notifications++
			return false
		},
	})

	var got []string
	for w.Next() {
		got = append(got, w.Path())
	}
	if !equalStrings(got, []string{"/b.txt", "/denied"}) {
		t.Fatalf("visited %v, want [/b.txt /denied]", got)
	}

	var tpErr *textproto.Error
	if !errors.As(w.Err(), &tpErr) || tpErr.Code != 550 {
		t.Fatalf("Err = %v, want textproto 550", w.Err())
	}
	if w.Path() != "/denied" || w.Stat().Type != EntryTypeFolder {
		t.Fatalf("Path/Stat changed after termination: %q %v", w.Path(), w.Stat())
	}
	// a.txt was still queued when the denied branch stopped the walk.
	if len(w.stack) != 1 {
		t.Fatalf("queued sibling lost, stack = %d", len(w.stack))
	}

	wantCommands := []string{"EPSV", "LIST /", "EPSV", "LIST /denied"}
	if got := s.recordedCommands(); !equalStrings(got, wantCommands) {
		t.Fatalf("commands = %v, want %v", got, wantCommands)
	}

	commandsBefore := s.recordedCommands()
	for i := 0; i < 3; i++ {
		if w.Next() {
			t.Fatal("Next returned true after the walk was rejected")
		}
	}
	if !equalStrings(s.recordedCommands(), commandsBefore) {
		t.Fatalf("terminated walker re-sent commands: before %v after %v", commandsBefore, s.recordedCommands())
	}
	if notifications != 1 {
		t.Fatalf("callback invoked %d times, want 1", notifications)
	}
	var tpErr2 *textproto.Error
	if !errors.As(w.Err(), &tpErr2) || tpErr2.Code != 550 {
		t.Fatalf("Err after repeated Next = %v, want the same 550", w.Err())
	}
}

// test421NotSkippable proves that a 421 reply stops the walk even when the
// callback would accept skipping, that the callback is not even invoked and
// that Err keeps the original error.
func test421NotSkippable(t *testing.T) {
	s := newPipeServer(t, pipeConfig{
		lists: map[string]listAction{
			"/":     listOK,
			"/dead": list421,
		},
		payloads: map[string]string{
			"/": "drwxr-xr-x   1 ftp      wheel           0 Jan 29 10:29 dead\r\n" +
				"-rw-r--r--   1 ftp      wheel           1 Jan 29 10:29 later.txt\r\n",
		},
	})

	notifications := 0
	w := s.client.WalkWithOptions("/", &WalkOptions{
		OnError: func(dir string, err error) bool {
			notifications++
			return true
		},
	})

	var got []string
	for w.Next() {
		got = append(got, w.Path())
	}
	if !equalStrings(got, []string{"/later.txt", "/dead"}) {
		t.Fatalf("visited %v, want [/later.txt /dead]", got)
	}

	var tpErr *textproto.Error
	if !errors.As(w.Err(), &tpErr) || tpErr.Code != 421 {
		t.Fatalf("Err = %v, want textproto 421", w.Err())
	}
	if notifications != 0 {
		t.Fatalf("callback invoked for non-550 error %d times", notifications)
	}

	wantCommands := []string{"EPSV", "LIST /", "EPSV", "LIST /dead"}
	if got := s.recordedCommands(); !equalStrings(got, wantCommands) {
		t.Fatalf("commands = %v, want %v", got, wantCommands)
	}

	commandsBefore := s.recordedCommands()
	if w.Next() {
		t.Fatal("Next returned true after a 421 failure")
	}
	if !equalStrings(s.recordedCommands(), commandsBefore) {
		t.Fatalf("terminated walker re-sent commands: %v", s.recordedCommands())
	}
}

// testDataReadAndCloseFailures proves that data stream read errors joined
// with data connection closing errors are never skippable, even when the
// callback returns true.
func testDataReadAndCloseFailures(t *testing.T) {
	readErr := errors.New("simulated data read failure")
	closeErr := errors.New("simulated data close failure")

	s := newPipeServer(t, pipeConfig{
		lists: map[string]listAction{
			"/":       listOK,
			"/broken": listDataFail,
		},
		payloads: map[string]string{
			"/": "drwxr-xr-x   1 ftp      wheel           0 Jan 29 10:29 broken\r\n" +
				"-rw-r--r--   1 ftp      wheel           1 Jan 29 10:29 later.txt\r\n",
		},
		// The root listing uses the first data connection; the broken branch
		// uses the second one, where reads and Close fail.
		failDataAtDial: 2,
		readErr:        readErr,
		closeErr:       closeErr,
	})

	notifications := 0
	w := s.client.WalkWithOptions("/", &WalkOptions{
		OnError: func(dir string, err error) bool {
			notifications++
			return true
		},
	})

	var got []string
	for w.Next() {
		got = append(got, w.Path())
	}
	if !equalStrings(got, []string{"/later.txt", "/broken"}) {
		t.Fatalf("visited %v, want [/later.txt /broken]", got)
	}

	err := w.Err()
	if err == nil {
		t.Fatal("Err = nil, want the joined data errors")
	}
	if !errors.Is(err, readErr) || !errors.Is(err, closeErr) {
		t.Fatalf("Err = %v, want it to wrap both %v and %v", err, readErr, closeErr)
	}
	var tpErr *textproto.Error
	if errors.As(err, &tpErr) {
		t.Fatalf("data failure must not be presented as a skippable textproto error: %v", err)
	}
	if notifications != 0 {
		t.Fatalf("callback invoked for a data connection failure %d times", notifications)
	}

	wantCommands := []string{"EPSV", "LIST /", "EPSV", "LIST /broken"}
	if got := s.recordedCommands(); !equalStrings(got, wantCommands) {
		t.Fatalf("commands = %v, want %v", got, wantCommands)
	}

	commandsBefore := s.recordedCommands()
	if w.Next() {
		t.Fatal("Next returned true after a data connection failure")
	}
	if !equalStrings(s.recordedCommands(), commandsBefore) {
		t.Fatalf("terminated walker re-sent commands: %v", s.recordedCommands())
	}
}

// testSkipDirSendsNoListing proves that SkipDir prevents both the listing
// command and any error callback for the skipped directory, while the walk
// continues. It also exercises the PASV command path (EPSV disabled).
func testSkipDirSendsNoListing(t *testing.T) {
	s := newPipeServer(t, pipeConfig{
		disableEPSV: true,
		lists: map[string]listAction{
			"/":     listOK,
			"/dirA": list550,
		},
		payloads: map[string]string{
			"/": "drwxr-xr-x   1 ftp      wheel           0 Jan 29 10:29 dirA\r\n" +
				"-rw-r--r--   1 ftp      wheel           1 Jan 29 10:29 sibling.txt\r\n",
		},
	})

	notifications := 0
	w := s.client.WalkWithOptions("/", &WalkOptions{
		OnError: func(dir string, err error) bool {
			notifications++
			return true
		},
	})

	if !w.Next() {
		t.Fatal("first Next returned false")
	}
	if w.Path() != "/sibling.txt" {
		t.Fatalf("first entry = %q, want /sibling.txt", w.Path())
	}
	if !w.Next() {
		t.Fatal("second Next returned false")
	}
	if w.Path() != "/dirA" {
		t.Fatalf("second entry = %q, want /dirA", w.Path())
	}

	// Skip the directory before it is listed: no LIST, no callback.
	w.SkipDir()
	if w.Next() {
		t.Fatal("walker did not finish after skipping the only directory")
	}
	if w.Err() != nil {
		t.Fatalf("Err = %v, want nil", w.Err())
	}
	if notifications != 0 {
		t.Fatalf("callback invoked after SkipDir %d times", notifications)
	}

	wantCommands := []string{"PASV", "LIST /"}
	if got := s.recordedCommands(); !equalStrings(got, wantCommands) {
		t.Fatalf("commands = %v, want %v", got, wantCommands)
	}
	for _, line := range s.recordedCommands() {
		if strings.HasPrefix(line, "LIST /dirA") {
			t.Fatalf("skipped directory was listed: %v", s.recordedCommands())
		}
	}
}

// testRootFailureCanBeSkipped proves that a 550 on the root itself ends the
// walk normally when the callback allows it. MLSD is used so the MLSD command
// sequence is covered as well.
func testRootFailureCanBeSkipped(t *testing.T) {
	s := newPipeServer(t, pipeConfig{
		mlsd:  true,
		lists: map[string]listAction{"/": list550},
	})

	notifications := 0
	w := s.client.WalkWithOptions("/", &WalkOptions{
		OnError: func(dir string, err error) bool {
			notifications++
			if dir != "/" {
				t.Errorf("callback path = %q, want /", dir)
			}
			var tpErr *textproto.Error
			if !errors.As(err, &tpErr) || tpErr.Code != 550 {
				t.Errorf("callback error = %v, want textproto 550", err)
			}
			return true
		},
	})

	if w.Next() {
		t.Fatal("Next returned true for an unlistable root")
	}
	if w.Err() != nil {
		t.Fatalf("Err = %v, want nil after skipping the root failure", w.Err())
	}
	if notifications != 1 {
		t.Fatalf("callback invoked %d times, want 1", notifications)
	}

	commandsBefore := s.recordedCommands()
	if w.Next() {
		t.Fatal("Next returned true after the root failure was skipped")
	}
	if !equalStrings(s.recordedCommands(), commandsBefore) {
		t.Fatalf("terminated walker re-sent commands: %v", s.recordedCommands())
	}

	wantCommands := []string{"EPSV", "MLSD /"}
	if got := s.recordedCommands(); !equalStrings(got, wantCommands) {
		t.Fatalf("commands = %v, want %v", got, wantCommands)
	}
}

// testRepeatedNextAfterTermination proves stable false results after a normal
// end: no commands are re-sent, the last entry stays available.
func testRepeatedNextAfterTermination(t *testing.T) {
	s := newPipeServer(t, pipeConfig{
		lists: map[string]listAction{"/": listOK},
		payloads: map[string]string{
			"/": "-rw-r--r--   1 ftp      wheel           1 Jan 29 10:29 f.txt\r\n",
		},
	})

	w := s.client.WalkWithOptions("/", &WalkOptions{
		OnError: func(string, error) bool { return true },
	})

	if !w.Next() {
		t.Fatal("first Next returned false")
	}
	if w.Path() != "/f.txt" {
		t.Fatalf("entry = %q, want /f.txt", w.Path())
	}
	for i := 0; i < 4; i++ {
		if w.Next() {
			t.Fatal("Next returned true after the walk finished")
		}
		if w.Err() != nil {
			t.Fatalf("Err = %v, want nil", w.Err())
		}
	}
	if w.Path() != "/f.txt" || w.Stat().Name != "f.txt" {
		t.Fatalf("last Path/Entry not preserved: %q %v", w.Path(), w.Stat())
	}

	wantCommands := []string{"EPSV", "LIST /"}
	if got := s.recordedCommands(); !equalStrings(got, wantCommands) {
		t.Fatalf("commands = %v, want %v", got, wantCommands)
	}
}

// testNilOptionsKeepLegacyBehavior proves that nil options, or a nil
// callback, reproduce the legacy Walk stopping behavior on 550.
func testNilOptionsKeepLegacyBehavior(t *testing.T) {
	cases := []struct {
		name    string
		options *WalkOptions
	}{
		{name: "nil options", options: nil},
		{name: "nil callback", options: &WalkOptions{}},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			s := newPipeServer(t, pipeConfig{
				lists: map[string]listAction{
					"/":       listOK,
					"/denied": list550,
				},
				payloads: map[string]string{
					"/": "-rw-r--r--   1 ftp      wheel           1 Jan 29 10:29 a.txt\r\n" +
						"drwxr-xr-x   1 ftp      wheel           0 Jan 29 10:29 denied\r\n",
				},
			})

			w := s.client.WalkWithOptions("/", tc.options)

			var got []string
			for w.Next() {
				got = append(got, w.Path())
			}
			if !equalStrings(got, []string{"/denied"}) {
				t.Fatalf("visited %v, want [/denied]", got)
			}
			var tpErr *textproto.Error
			if !errors.As(w.Err(), &tpErr) || tpErr.Code != 550 {
				t.Fatalf("Err = %v, want the original textproto 550", w.Err())
			}
			if w.Next() {
				t.Fatal("Next returned true after the legacy stop")
			}
			if !equalStrings(s.recordedCommands(), []string{"EPSV", "LIST /", "EPSV", "LIST /denied"}) {
				t.Fatalf("unexpected commands: %v", s.recordedCommands())
			}
		})
	}
}

// testOnlyPure550IsSkippable pins the classification rule: only a plain
// textproto 550 is skippable; a 550 joined with another error is not.
func testOnlyPure550IsSkippable(t *testing.T) {
	plain550 := &textproto.Error{Code: 550, Msg: "denied"}
	if !isSkippableBranchError(plain550) {
		t.Fatal("plain 550 must be skippable")
	}
	if isSkippableBranchError(&textproto.Error{Code: 421, Msg: "gone"}) {
		t.Fatal("421 must not be skippable")
	}
	if isSkippableBranchError(errors.New("connection reset")) {
		t.Fatal("a plain network error must not be skippable")
	}
	joined := errors.Join(plain550, errors.New("data connection close failed"))
	if isSkippableBranchError(joined) {
		t.Fatal("a 550 joined with a data closing error must not be skippable")
	}
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func countVerb(lines []string, verb string) int {
	n := 0
	for _, line := range lines {
		v, _, _ := strings.Cut(line, " ")
		if v == verb {
			n++
		}
	}
	return n
}
