# The most complete, best-tested WebDriver client for Go

[![GoDoc](https://godoc.org/github.com/tebeka/selenium?status.svg)](https://godoc.org/github.com/tebeka/selenium)
[![Travis](https://travis-ci.org/tebeka/selenium.svg?branch=master)](https://travis-ci.org/tebeka/selenium)
[![Go Report Card](https://goreportcard.com/badge/github.com/tebeka/selenium)](https://goreportcard.com/report/github.com/tebeka/selenium)

## About

This is a [WebDriver][selenium] client for [Go][go]. It supports the
[WebDriver protocol][webdriver] and has been tested with various versions of
[Selenium WebDriver][selenium], Firefox and [Geckodriver][geckodriver], and
Chrome and [ChromeDriver][chromedriver],

`selenium` is currently maintained by Eric Garrido ([@minusnine][minusnine]).

[selenium]: http://seleniumhq.org/
[webdriver]: https://www.w3.org/TR/webdriver/
[go]: http://golang.org/
[server]: http://seleniumhq.org/download/
[geckodriver]: https://github.com/mozilla/geckodriver
[chromedriver]: https://sites.google.com/a/chromium.org/chromedriver/
[minusnine]: http://github.com/minusnine

## Installing

Run

    go get -t -d github.com/tebeka/selenium

to fetch the package.

The package requires a working WebDriver installation, which can include recent
versions of a web browser being driven by Selenium WebDriver.

### Downloading Dependencies

We provide a means to download the ChromeDriver binary, the Firefox binary, the
Selenium WebDriver JARs, and the Sauce Connect proxy binary. This is primarily
intended for testing.

    $ cd vendor
    $ go run init.go --alsologtostderr  --download_browsers --download_latest
    $ cd ..

Re-run this periodically to get up-to-date versions of these binaries.

## Documentation

The API documentation is at https://godoc.org/github.com/tebeka/selenium. See [the example](https://github.com/tebeka/selenium/blob/master/example_test.go) and unit tests(for [sauce](https://github.com/tebeka/selenium/blob/master/sauce_test.go), [selenium](https://github.com/tebeka/selenium/blob/master/selenium_test.go) and [service](https://github.com/tebeka/selenium/blob/master/service_test.go)) for better usage information.

## Shadow DOM

Elements inside an open [shadow root](https://developer.mozilla.org/en-US/docs/Web/API/ShadowRoot)
cannot be found with the ordinary `FindElement` calls, and a shadow root is a
distinct search context from the page. This package supports shadow roots
through the W3C WebDriver endpoints directly - no JavaScript is injected, so
it also works when script execution is disabled.

The API consists of:

- `GetShadowRoot(elem WebElement) (ShadowRoot, error)` - returns the open
  shadow root attached to an element, using
  `POST /session/{id}/element/{id}/shadow`.
- `ShadowRoot.FindElement(by, value string) (WebElement, error)` and
  `ShadowRoot.FindElements(by, value string) ([]WebElement, error)` - search
  inside a root via `POST /session/{id}/shadow/{id}/element[s]`. They accept
  `ByCSSSelector`, `ByID` and `ByName`; ID and name values are translated to
  CSS selectors with every literal character escaped (quotes, backslashes,
  spaces and non-ASCII text are matched literally).
- `ResolveShadowPath(ctx, wd, steps, interval)` - walks a path of
  `ShadowStep{By, Value, EnterShadow}` values, entering a shadow root after a
  step when `EnterShadow` is true, and returns the element from the final
  step.

A shadow root reference and an element reference are different handles and
cannot be interchanged. Elements returned from a root are ordinary
`WebElement` values, so `SendKeys`, `GetAttribute` and another
`GetShadowRoot` for nested components all keep working. Shadow roots require a
W3C WebDriver session; a legacy JSON Wire Protocol session returns an error
rather than attempting a script-based fallback.

`ResolveShadowPath` handles components being rebuilt. If any step fails with
"no such element", "no such shadow root", "detached shadow root" or "stale
element reference", the whole path is re-resolved from the first step instead
of reusing stale intermediate handles; other protocol errors are returned
immediately. Retries share the deadline of the passed `context.Context` (no
new traversal starts after cancellation), the remote implicit timeout is left
untouched, and input actions are never replayed automatically.

```go
wd, err := selenium.NewRemote(selenium.Capabilities{"browserName": "chrome"}, "http://127.0.0.1:4444/wd/hub")
if err != nil {
    panic(err)
}
defer wd.Quit()

// Drive the endpoints directly through two nested components.
host, err := wd.FindElement(selenium.ByCSSSelector, "contact-card")
if err != nil {
    panic(err)
}
root, err := selenium.GetShadowRoot(host)
if err != nil {
    panic(err)
}
inner, err := root.FindElement(selenium.ByCSSSelector, "contact-form")
if err != nil {
    panic(err)
}
innerRoot, err := selenium.GetShadowRoot(inner)
if err != nil {
    panic(err)
}
field, err := innerRoot.FindElement(selenium.ByID, "email")
if err != nil {
    panic(err)
}
if err := field.SendKeys("ada@example.com"); err != nil {
    panic(err)
}

// Or resolve the same path as a whole, re-walking it from the first step if
// either component is rebuilt while the page settles.
steps := []selenium.ShadowStep{
    {By: selenium.ByCSSSelector, Value: "contact-card", EnterShadow: true},
    {By: selenium.ByCSSSelector, Value: "contact-form", EnterShadow: true},
    {By: selenium.ByID, Value: "email"},
}
ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
defer cancel()
field, err = selenium.ResolveShadowPath(ctx, wd, steps, 50*time.Millisecond)
if err != nil {
    panic(err)
}
```

## Known Issues

Any issues are usually because the underlying browser automation framework has a
bug or inconsistency. Where possible, we try to cover up these underlying
problems in the client, but sometimes workarounds require higher-level
intervention.

Please feel free to [file an issue][issue] if this client doesn't work as
expected.

[issue]: https://github.com/tebeka/selenium/issues/new

Below are known issues that affect the usage of this API. There are likely
others filed on the respective issue trackers.

### Selenium 2

No longer supported.

### Selenium 3

1.  [Selenium 3 NewSession does not implement the W3C-specified parameters](https://github.com/SeleniumHQ/selenium/issues/2827).

### Geckodriver (Standalone)

1.  [Geckodriver does not support the Log API](https://github.com/mozilla/geckodriver/issues/284)
    because it
    [hasn't been defined in the spec yet](https://github.com/w3c/webdriver/issues/406).
2.  Firefox via Geckodriver (and also through Selenium)
    [doesn't handle clicking on an element](https://github.com/mozilla/geckodriver/issues/1007).
3.  Firefox via Geckodriver doesn't handle sending control characters
    [without appending a terminating null key](https://github.com/mozilla/geckodriver/issues/665).

### Chromedriver

1. [Headless Chrome does not support running extensions](https://crbug.com/706008).

## Breaking Changes

There are a number of upcoming changes that break backward compatibility in an
effort to improve and adapt the existing API. They are listed here:

### 22 August 2017

The `Version` constant was removed as it is unused.

### 18 April 2017

The Log method was changed to accept a typed constant for the type of log to
retrieve, instead of a raw string. The return value was also changed to provide
a more idiomatic type.

## Hacking

Patches are encouraged through GitHub pull requests. Please ensure that:

1.  A test is added for anything more than a trivial change and that the
    existing tests pass. See below for instructions on setting up your test
    environment.
2.  Please ensure that `gofmt` has been run on the changed files before
    committing. Install a pre-commit hook with the following command:

    $ ln -s ../../misc/git/pre-commit .git/hooks/pre-commit

See [the issue tracker][issues] for features that need implementing.

[issues]: https://github.com/tebeka/selenium/issues

### Testing Locally

Install `xvfb` and Java if they is not already installed, e.g.:

    sudo apt-get install xvfb openjdk-11-jre

Run the tests:

    $ go test

*   There is one top-level test for each of:

    1.  Chromium and ChromeDriver.
    2.  A new version of Firefox and Selenium 3.
    3.  HTMLUnit, a Java-based lightweight headless browser implementation.
    4.  A new version of Firefox directly against Geckodriver.

    There are subtests that are shared between both top-level tests.

*   To run only one of the top-level tests, pass one of:

    *   `-test.run=TestFirefoxSelenium3`,
    *   `-test.run=TestFirefoxGeckoDriver`,
    *   `-test.run=TestHTMLUnit`, or
    *   `-test.run=TestChrome`.

    To run a specific subtest, pass `-test.run=Test<Browser>/<subtest>` as
    appropriate. This flag supports regular expressions.

*   If the Chrome or Firefox binaries, the Selenium JAR, the Geckodriver binary,
    or the ChromeDriver binary cannot be found, the corresponding tests will be
    skipped.

*   The binaries and JAR under test can be configured by passing flags to `go
    test`. See the available flags with `go test --arg --help`.

*   Add the argument `-test.v` to see detailed output from the test automation
    framework.

### Testing With Docker

To ensure hermeticity, we also have tests that run under Docker. You will need
an installed and running Docker system.

To run the tests under Docker, run:

    $ go test --docker

This will create a new Docker container and run the tests in it. (Note: flags
supplied to this invocation are not curried through to the `go test` invocation
within the Docker container).

For debugging Docker directly, run the following commands:

    $ docker build -t go-selenium testing/
    $ docker run --volume=$(pwd):/code --workdir=/code -it go-selenium bash
    root@6c7951e41db6:/code# testing/docker-test.sh
    ... lots of testing output ...

### Testing With Sauce Labs

Tests can be run using a browser located in the cloud via Sauce Labs.

To run the tests under Sauce, run:

    $ go test --test.run=TestSauce --test.timeout=20m \
      --experimental_enable_sauce \
      --sauce_user_name=[username goes here] \
      --sauce_access_key=[access key goes here]

The Sauce access key can be obtained via
[the Sauce Labs user settings page](https://saucelabs.com/beta/user-settings).

Test results can be viewed through the
[Sauce Labs Dashboard](https://saucelabs.com/beta/dashboard/tests).

## License

This project is licensed under the [MIT][mit] license.

[mit]: https://raw.githubusercontent.com/tebeka/selenium/master/LICENSE
