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

## Walk a directory tree, skipping inaccessible branches ##

Some servers list subdirectories that then reject `LIST`/`MLSD` with a
`550` reply. `Walk` stops on the first such error; `WalkWithOptions` lets an
`OnError` callback decide whether to skip the failed branch and keep
discovering the sibling entries that were already returned by the parent
listing.

Only a plain `textproto.Error` with status code `550` can be skipped. Any
other failure (for example a `421` reply, a broken connection or a data
transfer error) stops the walk even if the callback returns `true`; a `550`
joined with another error is not skippable either. When the callback returns
`true`, `Err` ends up `nil` and the callback is responsible for recording the
failure; returning `false` (or passing `nil` options) keeps the legacy
behavior of `Walk`.

```go
var skippedBranches []string

w := c.WalkWithOptions("/", &ftp.WalkOptions{
    OnError: func(dir string, err error) bool {
        var tpErr *textproto.Error
        if errors.As(err, &tpErr) && tpErr.Code == ftp.StatusFileUnavailable {
            // Record the failed branch and continue the walk with its
            // already queued siblings instead of stopping.
            skippedBranches = append(skippedBranches, dir)
            return true
        }
        // Any other error is not recoverable: stop and expose it via Err.
        return false
    },
})

for w.Next() {
    fmt.Println(w.Path(), w.Stat().Type)
}
if err := w.Err(); err != nil {
    log.Fatal(err)
}
```

A `ServerConn` still supports only one in-flight data connection and is not
safe for concurrent use; walk options do not change that. Everything,
including commands issued from the `OnError` callback, runs on the single
control connection, and the traversal remains depth-first in the server's
listing order, without sorting, concurrency or symlink following.

