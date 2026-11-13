// Package api provides a generic, low-level WebDriver API client for Go.
// All methods map directly to endpoints of the WebDriver Wire Protocol:
// https://code.google.com/p/selenium/wiki/JsonWireProtocol
//
// Sessions use the legacy JSON Wire Protocol by default. Use
// OpenW3CWithClient (or NewW3CWithClient for an existing session) to create a
// W3C WebDriver session. The dialect is fixed per session; commands never
// switch protocols after a failure. The form-flow operations migrated to W3C
// are Execute, GetActiveElement, GetWindow, GetWindows, SetWindow, the three
// timeout setters, GetAlertText, AcceptAlert and DismissAlert. All other
// methods, and direct Send calls, keep their original JSON Wire endpoints.
//
// This package was previously internal to the agouti package. It currently
// does not have a fixed API, but this will change in the near future
// (with the addition of adequate documentation).
package api
