Agouti
======

[![Build Status](https://api.travis-ci.org/sclevine/agouti.png?branch=master)](http://travis-ci.org/sclevine/agouti)
[![GoDoc](https://godoc.org/github.com/sclevine/agouti?status.svg)](https://godoc.org/github.com/sclevine/agouti)

**NOTE: Agouti is no longer actively maintained. This was one of my earliest Go projects, and I am no longer happy with the API design. I recommend selecting an alternative WebDriver client for Go.**

Agouti is a library for writing browser-based acceptance tests in Google Go. It provides [Gomega](https://github.com/onsi/gomega) matchers and plays nicely with [Ginkgo](https://github.com/onsi/ginkgo) or [Spec](https://github.com/sclevine/spec). See [agouti.org](http://agouti.org) and the [GoDoc](https://godoc.org/github.com/sclevine/agouti) for documentation. Have questions? Check out the [Agouti mailing list](https://groups.google.com/d/forum/agouti) or the #agouti IRC channel on Freenode.

The [integration tests](https://github.com/sclevine/agouti/blob/master/internal/integration/) are a great place to see everything in action and get started quickly!

File uploads
------------

`Selection.UploadFile(filename)` and `Selection.UploadFiles(filenames ...)` fill
`<input type="file" />` elements. Two modes are available, controlled by the
`RemoteFileUpload(enabled bool)` Option (disabled by default; accepted by
`NewPage`, `JoinPage`, and as a WebDriver default that each page may override
for itself):

* **Local mode (default):** the client-side absolute path of each file is sent
  to the browser directly. This only works when the browser runs on the same
  machine as the client. `UploadFile` keeps its historical behavior and does
  not check whether the file exists on the client.
* **Remote mode (`RemoteFileUpload(true)`):** each file is transmitted to the
  WebDriver server first (`POST /session/<id>/se/file`, one base64-encoded
  single-entry ZIP archive per file), and the file inputs are filled with the
  newline-joined server-side paths. Use this when driving a remote Selenium
  Grid. Remote paths are never cached: every call re-transmits every file.

`UploadFiles` (and `UploadFile` in remote mode) validates everything before
touching the page: the selection must consist entirely of
`<input type="file" />` elements (each with a `multiple` attribute when
several files are given), and every file must be a readable regular file. If
any check fails, no element is modified and nothing is transmitted.

Failures are reported by stage:

* **Staging failures** (a transfer fails, or the server returns an invalid
  remote path): no element is filled, the error identifies the file position,
  and files already staged on the server are left in place.
* **Write failures** (filling an element fails after all files were staged):
  elements filled so far keep their new values, remaining elements are left
  untouched, and the error identifies the element position.

<p align="center"><a href=http://agouti.org><img src="http://agouti.org/images/agouti_small.png" /></a></p>
