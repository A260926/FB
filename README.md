# goftp #

[![Units tests](https://github.com/jlaffaye/ftp/actions/workflows/unit_tests.yaml/badge.svg)](https://github.com/jlaffaye/ftp/actions/workflows/unit_tests.yaml)
[![Coverage Status](https://coveralls.io/repos/jlaffaye/ftp/badge.svg?branch=master&service=github)](https://coveralls.io/github/jlaffaye/ftp?branch=master)
[![golangci-lint](https://github.com/jlaffaye/ftp/actions/workflows/golangci-lint.yaml/badge.svg)](https://github.com/jlaffaye/ftp/actions/workflows/golangci-lint.yaml)
[![Go ReportCard](https://goreportcard.com/badge/jlaffaye/ftp)](http://goreportcard.com/report/jlaffaye/ftp)
[![Go Reference](https://pkg.go.dev/badge/github.com/jlaffaye/ftp.svg)](https://pkg.go.dev/github.com/jlaffaye/ftp)

A FTP client package for Go

## Install ##

```
go get -u github.com/jlaffaye/ftp
```

## Documentation ##

https://pkg.go.dev/github.com/jlaffaye/ftp

## Example ##

```go
c, err := ftp.Dial("ftp.example.org:21", ftp.DialWithTimeout(5*time.Second))
if err != nil {
    log.Fatal(err)
}

err = c.Login("anonymous", "anonymous")
if err != nil {
    log.Fatal(err)
}

// Do something with the FTP conn

if err := c.Quit(); err != nil {
    log.Fatal(err)
}
```

## Store a file example ##

```go
data := bytes.NewBufferString("Hello World")
err = c.Stor("test-file.txt", data)
if err != nil {
	panic(err)
}
```

## Read a file example ##

```go
r, err := c.Retr("test-file.txt")
if err != nil {
	panic(err)
}
defer r.Close()

buf, err := ioutil.ReadAll(r)
println(string(buf))
```

## Server-to-server transfer (FXP) ##

`CopyTo` moves a file directly from one FTP server to another (FXP, RFC 959
server-to-server transfer). The file content never passes through this
process: only the two control connections are used and no local data
connection is opened or read. Both connections must already be dialed and
logged in. The receiver is the source connection; the argument is the
destination.

```go
source, err := ftp.Dial("ftp-source.example.org:21")
if err != nil {
	log.Fatal(err)
}
defer source.Quit()
if err := source.Login("anonymous", "anonymous"); err != nil {
	log.Fatal(err)
}

dest, err := ftp.Dial("ftp-archive.example.org:21")
if err != nil {
	log.Fatal(err)
}
defer dest.Quit()
if err := dest.Login("archive", "secret"); err != nil {
	log.Fatal(err)
}

// Copies /incoming/big.bin on the source to /2026/big.bin on the archive
// server without downloading the file body locally.
if err := source.CopyTo(context.Background(), dest, "/incoming/big.bin", "/2026/big.bin"); err != nil {
	var copyErr *ftp.CopyError
	if errors.As(err, &copyErr) {
		// copyErr.Source and copyErr.Destination hold the per-side errors,
		// each preserving the original *textproto.Error (e.g. 550, 426).
		log.Printf("source error: %v", copyErr.Source)
		log.Printf("destination error: %v", copyErr.Destination)
	}
	log.Fatal(err)
}
```

Only plaintext sessions without a `PRET` requirement are supported; TLS
(implicit or explicit) and PRET are rejected before any command is sent.
`ServerConn` remains non-reentrant: while `CopyTo` runs, both connections are
busy until it returns. There is no automatic retry, checksum verification,
resume support or local temporary file.

The destination is put in passive mode using the existing EPSV/PASV selection
and PASV address-trust rules; the source is directed to that endpoint with
`EPRT` (address family 1 for IPv4, 2 for IPv6), falling back to the legacy
`PORT` command only for IPv4 and only when `EPRT` is rejected with 500, 502 or
504. `STOR` is issued on the destination and accepted (125/150) before `RETR`
is issued on the source. Each side's final reply (226 or 250) is always read,
even when the other side fails; negative final replies are reported per side
in `CopyError` and never trigger a retry or an `ABOR`.

### When a session can be reused ###

A control connection stays usable (for example for another `CopyTo`, `NoOp` or
any other command) as long as a complete reply was consumed and the control
stream boundary is still known:

- After a fully successful transfer, both sessions are synchronized and reusable.
- After a fully read negative reply, such as a rejected `STOR`/`RETR` or a
  negative final reply (550, 426, ...), the session is reusable because the
  reply has been completely consumed.
- When a transfer that was accepted on the destination is aborted because the
  source rejected `RETR`, the destination stays reusable only if its `ABOR`
  acknowledgement sequence is fully recognized (see below); otherwise the
  destination session is closed.

The affected session is closed and must not be reused on context cancellation
or timeout, an I/O error, or any reply sequence whose boundary cannot be
confirmed. A wait for a control reply is interrupted directly by the context
(no polling); internal reader, deadline and cancellation state are always
cleaned up before `CopyTo` returns, so no stale deadline is left for the next
command.

### ABOR compatibility ###

When `RETR` is rejected after `STOR` was already accepted, an `ABOR` is sent to
the destination only and its complete acknowledgement sequence is consumed.
Compatible peers respond with one of:

- a single `225`;
- `426` followed by `226`;
- an already-queued transfer completion `226`, followed by the `ABOR` reply `225`.

A standalone `226` cannot be attributed to `ABOR` (it may be the previous
transfer's completion), so the client keeps reading until the following `225`.
If that `225` does not arrive, or any other sequence is seen, the boundary
cannot be confirmed and the destination session is closed rather than risking
that a later command reads a reply belonging to the transfer. The source is
never sent an `ABOR` in this case because its rejecting reply was already read.

