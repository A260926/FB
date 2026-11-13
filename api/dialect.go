package api

// Dialect identifies the WebDriver wire protocol used by a Session.
//
// JSONWire is the legacy Selenium/JSON Wire Protocol. W3C is the
// W3C WebDriver protocol. A Session's dialect is fixed when the
// session is created or joined; commands never switch dialects.
type Dialect int

const (
	// JSONWire selects the legacy JSON Wire Protocol (the default).
	JSONWire Dialect = iota
	// W3C selects the W3C WebDriver protocol.
	W3C
)
