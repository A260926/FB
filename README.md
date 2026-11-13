Agouti
======

[![Build Status](https://api.travis-ci.org/sclevine/agouti.png?branch=master)](http://travis-ci.org/sclevine/agouti)
[![GoDoc](https://godoc.org/github.com/sclevine/agouti?status.svg)](https://godoc.org/github.com/sclevine/agouti)

**NOTE: Agouti is no longer actively maintained. This was one of my earliest Go projects, and I am no longer happy with the API design. I recommend selecting an alternative WebDriver client for Go.**

Agouti is a library for writing browser-based acceptance tests in Google Go. It provides [Gomega](https://github.com/onsi/gomega) matchers and plays nicely with [Ginkgo](https://github.com/onsi/ginkgo) or [Spec](https://github.com/sclevine/spec). See [agouti.org](http://agouti.org) and the [GoDoc](https://godoc.org/github.com/sclevine/agouti) for documentation. Have questions? Check out the [Agouti mailing list](https://groups.google.com/d/forum/agouti) or the #agouti IRC channel on Freenode.

The [integration tests](https://github.com/sclevine/agouti/blob/master/internal/integration/) are a great place to see everything in action and get started quickly!

<p align="center"><a href=http://agouti.org><img src="http://agouti.org/images/agouti_small.png" /></a></p>

## WebDriver protocols (JSON Wire vs W3C)

Agouti speaks the legacy JSON Wire Protocol by default. A page can instead
open (or join) a W3C WebDriver session with the `W3C` Option; the dialect is
chosen per session and is never inferred from a failed command:

```go
page, err := agouti.NewPage("http://127.0.0.1:9515", agouti.W3C(true))
```

`WebDriver.NewPage` inherits a default `W3C` Option from `NewWebDriver` and
allows a per-page override. `agouti.W3C(false)` explicitly selects the legacy
dialect, including for `agouti.JoinPage`. At the API layer, use
`api.OpenW3CWithClient(url, capabilities, client)` (or
`api.WebDriver.OpenW3C`) for a new W3C session and
`api.NewW3CWithClient(sessionURL, client)` for an existing one.

W3C handshakes send exactly one `capabilities.alwaysMatch` object with
`firstMatch: [{}]` and no `desiredCapabilities`. The legacy `acceptSslCerts`
and `chromeOptions` capability names are translated to `acceptInsecureCerts`
and `goog:chromeOptions`; a caller-supplied standard key takes precedence and
the caller's map is never modified.

The form flow operations below have been migrated for W3C sessions (legacy
paths, methods and bodies are unchanged):

| Operation | Legacy endpoint | W3C endpoint |
| --- | --- | --- |
| `Session.Execute` / `Page.RunScript` | `POST execute` | `POST execute/sync` |
| `Session.GetActiveElement` | `POST element/active` | `GET element/active` |
| `Session.GetWindow` | `GET window_handle` | `GET window` |
| `Session.GetWindows` | `GET window_handles` | `GET window/handles` |
| `Session.SetWindow` | `POST window` (`name`) | `POST window` (`handle`) |
| `Session.SetImplicitWait` | `POST timeouts/implicit_wait` | `POST timeouts` (`implicit`) |
| `Session.SetPageLoad` | `POST timeouts` (`ms`, `type`) | `POST timeouts` (`pageLoad`) |
| `Session.SetScriptTimeout` | `POST timeouts/async_script` | `POST timeouts` (`script`) |
| `Session.GetAlertText` / `Page.PopupText` | `GET alert_text` | `GET alert/text` |
| `Session.AcceptAlert` / `Page.ConfirmPopup` | `POST accept_alert` | `POST alert/accept` |
| `Session.DismissAlert` / `Page.CancelPopup` | `POST dismiss_alert` | `POST alert/dismiss` |

Other commands (cookies, navigation, element commands, frames, mouse/touch,
logs and `SetAlertText`) are still sent to their original endpoints; direct
`Session.Send` calls always go to the endpoint the caller provides.

Failed protocol calls return `*api.ProtocolError` (with `HTTPStatus`, `Code`,
`Message` and `Stacktrace`), which `errors.As` can extract from the errors
returned by `Page` methods. W3C error envelopes are read from
`value.error`/`value.message`/`value.stacktrace`; a legacy response with a
non-zero `status` is an error even when the HTTP status is 200. Commands with
side effects (scripts and popup confirmation) are never retried on error.

### W3C form example

```go
package main

import (
	"errors"
	"fmt"
	"log"

	"github.com/sclevine/agouti"
	"github.com/sclevine/agouti/api"
)

func fillForm() error {
	driver := agouti.ChromeDriver()
	if err := driver.Start(); err != nil {
		return err
	}
	defer driver.Stop()

	page, err := driver.NewPage(agouti.W3C(true), agouti.Browser("chrome"))
	if err != nil {
		return err
	}

	if err := page.Navigate("https://forms.invalid/contacts"); err != nil {
		return err
	}
	if err := page.SetImplicitWait(500); err != nil {
		return err
	}
	if err := page.FindByID("email").Fill("person@example.com"); err != nil {
		return err
	}
	if err := page.FindByID("name").Fill("Example Person"); err != nil {
		return err
	}
	if err := page.FindByID("submit").Click(); err != nil {
		return err
	}

	var title string
	if err := page.RunScript("return document.title", nil, &title); err != nil {
		return err
	}
	fmt.Println("submitted:", title)

	if page.Session().Dialect() != api.W3C {
		return errors.New("expected a W3C session")
	}
	return nil
}

func main() {
	if err := fillForm(); err != nil {
		var protocolError *api.ProtocolError
		if errors.As(err, &protocolError) {
			log.Printf("protocol error %d %s: %s",
				protocolError.HTTPStatus, protocolError.Code, protocolError.Message)
		}
		log.Fatal(err)
	}
}
```

The same flow runs without a driver or browser when the page is built with
an in-memory `RoundTripper`:

```go
page, err := agouti.NewPage("http://driver.invalid",
	agouti.W3C(true),
	agouti.HTTPClient(&http.Client{Transport: transport}))
```


### Dialect test

`dialect_test.go` runs the real `Page`, `Session` and HTTP codec against an
in-memory `RoundTripper`, so no driver or browser is installed. Run it (and
the legacy baseline) with:

```
bash support/run.sh target
```

