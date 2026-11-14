This project is not maintained and it is effectively abandoned, if you have interest in this code you should probably clone it.

webdriver
=========

The package implements a WebDriver that communicate with a browser using the JSON Wire Protocol (See https://code.google.com/p/selenium/wiki/JsonWireProtocol).
This is a pure go library and doesn't require a running Selenium driver. It currently supports Firefox (using the WebDriver extension) and Chrome (using the standalone server chromedriver). It should be fairly easy to add other browser that directly implement the wire protocol.

**Version: 0.1**  
Tests are partial and have been run only on Linux (with firefox webdriver 2.32.0 and chromedriver 2.1).

**Install:**  
$ go get github.com/fedesog/webdriver

**Requires:**
* chromedriver (for chrome):  
https://code.google.com/p/chromedriver/  
* webdriver.xpi (for firefox): that is founds in the selenium-server-standalone file  
https://code.google.com/p/selenium/


Binding form fields to controls:
---------------------------------

Legacy back-office forms usually have no reliable element ids. The pattern is
to locate a field by its visible label and then take the input control to its
right. Doing this field by field breaks when the candidate regions overlap:
two fields can end up bound to the same input, and the first field can steal
the only control the second field can use.

`BindFields` produces the whole binding plan in one call. It returns the
ordinary `WebElement` values, in slot order, so the caller can keep using
`SendKeys` exactly as before. The binding itself only performs element
lookups plus displayed/enabled checks and `GetLocation`/`Size` reads: it
never fills, clicks, scrolls (`GetLocationInView` is not used) or executes
JavaScript.

* `Locator{Using, Value}` is the same strategy/value pair accepted by
  `FindElement` and `FindElements`.
* `FieldSlot{Name, Label, Candidates}` describes one field. `Label` must
  resolve to exactly one displayed element whose rectangle has positive
  area. `Candidates` may resolve to many elements; only displayed, enabled,
  positive-area controls take part.
* A label/control edge exists when the control starts at or after the
  label's right edge, their vertical intervals overlap with positive length
  (boundaries merely touching do not count) and the horizontal gap is at
  most `maxGap`.
* Edge cost is `2*gap + abs(vertical center offset)`, with every coordinate
  operation checked for `int64` overflow (negative document coordinates are
  legal).
* The plan is the minimum total-cost one-to-one assignment (Hungarian
  algorithm; permutations are never enumerated, and 48x48 dense graphs are
  handled comfortably). Equal-cost plans break to the lexicographically
  smallest assignment vector. If no complete matching exists, the call
  returns an error and a nil plan — a half plan is never delivered.
* A remote element is identified by session id plus element id, never by Go
  pointer identity. Within one call its visibility, enabled state and
  rectangle are sampled at most once even if the id appears in several
  lists; nothing is retained across calls.
* `Session.BindFields` searches from the document root.
  `WebElement.BindFields` runs every lookup through the receiver element's
  descendant endpoints and never falls back to the document root.
* Empty or duplicate slot `Name` values and a negative `maxGap` are rejected
  before any request is sent. An empty slot list returns a non-nil empty
  slice. Failures come back as `*BindError` carrying the field `Name`, a
  stage name and the wrapped original protocol error.

Example (data entry on an old, id-less form):

    chromeDriver := webdriver.NewChromeDriver("/path/to/chromedriver")
    if err := chromeDriver.Start(); err != nil {
        log.Fatal(err)
    }
    defer chromeDriver.Stop()
    session, err := chromeDriver.NewSession(webdriver.Capabilities{}, nil)
    if err != nil {
        log.Fatal(err)
    }
    if err := session.Url("http://legacy.example/entry"); err != nil {
        log.Fatal(err)
    }

    slots := []webdriver.FieldSlot{
        {
            Name:       "customer",
            Label:      webdriver.Locator{Using: webdriver.XPath, Value: `//label[.="Customer"]`},
            Candidates: webdriver.Locator{Using: webdriver.CSS_Selector, Value: `input[name^="field"]`},
        },
        {
            Name:       "city",
            Label:      webdriver.Locator{Using: webdriver.XPath, Value: `//label[.="City"]`},
            Candidates: webdriver.Locator{Using: webdriver.CSS_Selector, Value: `input[name^="field"]`},
        },
    }

    // One plan for the whole form; no clicks, typing or scrolling happens here.
    controls, err := session.BindFields(slots, 50)
    if err != nil {
        log.Fatal(err)
    }
    values := []string{"Acme Ltd", "Ravenna"}
    for i, control := range controls {
        if err := control.SendKeys(values[i]); err != nil {
            log.Fatal(err)
        }
    }

To restrict every lookup to one container, resolve the container first and
call `BindFields` on the element:

    card, err := session.FindElement(webdriver.ID, "customer-card")
    if err != nil {
        log.Fatal(err)
    }
    controls, err := card.BindFields(slots, 50)


Example:
--------

    chromeDriver := webdriver.NewChromeDriver("/path/to/chromedriver")
    err := chromeDriver.Start()
    if err != nil {
    	log.Println(err)
    }
    desired := webdriver.Capabilities{"Platform": "Linux"}
    required := webdriver.Capabilities{}
    session, err := chromeDriver.NewSession(desired, required)
    if err != nil {
    	log.Println(err)
    }
    err = session.Url("http://golang.org")
    if err != nil {
    	log.Println(err)
    }
    time.Sleep(10 * time.Second)
    session.Delete()
    chromeDriver.Stop()

