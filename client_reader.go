package gortsplib

import (
	"sync"

	"github.com/bluenviron/gortsplib/v5/pkg/base"
	"github.com/bluenviron/gortsplib/v5/pkg/liberrors"
)

type clientReader struct {
	c *Client

	mutex                  sync.Mutex
	allowInterleavedFrames bool

	terminate chan struct{}

	done chan struct{}
}

func (r *clientReader) start() {
	r.terminate = make(chan struct{})
	r.done = make(chan struct{})

	go r.run()
}

func (r *clientReader) setAllowInterleavedFrames(v bool) {
	r.mutex.Lock()
	defer r.mutex.Unlock()
	r.allowInterleavedFrames = v
}

func (r *clientReader) close() {
	close(r.terminate)
	<-r.done
}

func (r *clientReader) run() {
	defer close(r.done)

	err := r.runInner()

	select {
	case r.c.chReadError <- err:
	case <-r.terminate:
	}
}

func (r *clientReader) runInner() error {
	for {
		what, err := r.c.conn.Read()
		if err != nil {
			return err
		}

		now := r.c.timeNow()
		r.c.tcpLastInboundTime.Store(now.Unix())

		switch what := what.(type) {
		case *base.Response:
			// flip the reception gate before any frame following the response
			// can be read, so PLAY handovers never lose or leak the first frame.
			if r.c.epochGate != nil {
				r.c.epochGate.onResponse(what.StatusCode)
			}

			select {
			case r.c.chResponse <- what:
			case <-r.terminate:
			}

		case *base.Request:
			select {
			case r.c.chRequest <- what:
			case <-r.terminate:
			}

		case *base.InterleavedFrame:
			r.mutex.Lock()

			if !r.allowInterleavedFrames {
				r.mutex.Unlock()
				return liberrors.ErrClientUnexpectedFrame{}
			}

			if r.c.epochGate != nil && !r.c.epochGate.allow() {
				// frames of the previous position are discarded; frames of the
				// new one wait here until the new epoch is published.
				r.mutex.Unlock()
				continue
			}

			if cb, ok := r.c.tcpCallbackByChannel[what.Channel]; ok {
				cb(what.Payload)
			}
			r.mutex.Unlock()
		}
	}
}
