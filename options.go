package agouti

import (
	"net/http"
	"time"
)

type config struct {
	Timeout             time.Duration
	DesiredCapabilities Capabilities
	BrowserName         string
	RejectInvalidSSL    bool
	Debug               bool
	HTTPClient          *http.Client
	ChromeOptions       map[string]interface{}
	w3c                 *bool
}

// An Option specifies configuration for a new WebDriver or Page.
type Option func(*config)

// W3C provides an Option for selecting the wire protocol dialect used by the
// session backing a Page. Passing true requests the W3C WebDriver protocol;
// passing false explicitly selects the legacy JSON Wire Protocol (the
// default). The dialect is chosen when the session is created or joined and
// never changes for that session.
func W3C(enabled bool) Option {
	return func(c *config) {
		value := enabled
		c.w3c = &value
	}
}

// Browser provides an Option for specifying a browser.
func Browser(name string) Option {
	return func(c *config) {
		c.BrowserName = name
	}
}

// Timeout provides an Option for specifying a timeout in seconds.
func Timeout(seconds int) Option {
	return func(c *config) {
		c.Timeout = time.Duration(seconds) * time.Second
	}
}

// ChromeOptions is used to pass additional options to Chrome via ChromeDriver.
func ChromeOptions(opt string, value interface{}) Option {
	return func(c *config) {
		if c.ChromeOptions == nil {
			c.ChromeOptions = make(map[string]interface{})
		}
		c.ChromeOptions[opt] = value
	}
}

// Desired provides an Option for specifying desired WebDriver Capabilities.
func Desired(capabilities Capabilities) Option {
	return func(c *config) {
		c.DesiredCapabilities = capabilities
	}
}

// RejectInvalidSSL is an Option specifying that the WebDriver should reject
// invalid SSL certificates. All WebDrivers should accept invalid SSL certificates
// by default. See: http://www.w3.org/TR/webdriver/#invalid-ssl-certificates
var RejectInvalidSSL Option = func(c *config) {
	c.RejectInvalidSSL = true
}

// Debug is an Option that connects the running WebDriver to stdout and stdin.
var Debug Option = func(c *config) {
	c.Debug = true
}

// HTTPClient provides an Option for specifying a *http.Client
func HTTPClient(client *http.Client) Option {
	return func(c *config) {
		c.HTTPClient = client
	}
}

func (c config) Merge(options []Option) *config {
	for _, option := range options {
		option(&c)
	}
	return &c
}

// IsW3C reports whether W3C mode was explicitly requested. It defaults to
// false, which selects the legacy JSON Wire Protocol.
func (c *config) IsW3C() bool {
	return c.w3c != nil && *c.w3c
}

func (c *config) Capabilities() Capabilities {
	merged := Capabilities{"acceptSslCerts": true}
	for feature, value := range c.DesiredCapabilities {
		merged[feature] = value
	}
	if c.BrowserName != "" {
		merged.Browser(c.BrowserName)
	}
	if c.ChromeOptions != nil {
		merged["chromeOptions"] = c.ChromeOptions
	}
	if c.RejectInvalidSSL {
		merged.Without("acceptSslCerts")
	}
	return merged
}
