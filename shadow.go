// Shadow DOM support for the remote WebDriver client.
//
// The W3C WebDriver specification exposes an open shadow root attached to an
// element through three endpoints:
//
//   - POST /session/{id}/element/{id}/shadow        - get the shadow root
//   - POST /session/{id}/shadow/{id}/element        - find one element in a root
//   - POST /session/{id}/shadow/{id}/elements       - find elements in a root
//
// This file implements those endpoints directly, without injecting
// JavaScript, so shadow roots work even when script execution is disabled.
package selenium

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

const (
	// shadowRootIdentifier is the W3C WebDriver map key whose value uniquely
	// identifies a shadow root.
	shadowRootIdentifier = "shadow-6066-11e4-a52e-4f735466cecf"

	// errLegacyShadow is returned when the remote session predates the W3C
	// shadow root commands. No JavaScript fallback is attempted.
	errLegacyShadow = "shadow root access requires a W3C WebDriver session"
)

// ShadowRoot is a search context scoped to the open shadow root of a web
// element. Instances are obtained with GetShadowRoot and are valid until the
// hosting component is rebuilt or the element goes stale.
//
// A ShadowRoot is not a WebElement: root references and element references are
// distinct handles and cannot be interchanged. Elements returned by the find
// methods are ordinary WebElement values and support the full WebElement API,
// including GetShadowRoot for nested components.
type ShadowRoot interface {
	// FindElement finds exactly one element within this shadow root.
	FindElement(by, value string) (WebElement, error)
	// FindElements finds all matching elements within this shadow root.
	FindElements(by, value string) ([]WebElement, error)
}

// remoteSR is the remote implementation of ShadowRoot.
type remoteSR struct {
	parent *remoteWD
	id     string
}

// shadowRootIDFromValue extracts a shadow root reference from a decoded
// "value" object. It returns an empty string when the object does not carry a
// usable shadow root handle, including when it carries an element handle
// instead or both handles at once.
func shadowRootIDFromValue(v map[string]string) string {
	_, hasElement := v[webElementIdentifier]
	root, hasRoot := v[shadowRootIdentifier]
	if hasElement || !hasRoot || root == "" {
		return ""
	}
	return root
}

// decodeShadowRoot decodes a single shadow root response. It refuses any
// payload that does not contain exactly one, non-empty shadow root reference.
func (wd *remoteWD) decodeShadowRoot(data []byte) (ShadowRoot, error) {
	reply := new(struct {
		Value map[string]string `json:"value"`
	})
	if err := json.Unmarshal(data, reply); err != nil {
		return nil, err
	}
	id := shadowRootIDFromValue(reply.Value)
	if id == "" {
		return nil, fmt.Errorf("invalid shadow root returned: %+v", reply.Value)
	}
	return &remoteSR{parent: wd, id: id}, nil
}

// GetShadowRoot returns the open shadow root attached to elem.
//
// It issues the standard element/{id}/shadow command and never falls back to
// executing JavaScript. Legacy JSON Wire Protocol sessions return an error,
// as they have no shadow root command. A nil, foreign, or otherwise invalid
// element argument also returns an error.
func GetShadowRoot(elem WebElement) (ShadowRoot, error) {
	re, ok := elem.(*remoteWE)
	if !ok || re == nil {
		return nil, fmt.Errorf("GetShadowRoot requires a WebElement returned by this WebDriver client, got %T", elem)
	}
	wd := re.parent
	if !wd.w3cCompatible {
		return nil, errors.New(errLegacyShadow)
	}

	url := wd.requestURL("/session/%s/element/%s/shadow", wd.id, re.id)
	response, err := wd.execute("POST", url, nil)
	if err != nil {
		return nil, err
	}
	return wd.decodeShadowRoot(response)
}

// shadowFindRequest is the request body for the shadow element commands.
type shadowFindRequest struct {
	Using string `json:"using"`
	Value string `json:"value"`
}

// cssEscape escapes a string so it can be embedded literally in a CSS
// selector. The implementation follows CSS.escape(): every character except
// unreserved ASCII is emitted as a six-digit hexadecimal escape, and a leading
// digit is escaped so identifiers cannot start as a number.
func cssEscape(s string) string {
	var b strings.Builder
	for i, r := range s {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '-', r == '_':
			if i == 0 && r >= '0' && r <= '9' {
				fmt.Fprintf(&b, "\\%06x", r)
				continue
			}
			b.WriteRune(r)
		default:
			fmt.Fprintf(&b, "\\%06x", r)
		}
	}
	return b.String()
}

// shadowLocator converts a supported locator strategy into the (using, value)
// pair sent over the wire. Shadow roots only support CSS, ID and name
// strategies; ID and name are translated to CSS selectors with every literal
// character escaped, so quotes, backslashes, spaces and non-ASCII text have no
// special meaning.
func shadowLocator(by, value string) (string, string, error) {
	switch by {
	case ByCSSSelector:
		return ByCSSSelector, value, nil
	case ByID:
		return ByCSSSelector, "#" + cssEscape(value), nil
	case ByName:
		return ByCSSSelector, `[name=` + cssEscape(value) + "]", nil
	default:
		return "", "", fmt.Errorf("shadow root searches support %q, %q and %q, got %q",
			ByCSSSelector, ByID, ByName, by)
	}
}

// find performs a search scoped to this shadow root.
func (root *remoteSR) find(by, value, suffix string) (json.RawMessage, error) {
	wd := root.parent
	if !wd.w3cCompatible {
		return nil, errors.New(errLegacyShadow)
	}
	using, selector, err := shadowLocator(by, value)
	if err != nil {
		return nil, err
	}
	data, err := json.Marshal(shadowFindRequest{Using: using, Value: selector})
	if err != nil {
		return nil, err
	}
	url := wd.requestURL("/session/%s/shadow/%s/element"+suffix, wd.id, root.id)
	return wd.execute("POST", url, data)
}

// decodeShadowElement decodes an element returned by a shadow-root search. In
// addition to the ordinary element rules, it rejects a payload that also
// carries a shadow root reference, since element and root handles must never
// be mixed.
func (wd *remoteWD) decodeShadowElement(data []byte) (WebElement, error) {
	reply := new(struct {
		Value map[string]string `json:"value"`
	})
	if err := json.Unmarshal(data, reply); err != nil {
		return nil, err
	}
	if _, mixed := reply.Value[shadowRootIdentifier]; mixed {
		return nil, fmt.Errorf("invalid element returned: response also carries a shadow root reference: %+v", reply.Value)
	}
	id := elementIDFromValue(reply.Value)
	if id == "" {
		return nil, fmt.Errorf("invalid element returned: %+v", reply.Value)
	}
	return &remoteWE{parent: wd, id: id}, nil
}

// decodeShadowElements decodes a list of elements returned by a shadow-root
// search. Every entry is validated before anything is returned, so a malformed
// reference never produces a partially populated result.
func (wd *remoteWD) decodeShadowElements(data []byte) ([]WebElement, error) {
	reply := new(struct {
		Value []map[string]string `json:"value"`
	})
	if err := json.Unmarshal(data, reply); err != nil {
		return nil, err
	}
	elems := make([]WebElement, 0, len(reply.Value))
	for _, entry := range reply.Value {
		if _, mixed := entry[shadowRootIdentifier]; mixed {
			return nil, fmt.Errorf("invalid element returned: response also carries a shadow root reference: %+v", entry)
		}
		id := elementIDFromValue(entry)
		if id == "" {
			return nil, fmt.Errorf("invalid element returned: %+v", entry)
		}
		elems = append(elems, &remoteWE{parent: wd, id: id})
	}
	return elems, nil
}

// FindElement finds exactly one element within this shadow root.
func (root *remoteSR) FindElement(by, value string) (WebElement, error) {
	response, err := root.find(by, value, "")
	if err != nil {
		return nil, err
	}
	return root.parent.decodeShadowElement(response)
}

// FindElements finds all matching elements within this shadow root. The whole
// list is decoded before it is returned, so a malformed reference never
// produces a partially populated result.
func (root *remoteSR) FindElements(by, value string) ([]WebElement, error) {
	response, err := root.find(by, value, "s")
	if err != nil {
		return nil, err
	}
	return root.parent.decodeShadowElements(response)
}

// ShadowStep is one step of a path through a page and its shadow roots.
type ShadowStep struct {
	// By is the locator strategy, e.g. ByCSSSelector, ByID or ByName.
	By string
	// Value is the locator value. It is always treated literally.
	Value string
	// EnterShadow reports whether the next step must be performed inside the
	// shadow root of the element found by this step. The final step must not
	// set it.
	EnterShadow bool
}

// retryableShadowErrors are the protocol errors that indicate a path handle no
// longer points at a live node. When any of them occurs, the whole path is
// re-resolved from the first step rather than reusing stale intermediates.
var retryableShadowErrors = map[string]bool{
	"no such element":         true,
	"no such shadow root":     true,
	"detached shadow root":    true,
	"stale element reference": true,
}

// validateShadowPath checks the path before any request is issued.
func validateShadowPath(steps []ShadowStep, interval time.Duration) error {
	if len(steps) == 0 {
		return errors.New("ResolveShadowPath requires at least one step")
	}
	if steps[len(steps)-1].EnterShadow {
		return errors.New("the final ShadowStep must locate an element, not enter a shadow root")
	}
	if interval <= 0 {
		return fmt.Errorf("ResolveShadowPath interval must be greater than zero, got %v", interval)
	}
	return nil
}

// elementSearcher is the common search surface of the page-level WebDriver
// and a shadow-root search context.
type elementSearcher interface {
	FindElement(by, value string) (WebElement, error)
	FindElements(by, value string) ([]WebElement, error)
}

// resolveOnce walks the complete path from the page root. It builds fresh
// handles on every call, so handles from a previous, failed traversal are
// never reused.
func resolveOnce(wd WebDriver, steps []ShadowStep) (WebElement, error) {
	// The active search context: the page until a shadow root is entered.
	var scope elementSearcher = wd
	var elem WebElement
	for _, step := range steps {
		var err error
		elem, err = scope.FindElement(step.By, step.Value)
		if err != nil {
			return nil, err
		}
		if step.EnterShadow {
			root, err := GetShadowRoot(elem)
			if err != nil {
				return nil, err
			}
			scope = root
			elem = nil
		}
	}
	return elem, nil
}

// isRetryableShadowError reports whether err is one of the stale-reference
// protocol errors that justify re-resolving the path from the first step.
func isRetryableShadowError(err error) bool {
	var se *Error
	if errors.As(err, &se) {
		return retryableShadowErrors[se.Err]
	}
	return false
}

// ResolveShadowPath locates an element reached through steps, crossing open
// shadow roots where a step sets EnterShadow. The first step is searched in
// the page; when a step has EnterShadow set, the following step is searched
// inside that element's shadow root, and shadow roots may be nested. The
// final step always returns a WebElement.
//
// If any step fails with "no such element", "no such shadow root",
// "detached shadow root" or "stale element reference" - for example because a
// web component was rebuilt - the entire path is re-resolved from the first
// step, discarding every intermediate handle from the failed traversal. Any
// other protocol error is returned immediately.
//
// The retry loop shares the deadline of ctx: after ctx is cancelled no new
// traversal is started, though an HTTP request already in flight is not
// forcibly interrupted. The wait between traversals is interval. The remote
// implicit timeout is never modified, and input actions are never replayed
// against a re-resolved element: only the element is returned.
func ResolveShadowPath(ctx context.Context, wd WebDriver, steps []ShadowStep, interval time.Duration) (WebElement, error) {
	if err := validateShadowPath(steps, interval); err != nil {
		return nil, err
	}

	var timer *time.Timer
	for {
		elem, err := resolveOnce(wd, steps)
		if err == nil {
			return elem, nil
		}
		if !isRetryableShadowError(err) {
			return nil, err
		}
		if ctx.Err() != nil {
			return nil, ctx.Err()
		}

		if timer == nil {
			timer = time.NewTimer(interval)
			defer timer.Stop()
		} else {
			timer.Reset(interval)
		}
		select {
		case <-ctx.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return nil, ctx.Err()
		case <-timer.C:
		}
	}
}
