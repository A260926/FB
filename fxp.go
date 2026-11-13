package ftp

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/textproto"
	"strings"
	"time"
)

// CopyError collects the per-side errors of a server-to-server (FXP) transfer
// initiated by [ServerConn.CopyTo]. Source holds the error from the RETR side
// (the receiver of CopyTo) and Destination the error from the STOR side.
type CopyError struct {
	Source      error
	Destination error
}

func (e *CopyError) Error() string {
	var b strings.Builder
	b.WriteString("ftp: FXP transfer failed")
	if e.Source != nil {
		b.WriteString(": source: ")
		b.WriteString(e.Source.Error())
	}
	if e.Destination != nil {
		b.WriteString(": destination: ")
		b.WriteString(e.Destination.Error())
	}
	return b.String()
}

// Unwrap returns the non-nil per-side errors so that errors.Is and errors.As
// can reach the original errors of both sides, including textproto.Error.
func (e *CopyError) Unwrap() []error {
	errs := make([]error, 0, 2)
	if e.Source != nil {
		errs = append(errs, e.Source)
	}
	if e.Destination != nil {
		errs = append(errs, e.Destination)
	}
	return errs
}

// fxpSideError marks which control connection an error belongs to during the
// preparation and command phases, before a CopyError can be produced.
type fxpSideError struct {
	side string
	err  error
}

func (e *fxpSideError) Error() string {
	return "ftp: FXP " + e.side + " side: " + e.err.Error()
}

func (e *fxpSideError) Unwrap() error { return e.err }

// fxpIsProtocolError reports whether err is a fully consumed FTP reply
// (textproto.Error), which leaves the control connection in a known state.
// Any other error (I/O failure, malformed reply, context cancellation) means
// the control stream boundary cannot be trusted.
func fxpIsProtocolError(err error) bool {
	var te *textproto.Error
	return errors.As(err, &te)
}

// fxpSideFail wraps err for the given side and closes the connection when the
// error is not a plain FTP reply, since the control stream is then unusable.
func (c *ServerConn) fxpSideFail(side string, err error) error {
	if !fxpIsProtocolError(err) {
		_ = c.netConn.Close()
	}
	return &fxpSideError{side: side, err: err}
}

// fxpDeadlineBackstop keeps the raw socket deadline just behind the context
// deadline so the context is marked done before the kernel I/O timeout fires.
const fxpDeadlineBackstop = 100 * time.Millisecond

// fxpWatch arms context interruption for a blocking control-reply wait.
//
// It mirrors the context deadline onto the control connection (so the kernel
// unblocks the read instead of polling) and spawns a watcher that closes the
// connection when the context ends. The returned stop function waits for no
// goroutine to be left behind and clears the deadline, so a completed call
// never leaves a stale deadline for the next command. CopyTo holds both
// connections exclusively for its whole duration, so no other deadline can be
// in effect while it runs.
func (c *ServerConn) fxpWatch(ctx context.Context) func() {
	conn := c.netConn
	if deadline, ok := ctx.Deadline(); ok {
		// Use the context deadline as a backstop slightly in the future so
		// that ctx.Done() (and the watcher's Close below) fires first. The
		// read then unblocks with the context already marked, letting callers
		// match context.DeadlineExceeded instead of a bare I/O timeout.
		_ = conn.SetDeadline(deadline.Add(fxpDeadlineBackstop))
	}

	stopped := make(chan struct{})
	finished := make(chan struct{})
	go func() {
		defer close(finished)
		select {
		case <-ctx.Done():
			// Unblock the in-flight ReadResponse; the caller will treat the
			// stream as untrusted and drop the session.
			_ = conn.Close()
			<-stopped
		case <-stopped:
		}
	}()

	return func() {
		close(stopped)
		<-finished
		_ = conn.SetDeadline(time.Time{})
	}
}

// fxpReadReply reads a single complete FTP reply and makes the wait
// interruptible by the context. A negative reply is returned as a
// *textproto.Error; I/O, cancellation and malformed replies return other
// errors and the caller must close the session. When the context ended while
// waiting, its error is returned so errors.Is can match context.Canceled and
// context.DeadlineExceeded.
func (c *ServerConn) fxpReadReply(ctx context.Context) (int, string, error) {
	stop := c.fxpWatch(ctx)
	defer stop()
	code, msg, err := c.conn.ReadResponse(-1)
	if err != nil && ctx.Err() != nil {
		return 0, "", ctx.Err()
	}
	return code, msg, err
}

// fxpCmd sends a command through the same command-injection check used by the
// rest of the client and reads its reply with context awareness.
func (c *ServerConn) fxpCmd(ctx context.Context, format string, args ...interface{}) (int, string, error) {
	if err := checkForCommandInjection(format, args...); err != nil {
		return 0, "", err
	}
	if err := ctx.Err(); err != nil {
		return 0, "", err
	}
	if _, err := c.conn.Cmd(format, args...); err != nil {
		return 0, "", err
	}
	return c.fxpReadReply(ctx)
}

// fxpReadFinal reads a transfer completion reply. 226 and 250 are success;
// any other complete reply is returned as *textproto.Error without triggering
// a retry or an ABOR.
func (c *ServerConn) fxpReadFinal(ctx context.Context) (int, string, error) {
	code, msg, err := c.fxpReadReply(ctx)
	if err != nil {
		return 0, "", err
	}
	if code == StatusClosingDataConnection || code == StatusRequestedFileActionOK {
		return code, msg, nil
	}
	return code, msg, &textproto.Error{Code: code, Msg: msg}
}

// fxpSendPort tells the source server where to open the data connection to the
// destination's passive endpoint. EPRT is always tried first, with address
// family 1 for IPv4 and 2 for IPv6. Only for IPv4, and only when EPRT is
// rejected with 500, 502 or 504, does it fall back to the legacy PORT command.
func (c *ServerConn) fxpSendPort(ctx context.Context, ip net.IP, port int) error {
	family := 2
	host := ip.String()
	v4 := ip.To4()
	if v4 != nil {
		family = 1
		host = v4.String()
	}

	// ReadResponse is called without an expected code, so a negative reply
	// arrives as a code with a nil error and must be inspected explicitly.
	code, msg, err := c.fxpCmd(ctx, "EPRT |%d|%s|%d|", family, host, port)
	if err != nil {
		return err
	}
	if code == StatusCommandOK {
		return nil
	}

	if v4 != nil &&
		(code == StatusBadCommand ||
			code == StatusNotImplemented ||
			code == StatusNotImplementedParameter) {
		octets := strings.Split(v4.String(), ".")
		portCommand := fmt.Sprintf("PORT %s,%d,%d",
			strings.Join(octets, ","), port/256, port%256)
		code2, msg2, err2 := c.fxpCmd(ctx, "%s", portCommand)
		if err2 != nil {
			return err2
		}
		if code2 != StatusCommandOK {
			return &textproto.Error{Code: code2, Msg: msg2}
		}
		return nil
	}

	return &textproto.Error{Code: code, Msg: msg}
}

// fxpAbort aborts a transfer that was accepted on the destination side and
// consumes its full acknowledgement sequence. Only these sequences are
// recognized:
//   - a single 225;
//   - 426 followed by 226;
//   - a queued transfer-completion 226 followed by the ABOR reply 225.
//
// A lone 226 cannot be attributed to ABOR: another reply (225) must follow.
// Anything outside these sequences, or any I/O/cancellation error, closes the
// session because the control stream boundary can no longer be confirmed.
func (c *ServerConn) fxpAbort(ctx context.Context) error {
	if err := ctx.Err(); err != nil {
		_ = c.netConn.Close()
		return err
	}
	if _, err := c.conn.Cmd("ABOR"); err != nil {
		_ = c.netConn.Close()
		return err
	}

	code, msg, err := c.fxpReadReply(ctx)
	if err != nil {
		_ = c.netConn.Close()
		return err
	}

	switch code {
	case StatusDataConnectionOpen: // single 225
		return nil

	case StatusTransfertAborted: // 426 then 226
		code2, msg2, err2 := c.fxpReadReply(ctx)
		if err2 != nil {
			_ = c.netConn.Close()
			return err2
		}
		if code2 != StatusClosingDataConnection {
			_ = c.netConn.Close()
			return &textproto.Error{Code: code2, Msg: msg2}
		}
		return nil

	case StatusClosingDataConnection: // ambiguous 226: a 225 must follow
		code2, msg2, err2 := c.fxpReadReply(ctx)
		if err2 != nil {
			_ = c.netConn.Close()
			return err2
		}
		if code2 != StatusDataConnectionOpen {
			_ = c.netConn.Close()
			return &textproto.Error{Code: code2, Msg: msg2}
		}
		return nil

	default:
		_ = c.netConn.Close()
		return &textproto.Error{Code: code, Msg: msg}
	}
}

// CopyTo performs an FXP (server-to-server) transfer: the file at sourcePath on
// the receiver connection is stored at destinationPath on the destination
// connection without ever passing through this process. Only the two control
// connections are used; no data connection is opened or read locally.
//
// ServerConn remains non-reentrant: while CopyTo runs both connections are
// busy until it returns. Only plaintext sessions without PRET requirements are
// supported; TLS and PRET are rejected before any command is sent.
//
// The destination enters passive mode (EPSV/PASV with the existing endpoint
// trust rules), the source is told to connect to that endpoint with EPRT
// (PORT fallback for IPv4 only), then STOR is issued on the destination and
// RETR on the source. The final reply of each side is always consumed, even if
// the other side fails; failures are returned in a *CopyError preserving the
// original textproto.Error values.
//
// A session whose final reply (positive or negative) was fully read stays
// synchronized and can be reused, for example with NoOp. On cancellation,
// I/O errors or unrecognizable reply sequences the affected session is closed,
// as its control stream can no longer be trusted.
func (c *ServerConn) CopyTo(ctx context.Context, destination *ServerConn, sourcePath, destinationPath string) error {
	// All validation happens before a single command is sent.
	if ctx == nil {
		return errors.New("ftp: CopyTo requires a non-nil context")
	}
	if destination == nil {
		return errors.New("ftp: CopyTo requires a non-nil destination connection")
	}
	if c == destination {
		return errors.New("ftp: CopyTo source and destination must be different connections")
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	if strings.ContainsAny(sourcePath, "\r\n") || strings.ContainsAny(destinationPath, "\r\n") {
		return ErrInvalidCommand
	}
	if c.options.tlsConfig != nil || destination.options.tlsConfig != nil {
		return errors.New("ftp: CopyTo does not support TLS control or data sessions")
	}
	if c.usePRET || destination.usePRET {
		return errors.New("ftp: CopyTo does not support servers requiring PRET")
	}

	// 1. Passive endpoint on the destination, reusing the EPSV/PASV selection
	// and the PASV address trust rules. openDataConn is never called.
	stop := destination.fxpWatch(ctx)
	host, port, err := destination.getDataConnPort()
	stop()
	if err != nil {
		return destination.fxpSideFail("destination", err)
	}
	ip := net.ParseIP(host)
	if ip == nil {
		return &fxpSideError{
			side: "destination",
			err:  fmt.Errorf("invalid passive endpoint host %q", host),
		}
	}

	// 2. Hand the endpoint to the source with EPRT (PORT fallback for IPv4).
	if err = c.fxpSendPort(ctx, ip, port); err != nil {
		return c.fxpSideFail("source", err)
	}

	// 3. STOR on the destination must be accepted before RETR is attempted.
	code, msg, err := destination.fxpCmd(ctx, "STOR %s", destinationPath)
	if err != nil {
		return destination.fxpSideFail("destination", err)
	}
	if code != StatusAlreadyOpen && code != StatusAboutToSend {
		// STOR rejected: nothing started, so no RETR and no ABOR anywhere.
		return &fxpSideError{
			side: "destination",
			err:  &textproto.Error{Code: code, Msg: msg},
		}
	}

	// 4. RETR on the source.
	code, msg, err = c.fxpCmd(ctx, "RETR %s", sourcePath)
	if err != nil {
		// The source reply never arrived intact: its boundary is unknown.
		_ = c.netConn.Close()
		copyErr := &CopyError{
			Source: &fxpSideError{side: "source", err: err},
		}
		if abortErr := destination.fxpAbort(ctx); abortErr != nil {
			copyErr.Destination = &fxpSideError{side: "destination", err: abortErr}
		}
		return copyErr
	}
	if code != StatusAlreadyOpen && code != StatusAboutToSend {
		// RETR rejected and its negative reply is already consumed: only the
		// destination side needs the ABOR sequence.
		copyErr := &CopyError{
			Source: &fxpSideError{
				side: "source",
				err:  &textproto.Error{Code: code, Msg: msg},
			},
		}
		if abortErr := destination.fxpAbort(ctx); abortErr != nil {
			copyErr.Destination = &fxpSideError{side: "destination", err: abortErr}
		}
		return copyErr
	}

	// 5. Both transfers accepted: read both final replies unconditionally.
	//    Negative finals are recorded, never retried and never followed by ABOR.
	_, _, srcErr := c.fxpReadFinal(ctx)
	_, _, dstErr := destination.fxpReadFinal(ctx)

	// A fully read reply (including a negative *textproto.Error) leaves the
	// control stream synchronized; anything else makes its boundary unknown.
	if srcErr != nil && !fxpIsProtocolError(srcErr) {
		_ = c.netConn.Close()
	}
	if dstErr != nil && !fxpIsProtocolError(dstErr) {
		_ = destination.netConn.Close()
	}

	if srcErr == nil && dstErr == nil {
		return nil
	}
	return &CopyError{Source: srcErr, Destination: dstErr}
}
