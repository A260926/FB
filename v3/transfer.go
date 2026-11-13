package grab

import (
	"context"
	"errors"
	"fmt"
	"io"
	"sync/atomic"
	"time"

	"github.com/cavaliergopher/grab/v3/pkg/bps"
)

// errTransferWrite marks an error that originated while writing to the
// destination, and errTransferLimit an error returned by the rate limiter.
// Wrapping them lets a reconnect decision tell these apart from errors reading
// the response body, even when a writer or limiter happens to return the same
// concrete error type as the source reader (e.g. io.ErrUnexpectedEOF or a
// net.Error).
var (
	errTransferWrite = errors.New("grab: destination write failed")
	errTransferLimit = errors.New("grab: rate limiter failed")
)

type transfer struct {
	n     int64 // must be 64bit aligned on 386
	ctx   context.Context
	gauge bps.Gauge
	lim   RateLimiter
	w     io.Writer
	r     io.Reader
	b     []byte
}

func newTransfer(ctx context.Context, lim RateLimiter, dst io.Writer, src io.Reader, buf []byte) *transfer {
	return &transfer{
		ctx:   ctx,
		gauge: bps.NewSMA(6), // five second moving average sampling every second
		lim:   lim,
		w:     dst,
		r:     src,
		b:     buf,
	}
}

// copy behaves similarly to io.CopyBuffer except that it checks for cancelation
// of the given context.Context, reports progress in a thread-safe manner and
// tracks the transfer rate.
func (c *transfer) copy() (written int64, err error) {
	// maintain a bps gauge in another goroutine
	ctx, cancel := context.WithCancel(c.ctx)
	defer cancel()
	go bps.Watch(ctx, c.gauge, c.N, time.Second)

	// start the transfer
	if c.b == nil {
		c.b = make([]byte, 32*1024)
	}
	for {
		select {
		case <-c.ctx.Done():
			err = c.ctx.Err()
			return
		default:
			// keep working
		}
		nr, er := c.r.Read(c.b)
		if nr > 0 {
			nw, ew := c.w.Write(c.b[0:nr])
			if nw > 0 {
				written += int64(nw)
				atomic.StoreInt64(&c.n, written)
			}
			if ew != nil {
				err = fmt.Errorf("%w: %w", errTransferWrite, ew)
				break
			}
			if nr != nw {
				err = fmt.Errorf("%w: %w", errTransferWrite, io.ErrShortWrite)
				break
			}
			// wait for rate limiter
			if c.lim != nil {
				if lerr := c.lim.WaitN(c.ctx, nr); lerr != nil {
					err = fmt.Errorf("%w: %w", errTransferLimit, lerr)
					return
				}
			}
		}
		if er != nil {
			if er != io.EOF {
				err = er
			}
			break
		}
	}
	return written, err
}

// N returns the number of bytes transferred.
func (c *transfer) N() (n int64) {
	if c == nil {
		return 0
	}
	n = atomic.LoadInt64(&c.n)
	return
}

// BPS returns the current bytes per second transfer rate using a simple moving
// average.
func (c *transfer) BPS() (bps float64) {
	if c == nil || c.gauge == nil {
		return 0
	}
	return c.gauge.BPS()
}
