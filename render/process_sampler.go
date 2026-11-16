//-----------------------------------------------------------------------------
/*

SDF Field Evaluation Subprocess

A line oriented JSON protocol over stdin/stdout is used to evaluate batches
of points in a separate process.  Requests are:

	{"id":<int>,"points":[[x,y,z],...]}

Responses are:

	{"id":<int>,"values":[<float>,...]}

The response for a request carries the same id and the values are in the
same order as the request points.  Responses may be returned out of order.

*/
//-----------------------------------------------------------------------------

package render

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"os/exec"
	"sync"
)

//-----------------------------------------------------------------------------

// processRequest is a single evaluation request.
type processRequest struct {
	ID     int          `json:"id"`
	Points [][3]float64 `json:"points"`
}

// processResponse is a single evaluation response.
// Pointers are used so that missing or null values are distinguishable
// from a legitimate zero value.
type processResponse struct {
	ID     *int       `json:"id"`
	Values []*float64 `json:"values"`
}

// sendBatch is a batch of world-space points to be sent to the subprocess.
type sendBatch struct {
	id     int
	points [][3]float64
}

// sampleEvent is an event produced by the stdout reader.
type sampleEvent struct {
	id     int
	values []float64
	eof    bool
	err    error
}

//-----------------------------------------------------------------------------

// stderrTail continuously drains the subprocess stderr and retains the last
// max bytes.  It never blocks, so the subprocess cannot stall on a full
// stderr pipe.
type stderrTail struct {
	mu  sync.Mutex
	max int
	buf []byte
}

func newStderrTail(max int) *stderrTail {
	return &stderrTail{max: max, buf: make([]byte, 0, max)}
}

func (t *stderrTail) Write(p []byte) (int, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	n := len(p)
	if len(p) >= t.max {
		t.buf = append(t.buf[:0], p[len(p)-t.max:]...)
		return n, nil
	}
	// Drop enough from the front to make room.
	keep := t.max - len(p)
	if len(t.buf) > keep {
		t.buf = append(t.buf[:0], t.buf[len(t.buf)-keep:]...)
	}
	t.buf = append(t.buf, p...)
	return n, nil
}

func (t *stderrTail) Tail() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	return string(append([]byte(nil), t.buf...))
}

//-----------------------------------------------------------------------------

// processSampler owns a single field evaluation subprocess and its protocol
// plumbing.  All protocol state (id table, pending batches) belongs to one
// instance; instances never share state.
type processSampler struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	stdout io.ReadCloser
	stderr *stderrTail

	sendCh chan *sendBatch
	events chan sampleEvent
	errCh  chan error

	stop     chan struct{}
	stopOnce sync.Once

	stdinOnce sync.Once

	wg sync.WaitGroup
}

// startProcessSampler starts the command and its reader/writer goroutines.
func startProcessSampler(command []string, maxInFlight int) (*processSampler, error) {
	p := &processSampler{
		stderr: newStderrTail(4 * 1024),
		sendCh: make(chan *sendBatch, maxInFlight),
		events: make(chan sampleEvent, 2*maxInFlight+8),
		errCh:  make(chan error, 4),
		stop:   make(chan struct{}),
	}

	p.cmd = exec.Command(command[0], command[1:]...)
	stdin, err := p.cmd.StdinPipe()
	if err != nil {
		return nil, fmt.Errorf("field process: stdin pipe: %w", err)
	}
	stdout, err := p.cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("field process: stdout pipe: %w", err)
	}
	p.stdin = stdin
	p.stdout = stdout
	// A non-*os.File writer makes os/exec run a copying goroutine that
	// continuously drains stderr.  cmd.Wait waits for that goroutine.
	p.cmd.Stderr = p.stderr

	if err := p.cmd.Start(); err != nil {
		return nil, fmt.Errorf("field process: start %q: %w", command[0], err)
	}

	p.wg.Add(2)
	go p.reader()
	go p.writer()
	return p, nil
}

//-----------------------------------------------------------------------------

// reader reads newline terminated responses from stdout.  A final line
// without a newline is reported as an error (a half line EOF).
func (p *processSampler) reader() {
	defer p.wg.Done()
	r := bufio.NewReader(p.stdout)
	for {
		line, err := r.ReadBytes('\n')
		if err != nil {
			if err == io.EOF {
				if len(line) > 0 {
					p.emit(sampleEvent{err: fmt.Errorf("field process: protocol: partial response line at EOF")})
					return
				}
				p.emit(sampleEvent{eof: true})
				return
			}
			p.emit(sampleEvent{err: fmt.Errorf("field process: read stdout: %w", err)})
			return
		}
		line = line[:len(line)-1]
		if len(line) > 0 && line[len(line)-1] == '\r' {
			line = line[:len(line)-1]
		}
		var resp processResponse
		if err := json.Unmarshal(line, &resp); err != nil {
			p.emit(sampleEvent{err: fmt.Errorf("field process: protocol: invalid JSON: %w", err)})
			return
		}
		if resp.ID == nil {
			p.emit(sampleEvent{err: fmt.Errorf("field process: protocol: response has no id")})
			return
		}
		values := make([]float64, len(resp.Values))
		for i, v := range resp.Values {
			if v == nil {
				p.emit(sampleEvent{err: fmt.Errorf("field process: protocol: response %d has a null value", *resp.ID)})
				return
			}
			if math.IsNaN(*v) || math.IsInf(*v, 0) {
				p.emit(sampleEvent{err: fmt.Errorf("field process: protocol: response %d has a non-finite value", *resp.ID)})
				return
			}
			values[i] = *v
		}
		p.emit(sampleEvent{id: *resp.ID, values: values})
	}
}

// writer serializes requests onto the subprocess stdin.
func (p *processSampler) writer() {
	defer p.wg.Done()
	for {
		select {
		case b, ok := <-p.sendCh:
			if !ok {
				p.closeStdin()
				return
			}
			req := processRequest{ID: b.id, Points: b.points}
			data, err := json.Marshal(req)
			if err != nil {
				p.reportError(fmt.Errorf("field process: encode request: %w", err))
				return
			}
			data = append(data, '\n')
			if err := writeFull(p.stdin, data); err != nil {
				p.reportError(fmt.Errorf("field process: write request: %w", err))
				return
			}
		case <-p.stop:
			p.closeStdin()
			return
		}
	}
}

// emit hands an event to the coordinator.  Once shutdown has started the
// event is dropped so a reader can never be wedged on a full channel; it
// continues draining stdout until EOF so the child can exit.
func (p *processSampler) emit(ev sampleEvent) {
	select {
	case p.events <- ev:
	case <-p.stop:
	}
}

func (p *processSampler) reportError(err error) {
	select {
	case p.errCh <- err:
	case <-p.stop:
	}
}

func (p *processSampler) closeStdin() {
	p.stdinOnce.Do(func() {
		p.stdin.Close()
	})
}

// beginShutdown signals all goroutines that no further events are wanted.
func (p *processSampler) beginShutdown() {
	p.stopOnce.Do(func() {
		close(p.stop)
	})
}

// abort terminates the subprocess and waits for every internal goroutine.
// It is used on error and cancellation paths, so it can never block on a
// response.
func (p *processSampler) abort() {
	p.beginShutdown()
	if p.cmd.Process != nil {
		_ = p.cmd.Process.Kill()
	}
	p.closeStdin()
	p.wg.Wait()
	_ = p.cmd.Wait()
}

// finish performs a graceful shutdown: the stdin stream is closed, the
// subprocess is allowed to exit, and then it is reaped.  Cancellation of
// ctx kills the process and returns the context error.
func (p *processSampler) finish(ctx context.Context) error {
	// All requests have been answered, so anything still buffered is
	// stray.  Dropping events keeps the reader draining stdout without
	// blocking on a full event channel.
	p.beginShutdown()
	close(p.sendCh)

	done := make(chan error, 1)
	go func() {
		p.wg.Wait()
		done <- p.cmd.Wait()
	}()

	select {
	case err := <-done:
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return err
	case <-ctx.Done():
		p.beginShutdown()
		if p.cmd.Process != nil {
			_ = p.cmd.Process.Kill()
		}
		p.closeStdin()
		<-done
		return ctx.Err()
	}
}

// processExitError decorates a process exit error with the retained stderr
// tail.
func (p *processSampler) processExitError(err error) error {
	if err == nil {
		return nil
	}
	return p.withTail(fmt.Errorf("field process: %w", err))
}

// withTail appends the retained stderr tail, if any, to an error.
func (p *processSampler) withTail(err error) error {
	tail := p.stderr.Tail()
	if tail == "" {
		return err
	}
	return fmt.Errorf("%w: %s", err, tail)
}

//-----------------------------------------------------------------------------

func writeFull(w io.Writer, data []byte) error {
	for len(data) > 0 {
		n, err := w.Write(data)
		if err != nil {
			return err
		}
		if n <= 0 {
			return io.ErrShortWrite
		}
		data = data[n:]
	}
	return nil
}

//-----------------------------------------------------------------------------
