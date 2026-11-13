# goftp - an FTP client for golang

[![Build Status](https://travis-ci.org/secsy/goftp.svg)](https://travis-ci.org/secsy/goftp) [![GoDoc](https://godoc.org/github.com/secsy/goftp?status.svg)](https://godoc.org/github.com/secsy/goftp)

goftp aims to be a high-level FTP client that takes advantage of useful FTP features when supported by the server.

Here are some notable package highlights:

* Connection pooling for parallel transfers/traversal.
* Automatic resumption of interruped file transfers.
* Explicit and implicit FTPS support (TLS only, no SSL).
* IPv6 support.
* Reasonably good automated tests that run against pure-ftpd and proftpd.

Please see the godocs for details and examples.

Pull requests or feature requests are welcome, but in the case of the former, you better add tests.

### Tests ###

How to run tests (windows not supported):
* ```./build_test_server.sh``` from root goftp directory (this downloads and compiles pure-ftpd and proftpd)
* ```go test``` from the root goftp directory

### Data connection lifecycle ###

Every transfer (`Retrieve`, `Store`, `ReadDir`) prepares its data connection
before sending the transfer command: in passive mode the client dials the
server's data port, in active mode it listens locally and tells the server
where to connect. Those local resources follow a strict ownership protocol so
they cannot leak when a path is repeatedly denied:

* **Before the server accepts the command** — while the client has not yet
  received a `125`/`150` reply — a complete rejection such as `550` only
  invalidates the prepared data socket (passive) or listener (active). The
  client closes that resource immediately, leaves the control connection in
  sync, and returns it to the pool. It does not read a `226` that will never
  arrive and does not send `ABOR`; the next command reads its own reply.
* **After a `125`/`150` reply** — when the getter is invoked, ownership of
  the data connection transfers to the transfer logic, which closes it after
  the copy. If the active-mode accept fails, the data stream is interrupted,
  the final reply cannot be read, or the final reply is not a successful
  completion (2xx), the control connection is marked broken and evicted from
  the pool rather than reused: its reply boundary can no longer be trusted,
  and a late `226` must not be handed to the next user. A replacement
  connection is opened on demand, respecting `ConnectionsPerHost`. Successful
  transfers keep their existing completion and automatic-resume behavior.
* **No background accept** — active mode does not block a goroutine on
  `Accept` while the command is in flight; the accept happens once, inside the
  getter, after the `125`/`150` reply.
* **Cleanup is idempotent** — releasing a prepared connection twice cannot
  close a later transfer's socket. Closing a `RawConn` (or the whole
  `Client`) whose prepared getter was never called releases those resources
  automatically.

`Error` codes, `Temporary()`/`Timeout()` flags and successful return values
are unchanged; post-150 failures surface the data or reply error rather than
being converted into success by cleanup.
