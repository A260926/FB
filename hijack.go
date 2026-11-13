package rod

import (
	"bytes"
	"context"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strings"
	"sync"

	"github.com/go-rod/rod/lib/proto"
	"github.com/go-rod/rod/lib/utils"
	"github.com/ysmood/gson"
)

// HijackRequests same as Page.HijackRequests, but can intercept requests of the entire browser.
func (b *Browser) HijackRequests() *HijackRouter {
	return newHijackRouter(b, b).initEvents()
}

// HijackRequests creates a new router instance for requests hijacking.
// When use Fetch domain outside the router should be stopped. Enabling hijacking disables page caching,
// but such as 304 Not Modified will still work as expected.
// The entire process of hijacking one request:
//
//	browser --req-> rod ---> server ---> rod --res-> browser
//
// The --req-> and --res-> are the parts that can be modified.
func (p *Page) HijackRequests() *HijackRouter {
	return newHijackRouter(p.browser, p).initEvents()
}

// HijackRouter context.
type HijackRouter struct {
	run  func()
	stop func()

	// confMu guards the local Fetch configuration. The configuration is only
	// updated after a Fetch.enable call is confirmed by the browser, so a failed
	// configuration call leaves the last confirmed state untouched.
	confMu      sync.Mutex
	handlers    []*hijackHandler
	authHandler func(*proto.FetchAuthRequired) *proto.FetchAuthChallengeResponse
	authOnError func(error)
	enable      *proto.FetchEnable

	client    proto.Client
	browser   *Browser
	sessionID proto.TargetSessionID
}

func newHijackRouter(browser *Browser, client proto.Client) *HijackRouter {
	var sessionID proto.TargetSessionID
	if tsa, ok := client.(proto.Sessionable); ok {
		sessionID = tsa.GetSessionID()
	}

	return &HijackRouter{
		enable:      &proto.FetchEnable{},
		browser:     browser,
		client:      client,
		sessionID:   sessionID,
		handlers:    []*hijackHandler{},
		authOnError: func(error) {},
	}
}

func (r *HijackRouter) initEvents() *HijackRouter { //nolint: gocognit
	ctx := r.browser.ctx
	if cta, ok := r.client.(proto.Contextable); ok {
		ctx = cta.GetContext()
	}

	eventCtx, cancel := context.WithCancel(ctx)
	r.stop = cancel

	_ = r.enable.Call(r.client)

	// Both Fetch protocol events go through the same broadcast subscription and
	// the same dispatch chain. eachEvent only receives events of the router's own
	// session for a page router, while a browser router keeps every session.
	r.run = r.browser.Context(eventCtx).eachEvent(r.sessionID,
		func(e *proto.FetchRequestPaused, sid proto.TargetSessionID) bool {
			client := r.sessionClient(eventCtx, sid)

			go r.dispatchRequestPaused(eventCtx, e, client)

			return false
		},
		func(e *proto.FetchAuthRequired, sid proto.TargetSessionID) bool {
			client := r.sessionClient(eventCtx, sid)

			go r.dispatchAuthRequired(e, client)

			return false
		},
	)
	return r
}

// sessionClient returns a proto.Client that sends every command back to the
// session that produced the event. The session id is taken from the event,
// never inferred from the request id; an empty session targets the browser.
func (r *HijackRouter) sessionClient(ctx context.Context, sessionID proto.TargetSessionID) proto.Client {
	return &hijackSessionClient{
		ctx:       ctx,
		browser:   r.browser,
		sessionID: sessionID,
	}
}

// hijackSessionClient behaves like a Page client bound to one specific session:
// proto.call reads the context and session id through Contextable/Sessionable.
type hijackSessionClient struct {
	ctx       context.Context
	browser   *Browser
	sessionID proto.TargetSessionID
}

func (c *hijackSessionClient) GetContext() context.Context { return c.ctx }

func (c *hijackSessionClient) GetSessionID() proto.TargetSessionID { return c.sessionID }

func (c *hijackSessionClient) Call(
	ctx context.Context, sessionID string, methodName string, params interface{},
) ([]byte, error) {
	return c.browser.Call(ctx, sessionID, methodName, params)
}

// buildEnableLocked merges the url/resource-type patterns and the auth switch
// into a single Fetch.enable config. Caller must hold confMu.
func (r *HijackRouter) buildEnableLocked() *proto.FetchEnable {
	patterns := make([]*proto.FetchRequestPattern, 0, len(r.handlers))
	for _, h := range r.handlers {
		patterns = append(patterns, &proto.FetchRequestPattern{
			URLPattern:   h.pattern,
			ResourceType: h.resourceType,
		})
	}

	return &proto.FetchEnable{
		Patterns:           patterns,
		HandleAuthRequests: r.authHandler != nil,
	}
}

// commit mutates the local configuration and sends a merged Fetch.enable. When
// the call fails the local state is rolled back to the last confirmed state,
// so the next configuration operation continues from a known-good configuration.
func (r *HijackRouter) commit(mutate func()) error {
	r.confMu.Lock()
	snapHandlers := append([]*hijackHandler(nil), r.handlers...)
	snapAuthHandler := r.authHandler
	snapAuthOnError := r.authOnError

	mutate()
	enable := r.buildEnableLocked()
	r.confMu.Unlock()

	if err := enable.Call(r.client); err != nil {
		r.confMu.Lock()
		r.handlers = snapHandlers
		r.authHandler = snapAuthHandler
		r.authOnError = snapAuthOnError
		r.confMu.Unlock()

		return err
	}

	r.confMu.Lock()
	r.enable = enable
	r.confMu.Unlock()

	return nil
}

// Add a hijack handler to router, the doc of the pattern is the same as "proto.FetchRequestPattern.URLPattern".
// An empty resourceType matches every resource type.
func (r *HijackRouter) Add(pattern string, resourceType proto.NetworkResourceType, handler func(*Hijack)) error {
	return r.commit(func() {
		r.handlers = append(r.handlers, &hijackHandler{
			pattern:      pattern,
			resourceType: resourceType,
			regexp:       regexp.MustCompile(proto.PatternToReg(pattern)),
			handler:      handler,
		})
	})
}

// Remove handler via the pattern. Every handler registered with the pattern is
// removed, handlers of other patterns keep their full resource-type settings.
func (r *HijackRouter) Remove(pattern string) error {
	return r.commit(func() {
		handlers := []*hijackHandler{}
		for _, h := range r.handlers {
			if h.pattern != pattern {
				handlers = append(handlers, h)
			}
		}
		r.handlers = handlers
	})
}

// SetAuthHandler enables Fetch.authRequired handling on the same router and the
// same Fetch.enable configuration as the url/resource-type routes.
//
// A non-nil handler enables the authentication events, passing nil removes the
// authentication feature. onError receives the original error when the
// Fetch.continueWithAuth command fails; a nil onError is replaced with a no-op.
//
// Like Add and Remove, it must be configured before Run, hot reloading while the
// router is running is not supported. The handler decides the answer of every
// challenge: return a *proto.FetchAuthChallengeResponse to provide credentials
// or cancel explicitly, return nil to answer CancelAuth so that no interactive
// authentication dialog pops up. Each challenge of a request id is delivered
// separately, the router never assumes a previous challenge was already handled.
func (r *HijackRouter) SetAuthHandler(
	handler func(*proto.FetchAuthRequired) *proto.FetchAuthChallengeResponse,
	onError func(error),
) error {
	if onError == nil {
		onError = func(error) {}
	}

	return r.commit(func() {
		r.authHandler = handler
		r.authOnError = onError
	})
}

// dispatchRequestPaused only feeds the normal request handlers.
func (r *HijackRouter) dispatchRequestPaused(ctx context.Context, e *proto.FetchRequestPaused, client proto.Client) {
	hijackCtx := r.new(ctx, e)

	r.confMu.Lock()
	handlers := append([]*hijackHandler(nil), r.handlers...)
	r.confMu.Unlock()

	for _, h := range handlers {
		if !h.match(e) {
			continue
		}

		// Clear the Skip flag left by any previous candidate before each call.
		hijackCtx.Skip = false
		h.handler(hijackCtx)

		if hijackCtx.continueRequest != nil {
			hijackCtx.continueRequest.RequestID = e.RequestID
			if err := hijackCtx.continueRequest.Call(client); err != nil {
				hijackCtx.OnError(err)
			}

			return
		}

		if hijackCtx.Skip {
			continue
		}

		if hijackCtx.Response.fail.ErrorReason != "" {
			if err := hijackCtx.Response.fail.Call(client); err != nil {
				hijackCtx.OnError(err)
			}

			return
		}

		if err := hijackCtx.Response.payload.Call(client); err != nil {
			hijackCtx.OnError(err)

			return
		}

		// A terminal Fulfill ends dispatch, later handlers are not called.
		return
	}

	// No matching handler, or every candidate skipped: continue once untouched.
	err := proto.FetchContinueRequest{RequestID: e.RequestID}.Call(client)
	if err != nil {
		hijackCtx.OnError(err)
	}
}

// dispatchAuthRequired only feeds the authentication handler. It never runs the
// normal request handlers and never sends Fetch.continueRequest.
func (r *HijackRouter) dispatchAuthRequired(e *proto.FetchAuthRequired, client proto.Client) {
	r.confMu.Lock()
	handler := r.authHandler
	onError := r.authOnError
	r.confMu.Unlock()

	if handler == nil {
		return
	}

	response := handler(e)
	if response == nil {
		response = &proto.FetchAuthChallengeResponse{
			Response: proto.FetchAuthChallengeResponseResponseCancelAuth,
		}
	} else {
		// Work on a copy so the caller's returned object is never written back.
		copied := *response
		response = &copied
	}

	err := proto.FetchContinueWithAuth{
		RequestID:             e.RequestID,
		AuthChallengeResponse: response,
	}.Call(client)
	if err != nil {
		onError(err)
	}
}

// new context.
func (r *HijackRouter) new(ctx context.Context, e *proto.FetchRequestPaused) *Hijack {
	headers := http.Header{}
	for k, v := range e.Request.Headers {
		headers[k] = []string{v.String()}
	}

	u, _ := url.Parse(e.Request.URL)

	req := &http.Request{
		Method: e.Request.Method,
		URL:    u,
		Body:   io.NopCloser(strings.NewReader(e.Request.PostData)),
		Header: headers,
	}

	return &Hijack{
		Request: &HijackRequest{
			event: e,
			req:   req.WithContext(ctx),
		},
		Response: &HijackResponse{
			payload: &proto.FetchFulfillRequest{
				ResponseCode: 200,
				RequestID:    e.RequestID,
			},
			fail: &proto.FetchFailRequest{
				RequestID: e.RequestID,
			},
		},
		OnError: func(_ error) {},

		browser: r.browser,
	}
}

// Run the router, after you call it, you shouldn't add new handler to it.
func (r *HijackRouter) Run() {
	r.run()
}

// Stop the router.
func (r *HijackRouter) Stop() error {
	r.stop()
	return proto.FetchDisable{}.Call(r.client)
}

// hijackHandler to handle each request that match the regexp.
type hijackHandler struct {
	pattern      string
	resourceType proto.NetworkResourceType
	regexp       *regexp.Regexp
	handler      func(*Hijack)
}

// match reports whether the paused event matches both the url pattern and the
// resource type. The event resource type is always checked here because Chrome
// may pause other resource types of the same url for a different pattern.
func (h *hijackHandler) match(e *proto.FetchRequestPaused) bool {
	if h.resourceType != "" && h.resourceType != e.ResourceType {
		return false
	}

	return h.regexp.MatchString(e.Request.URL)
}

// Hijack context.
type Hijack struct {
	Request  *HijackRequest
	Response *HijackResponse
	OnError  func(error)

	// Skip to next handler
	Skip bool

	continueRequest *proto.FetchContinueRequest

	// CustomState is used to store things for this context
	CustomState interface{}

	browser *Browser
}

// ContinueRequest without hijacking. The RequestID will be set by the router, you don't have to set it.
func (h *Hijack) ContinueRequest(cq *proto.FetchContinueRequest) {
	h.continueRequest = cq
}

// LoadResponse will send request to the real destination and load the response as default response to override.
func (h *Hijack) LoadResponse(client *http.Client, loadBody bool) error {
	res, err := client.Do(h.Request.req)
	if err != nil {
		return err
	}

	defer func() { _ = res.Body.Close() }()

	h.Response.payload.ResponseCode = res.StatusCode
	h.Response.RawResponse = res

	for k, vs := range res.Header {
		for _, v := range vs {
			h.Response.SetHeader(k, v)
		}
	}

	if loadBody {
		b, err := io.ReadAll(res.Body)
		if err != nil {
			return err
		}
		h.Response.payload.Body = b
	}

	return nil
}

// HijackRequest context.
type HijackRequest struct {
	event *proto.FetchRequestPaused
	req   *http.Request
}

// Type of the resource.
func (ctx *HijackRequest) Type() proto.NetworkResourceType {
	return ctx.event.ResourceType
}

// Method of the request.
func (ctx *HijackRequest) Method() string {
	return ctx.event.Request.Method
}

// URL of the request.
func (ctx *HijackRequest) URL() *url.URL {
	u, _ := url.Parse(ctx.event.Request.URL)
	return u
}

// Header via a key.
func (ctx *HijackRequest) Header(key string) string {
	return ctx.event.Request.Headers[key].String()
}

// Headers of request.
func (ctx *HijackRequest) Headers() proto.NetworkHeaders {
	return ctx.event.Request.Headers
}

// Body of the request, devtools API doesn't support binary data yet, only string can be captured.
func (ctx *HijackRequest) Body() string {
	return ctx.event.Request.PostData
}

// JSONBody of the request.
func (ctx *HijackRequest) JSONBody() gson.JSON {
	return gson.NewFrom(ctx.Body())
}

// Req returns the underlying http.Request instance that will be used to send the request.
func (ctx *HijackRequest) Req() *http.Request {
	return ctx.req
}

// SetContext of the underlying http.Request instance.
func (ctx *HijackRequest) SetContext(c context.Context) *HijackRequest {
	ctx.req = ctx.req.WithContext(c)
	return ctx
}

// SetBody of the request, if obj is []byte or string, raw body will be used, else it will be encoded as json.
func (ctx *HijackRequest) SetBody(obj interface{}) *HijackRequest {
	var b []byte

	switch body := obj.(type) {
	case []byte:
		b = body
	case string:
		b = []byte(body)
	default:
		b = utils.MustToJSONBytes(body)
	}

	ctx.req.Body = io.NopCloser(bytes.NewBuffer(b))

	return ctx
}

// IsNavigation determines whether the request is a navigation request.
func (ctx *HijackRequest) IsNavigation() bool {
	return ctx.Type() == proto.NetworkResourceTypeDocument
}

// HijackResponse context.
type HijackResponse struct {
	payload     *proto.FetchFulfillRequest
	RawResponse *http.Response
	fail        *proto.FetchFailRequest
}

// Payload to respond the request from the browser.
func (ctx *HijackResponse) Payload() *proto.FetchFulfillRequest {
	return ctx.payload
}

// Body of the payload.
func (ctx *HijackResponse) Body() string {
	return string(ctx.payload.Body)
}

// Headers returns the clone of response headers.
// If you want to modify the response headers use HijackResponse.SetHeader .
func (ctx *HijackResponse) Headers() http.Header {
	header := http.Header{}

	for _, h := range ctx.payload.ResponseHeaders {
		header.Add(h.Name, h.Value)
	}

	return header
}

// SetHeader of the payload via key-value pairs.
func (ctx *HijackResponse) SetHeader(pairs ...string) *HijackResponse {
	headerIndex := make(map[string]int, len(ctx.payload.ResponseHeaders))
	for i, header := range ctx.payload.ResponseHeaders {
		headerIndex[header.Name] = i
	}

	for i := 0; i < len(pairs); i += 2 {
		name := pairs[i]
		value := pairs[i+1]

		if idx, exists := headerIndex[name]; exists {
			ctx.payload.ResponseHeaders[idx].Value = value
		} else {
			ctx.payload.ResponseHeaders = append(ctx.payload.ResponseHeaders, &proto.FetchHeaderEntry{
				Name:  name,
				Value: value,
			})
			headerIndex[name] = len(ctx.payload.ResponseHeaders) - 1
		}
	}
	return ctx
}

// AddHeader appends key-value pairs to the end of the response headers.
// Duplicate keys will be preserved.
func (ctx *HijackResponse) AddHeader(pairs ...string) *HijackResponse {
	for i := 0; i < len(pairs); i += 2 {
		ctx.payload.ResponseHeaders = append(ctx.payload.ResponseHeaders, &proto.FetchHeaderEntry{
			Name:  pairs[i],
			Value: pairs[i+1],
		})
	}
	return ctx
}

// SetBody of the payload, if obj is []byte or string, raw body will be used, else it will be encoded as json.
func (ctx *HijackResponse) SetBody(obj interface{}) *HijackResponse {
	switch body := obj.(type) {
	case []byte:
		ctx.payload.Body = body
	case string:
		ctx.payload.Body = []byte(body)
	default:
		ctx.payload.Body = utils.MustToJSONBytes(body)
	}
	return ctx
}

// Fail request.
func (ctx *HijackResponse) Fail(reason proto.NetworkErrorReason) *HijackResponse {
	ctx.fail.ErrorReason = reason
	return ctx
}

// HandleAuth for the next basic HTTP authentication.
// It will prevent the popup that requires user to input user name and password.
// Ref: https://developer.mozilla.org/en-US/docs/Web/HTTP/Authentication
func (b *Browser) HandleAuth(username, password string) func() error {
	enable := b.DisableDomain("", &proto.FetchEnable{})
	disable := b.EnableDomain("", &proto.FetchEnable{
		HandleAuthRequests: true,
	})

	paused := &proto.FetchRequestPaused{}
	auth := &proto.FetchAuthRequired{}

	ctx, cancel := context.WithCancel(b.ctx)
	waitPaused := b.Context(ctx).WaitEvent(paused)
	waitAuth := b.Context(ctx).WaitEvent(auth)

	return func() (err error) {
		defer enable()
		defer disable()
		defer cancel()

		waitPaused()

		err = proto.FetchContinueRequest{
			RequestID: paused.RequestID,
		}.Call(b)
		if err != nil {
			return
		}

		waitAuth()

		err = proto.FetchContinueWithAuth{
			RequestID: auth.RequestID,
			AuthChallengeResponse: &proto.FetchAuthChallengeResponse{
				Response: proto.FetchAuthChallengeResponseResponseProvideCredentials,
				Username: username,
				Password: password,
			},
		}.Call(b)

		return
	}
}
