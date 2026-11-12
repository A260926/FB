package browser

import (
	"bytes"
	"compress/flate"
	"compress/gzip"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"strings"
	"time"

	"github.com/PuerkitoBio/goquery"
	"github.com/headzoo/surf/errors"
	"github.com/headzoo/surf/jar"
)

// Attribute represents a Browser capability.
type Attribute int

// AttributeMap represents a map of Attribute values.
type AttributeMap map[Attribute]bool

// File represents a input type file, that includes the fileName and a io.reader
type File struct {
	fileName string
	data     io.Reader
}

// FileSet represents a map of files used to port multipart
type FileSet map[string]*File

const (
	// SendReferer instructs a Browser to send the Referer header.
	SendReferer Attribute = iota

	// MetaRefreshHandling instructs a Browser to handle the refresh meta tag.
	MetaRefreshHandling

	// FollowRedirects instructs a Browser to follow Location headers.
	FollowRedirects
)

// InitialAssetsSliceSize is the initial size when allocating a slice of page
// assets. Increasing this size may lead to a very small performance increase
// when downloading assets from a page with a lot of assets.
var InitialAssetsSliceSize = 20

// Browsable represents an HTTP web browser.
type Browsable interface {
	// SetUserAgent sets the user agent.
	SetUserAgent(ua string)

	// SetAttribute sets a browser instruction attribute.
	SetAttribute(a Attribute, v bool)

	// SetAttributes is used to set all the browser attributes.
	SetAttributes(a AttributeMap)

	// SetState sets the init browser state.
	SetState(sj *jar.State)

	// State returns the browser state.
	State() *jar.State

	// SetBookmarksJar sets the bookmarks jar the browser uses.
	SetBookmarksJar(bj jar.BookmarksJar)

	// BookmarksJar returns the bookmarks jar the browser uses.
	BookmarksJar() jar.BookmarksJar

	// SetCookieJar is used to set the cookie jar the browser uses.
	SetCookieJar(cj http.CookieJar)

	// CookieJar returns the cookie jar the browser uses.
	CookieJar() http.CookieJar

	// SetHistoryJar is used to set the history jar the browser uses.
	SetHistoryJar(hj jar.History)

	// HistoryJar returns the history jar the browser uses.
	HistoryJar() jar.History

	// SetHeadersJar sets the headers the browser sends with each request.
	SetHeadersJar(h http.Header)

	// SetTimeout sets the timeout for requests.
	SetTimeout(t time.Duration)

	// SetTransport sets the http library transport mechanism for each request.
	SetTransport(rt http.RoundTripper)

	// AddRequestHeader adds a header the browser sends with each request.
	AddRequestHeader(name, value string)

	// SetReplayStorage configures where request bodies are kept so requests
	// can be re-sent by Reload after their original body has been consumed.
	SetReplayStorage(memoryLimit int64, dir string) error

	// Close releases resources owned by the browser, including temporary
	// files backing replayable request bodies. It is safe to call multiple
	// times.
	Close() error

	// Open requests the given URL using the GET method.
	Open(url string) error

	// Open requests the given URL using the HEAD method.
	Head(url string) error

	// OpenForm appends the data values to the given URL and sends a GET request.
	OpenForm(url string, data url.Values) error

	// OpenBookmark calls Get() with the URL for the bookmark with the given name.
	OpenBookmark(name string) error

	// Post requests the given URL using the POST method.
	Post(url string, contentType string, body io.Reader) error

	// PostForm requests the given URL using the POST method with the given data.
	PostForm(url string, data url.Values) error

	// PostMultipart requests the given URL using the POST method with the given data using multipart/form-data format.
	PostMultipart(u string, fields url.Values, files FileSet) error

	// Back loads the previously requested page.
	Back() bool

	// Reload duplicates the last successful request.
	Reload() error

	// Bookmark saves the page URL in the bookmarks with the given name.
	Bookmark(name string) error

	// Click clicks on the page element matched by the given expression.
	Click(expr string) error

	// Form returns the form in the current page that matches the given expr.
	Form(expr string) (Submittable, error)

	// Forms returns an array of every form in the page.
	Forms() []Submittable

	// Links returns an array of every link found in the page.
	Links() []*Link

	// Images returns an array of every image found in the page.
	Images() []*Image

	// Stylesheets returns an array of every stylesheet linked to the document.
	Stylesheets() []*Stylesheet

	// Scripts returns an array of every script linked to the document.
	Scripts() []*Script

	// SiteCookies returns the cookies for the current site.
	SiteCookies() []*http.Cookie

	// ResolveUrl returns an absolute URL for a possibly relative URL.
	ResolveUrl(u *url.URL) *url.URL

	// ResolveStringUrl works just like ResolveUrl, but the argument and return value are strings.
	ResolveStringUrl(u string) (string, error)

	// Download writes the contents of the document to the given writer.
	Download(o io.Writer) (int64, error)

	// Url returns the page URL as a string.
	Url() *url.URL

	// StatusCode returns the response status code.
	StatusCode() int

	// Title returns the page title.
	Title() string

	// ResponseHeaders returns the page headers.
	ResponseHeaders() http.Header

	// Body returns the page body as a string of html.
	Body() string

	// Dom returns the inner *goquery.Selection.
	Dom() *goquery.Selection

	// Find returns the dom selections matching the given expression.
	Find(expr string) *goquery.Selection

	// Create a new Browser instance and inherit the configuration
	// Read more: https://github.com/headzoo/surf/issues/23
	NewTab() (b *Browser)
}

// Browser is the default Browser implementation.
type Browser struct {
	// HTTP client
	client *http.Client

	// state is the current browser state.
	state *jar.State

	// userAgent is the User-Agent header value sent with requests.
	userAgent string

	// bookmarks stores the saved bookmarks.
	bookmarks jar.BookmarksJar

	// history stores the visited pages.
	history jar.History

	// headers are additional headers to send with each request.
	headers http.Header

	// attributes is the set browser attributes.
	attributes AttributeMap

	// refresh is a timer used to meta refresh pages.
	refresh *time.Timer

	// body mirrors the decoded response bytes of the current page. The
	// authoritative copy lives in state.Body; this field is kept in sync on
	// every commit and Back so Download and legacy callers always observe the
	// bytes of the currently restored page.
	body []byte

	// replay stores request bodies so consumed requests can be re-sent.
	replay *jar.ReplayStorage

	// closed is set once Close has been called.
	closed bool
}

// buildClient instanciates the *http.Client used by the browser
func (bow *Browser) buildClient() *http.Client {
	return &http.Client{
		CheckRedirect: bow.shouldRedirect,
	}
}

// Open requests the given URL using the GET method.
func (bow *Browser) Open(u string) error {
	ur, err := url.Parse(u)
	if err != nil {
		return err
	}
	return bow.httpGET(ur, nil)
}

// Head requests the given URL using the HEAD method.
func (bow *Browser) Head(u string) error {
	ur, err := url.Parse(u)
	if err != nil {
		return err
	}
	return bow.httpHEAD(ur, nil)
}

// OpenForm appends the data values to the given URL and sends a GET request.
func (bow *Browser) OpenForm(u string, data url.Values) error {
	ul, err := url.Parse(u)
	if err != nil {
		return err
	}
	ul.RawQuery = data.Encode()

	return bow.Open(ul.String())
}

// OpenBookmark calls Open() with the URL for the bookmark with the given name.
func (bow *Browser) OpenBookmark(name string) error {
	url, err := bow.bookmarks.Read(name)
	if err != nil {
		return err
	}
	return bow.Open(url)
}

// Post requests the given URL using the POST method.
//
// When body implements io.Closer it is closed exactly once by the browser,
// including when preparing or sending the request fails. The fully encoded
// body is captured by the replay storage before the request is sent, so
// Reload can re-send it even when body is a one-shot reader.
func (bow *Browser) Post(u string, contentType string, body io.Reader) error {
	ur, err := url.Parse(u)
	if err != nil {
		closeOnce(body)
		return err
	}
	err = bow.httpPOST(ur, bow.Url(), contentType, body)
	closeOnce(body)
	return err
}

// PostForm requests the given URL using the POST method with the given data.
func (bow *Browser) PostForm(u string, data url.Values) error {
	return bow.Post(u, "application/x-www-form-urlencoded", strings.NewReader(data.Encode()))
}

// PostMultipart requests the given URL using the POST method with the given data using multipart/form-data format.
//
// The multipart body is produced as a stream and captured by the replay
// storage as it is read: small bodies stay in memory and large attachments
// spill into a temporary file without ever holding the whole encoded body in
// a bytes.Buffer. Readers attached through Form.File/SetFile remain owned by
// the caller; the browser never closes them.
func (bow *Browser) PostMultipart(u string, fields url.Values, files FileSet) error {
	ur, err := url.Parse(u)
	if err != nil {
		return err
	}

	pr, pw := io.Pipe()
	writer := multipart.NewWriter(pw)
	contentType := writer.FormDataContentType()

	// The goroutine streams fields and attachments into the pipe. The browser
	// side drains the pipe while preparing the replayable body, so peak
	// memory does not grow with the attachment size.
	go func() {
		var writeErr error
		for k, vs := range fields {
			for _, v := range vs {
				if writeErr != nil {
					break
				}
				writeErr = writer.WriteField(k, v)
			}
			if writeErr != nil {
				break
			}
		}
		if writeErr == nil {
			for k, file := range files {
				var fw io.Writer
				fw, writeErr = writer.CreateFormFile(k, file.fileName)
				if writeErr != nil {
					break
				}
				if file.data != nil {
					_, writeErr = io.Copy(fw, file.data)
					if writeErr != nil {
						break
					}
				}
			}
		}
		if writeErr == nil {
			writeErr = writer.Close()
		}
		// Closing the pipe with the error makes the preparation on the
		// browser side fail with the original cause without sending anything.
		pw.CloseWithError(writeErr)
	}()

	err = bow.httpPOST(ur, bow.Url(), contentType, pr)
	// On the success path the pipe is already drained and closed; on
	// failure this unblocks the writer goroutine so it cannot leak.
	_ = pr.Close()
	return err
}

// Back loads the previously requested page.
//
// Back only restores pages from the local history: no request is made to the
// origin server. A single completed page is enough to go back, and the empty
// state of a freshly created browser is never exposed. It returns false when
// there is no previous page to restore.
func (bow *Browser) Back() bool {
	if bow.closed || bow.history == nil {
		return false
	}
	prev := bow.history.Pop()
	if prev == nil {
		return false
	}
	old := bow.state
	bow.state = prev
	bow.body = prev.Body
	if old != nil {
		old.ReleaseResources()
	}
	return true
}

// Reload re-sends the request that produced the current page.
//
// The original navigation request is replayed with its method, entry URL,
// Content-Type, applied request headers and request body intact. Each replay
// reads the stored body through a brand new reader, so Reload can be called
// repeatedly and after Back. Redirects are handled by net/http again: a POST
// that was originally answered with a 303 is re-sent as that same POST, not
// as the final GET of the receipt page. Cookies are recomputed by the
// cookie jar for this request; cookies appended to a previous send are not
// treated as permanent headers.
//
// Reload replaces the current record in place and does not add another
// history entry.
func (bow *Browser) Reload() error {
	if bow.closed {
		return errors.New("Browser has been closed.")
	}
	if bow.state == nil || bow.state.Request == nil {
		return errors.NewPageNotLoaded("Cannot reload, the previous request failed.")
	}

	orig := bow.state.Request
	req := orig.Clone(orig.Context())
	// The cookie jar recomputes the Cookie header on every send; keeping the
	// previously appended value would pin stale cookies to the request.
	req.Header.Del("Cookie")
	req.GetBody = nil

	stored := bow.state.Replay
	if stored != nil {
		// The new state takes its own reference to the shared body. It is
		// released again if anything fails before the page is committed.
		stored.Retain()
		reader, err := stored.NewReader()
		if err != nil {
			stored.Release()
			return err
		}
		req.Body = reader
		req.ContentLength = stored.Size()
		// A 307/308 redirect may need to rewind the body; every rewind gets
		// its own independent reader over the stored bytes.
		req.GetBody = func() (io.ReadCloser, error) {
			return stored.NewReader()
		}
	} else {
		req.Body = nil
		req.ContentLength = 0
	}

	return bow.sendPrepared(req, stored, true)
}

// Bookmark saves the page URL in the bookmarks with the given name.
func (bow *Browser) Bookmark(name string) error {
	return bow.bookmarks.Save(name, bow.ResolveUrl(bow.Url()).String())
}

// Click clicks on the page element matched by the given expression.
//
// Currently this is only useful for click on links, which will cause the browser
// to load the page pointed at by the link. Future versions of Surf may support
// JavaScript and clicking on elements will fire the click event.
func (bow *Browser) Click(expr string) error {
	sel := bow.Find(expr)
	if sel.Length() == 0 {
		return errors.NewElementNotFound(
			"Element not found matching expr '%s'.", expr)
	}
	if !sel.Is("a") {
		return errors.NewElementNotFound(
			"Expr '%s' must match an anchor tag.", expr)
	}

	href, err := bow.attrToResolvedUrl("href", sel)
	if err != nil {
		return err
	}

	return bow.httpGET(href, bow.Url())
}

// Form returns the form in the current page that matches the given expr.
func (bow *Browser) Form(expr string) (Submittable, error) {
	sel := bow.Find(expr)
	if sel.Length() == 0 {
		return nil, errors.NewElementNotFound(
			"Form not found matching expr '%s'.", expr)
	}
	if !sel.Is("form") {
		return nil, errors.NewElementNotFound(
			"Expr '%s' does not match a form tag.", expr)
	}

	return NewForm(bow, sel), nil
}

// Forms returns an array of every form in the page.
func (bow *Browser) Forms() []Submittable {
	sel := bow.Find("form")
	len := sel.Length()
	if len == 0 {
		return nil
	}

	forms := make([]Submittable, len)
	sel.Each(func(_ int, s *goquery.Selection) {
		forms = append(forms, NewForm(bow, s))
	})
	return forms
}

// Links returns an array of every link found in the page.
func (bow *Browser) Links() []*Link {
	links := make([]*Link, 0, InitialAssetsSliceSize)
	bow.Find("a").Each(func(_ int, s *goquery.Selection) {
		href, err := bow.attrToResolvedUrl("href", s)
		if err == nil {
			links = append(links, NewLinkAsset(
				href,
				bow.attrOrDefault("id", "", s),
				s.Text(),
			))
		}
	})

	return links
}

// Images returns an array of every image found in the page.
func (bow *Browser) Images() []*Image {
	images := make([]*Image, 0, InitialAssetsSliceSize)
	bow.Find("img").Each(func(_ int, s *goquery.Selection) {
		src, err := bow.attrToResolvedUrl("src", s)
		if err == nil {
			images = append(images, NewImageAsset(
				src,
				bow.attrOrDefault("id", "", s),
				bow.attrOrDefault("alt", "", s),
				bow.attrOrDefault("title", "", s),
			))
		}
	})

	return images
}

// Stylesheets returns an array of every stylesheet linked to the document.
func (bow *Browser) Stylesheets() []*Stylesheet {
	stylesheets := make([]*Stylesheet, 0, InitialAssetsSliceSize)
	bow.Find("link").Each(func(_ int, s *goquery.Selection) {
		rel, ok := s.Attr("rel")
		if ok && rel == "stylesheet" {
			href, err := bow.attrToResolvedUrl("href", s)
			if err == nil {
				stylesheets = append(stylesheets, NewStylesheetAsset(
					href,
					bow.attrOrDefault("id", "", s),
					bow.attrOrDefault("media", "all", s),
					bow.attrOrDefault("type", "text/css", s),
				))
			}
		}
	})

	return stylesheets
}

// Scripts returns an array of every script linked to the document.
func (bow *Browser) Scripts() []*Script {
	scripts := make([]*Script, 0, InitialAssetsSliceSize)
	bow.Find("script").Each(func(_ int, s *goquery.Selection) {
		src, err := bow.attrToResolvedUrl("src", s)
		if err == nil {
			scripts = append(scripts, NewScriptAsset(
				src,
				bow.attrOrDefault("id", "", s),
				bow.attrOrDefault("type", "text/javascript", s),
			))
		}
	})

	return scripts
}

// SiteCookies returns the cookies for the current site.
func (bow *Browser) SiteCookies() []*http.Cookie {
	if bow.client == nil {
		bow.client = bow.buildClient()
	}
	return bow.client.Jar.Cookies(bow.Url())
}

// SetState sets the browser state.
func (bow *Browser) SetState(sj *jar.State) {
	bow.state = sj
}

// State returns the browser state.
func (bow *Browser) State() *jar.State {
	return bow.state
}

// SetCookieJar is used to set the cookie jar the browser uses.
func (bow *Browser) SetCookieJar(cj http.CookieJar) {
	if bow.client == nil {
		bow.client = bow.buildClient()
	}
	bow.client.Jar = cj
}

// CookieJar returns the cookie jar the browser uses.
func (bow *Browser) CookieJar() http.CookieJar {
	if bow.client == nil {
		bow.client = bow.buildClient()
	}
	return bow.client.Jar
}

// SetUserAgent sets the user agent.
func (bow *Browser) SetUserAgent(userAgent string) {
	bow.userAgent = userAgent
}

// SetAttribute sets a browser instruction attribute.
func (bow *Browser) SetAttribute(a Attribute, v bool) {
	bow.attributes[a] = v
}

// SetAttributes is used to set all the browser attributes.
func (bow *Browser) SetAttributes(a AttributeMap) {
	bow.attributes = a
}

// SetBookmarksJar sets the bookmarks jar the browser uses.
func (bow *Browser) SetBookmarksJar(bj jar.BookmarksJar) {
	bow.bookmarks = bj
}

// BookmarksJar returns the bookmarks jar the browser uses.
func (bow *Browser) BookmarksJar() jar.BookmarksJar {
	return bow.bookmarks
}

// SetHistoryJar is used to set the history jar the browser uses.
func (bow *Browser) SetHistoryJar(hj jar.History) {
	if old, ok := bow.history.(*jar.MemoryHistory); ok && old != hj {
		// Drop this browser's ownership of the previous jar and release its
		// resources when it was the last owner.
		if old.Detach() {
			old.Clear()
		}
	}
	bow.history = hj
}

// HistoryJar returns the history jar the browser uses.
func (bow *Browser) HistoryJar() jar.History {
	return bow.history
}

// SetHeadersJar sets the headers the browser sends with each request.
func (bow *Browser) SetHeadersJar(h http.Header) {
	bow.headers = h
}

// SetTimeout sets the timeout for requests.
func (bow *Browser) SetTimeout(t time.Duration) {
	if bow.client == nil {
		bow.client = bow.buildClient()
	}
	bow.client.Timeout = t
}

// SetTransport sets the http library transport mechanism for each request.
func (bow *Browser) SetTransport(rt http.RoundTripper) {
	if bow.client == nil {
		bow.client = bow.buildClient()
	}
	bow.client.Transport = rt
}

// AddRequestHeader sets a header the browser sends with each request.
func (bow *Browser) AddRequestHeader(name, value string) {
	bow.headers.Set(name, value)
}

// DelRequestHeader deletes a header so the browser will not send it with future requests.
func (bow *Browser) DelRequestHeader(name string) {
	bow.headers.Del(name)
}

// SetReplayStorage configures where fully encoded request bodies are kept so
// consumed requests can be re-sent by Reload. Bodies up to memoryLimit bytes
// are held in memory; larger bodies are migrated to a temporary file created
// in dir. A memoryLimit of 0 sends every body to disk directly. An empty dir
// uses the operating system's temporary directory. A negative memoryLimit
// returns an error and leaves the current settings untouched.
func (bow *Browser) SetReplayStorage(memoryLimit int64, dir string) error {
	if memoryLimit < 0 {
		return errors.New("SetReplayStorage: memoryLimit must not be negative")
	}
	if dir != "" {
		info, err := os.Stat(dir)
		if err != nil {
			return err
		}
		if !info.IsDir() {
			return errors.New("SetReplayStorage: %s is not a directory", dir)
		}
	}
	bow.replay = jar.NewReplayStorage(memoryLimit, dir)
	return nil
}

// Close releases the resources owned by the browser, notably the temporary
// files backing replayable request bodies held by the current page and the
// history. Calling Close more than once is harmless. After Close,
// Open/Head/Post/PostForm/PostMultipart/Reload return an error and Back
// returns false.
//
// When the browser was created with NewTab and shares its pages with other
// tabs, Close only releases this tab's references: the other tabs keep
// working and their replay files are removed once the last owner goes away.
func (bow *Browser) Close() error {
	if bow.closed {
		return nil
	}
	bow.closed = true
	if bow.refresh != nil {
		bow.refresh.Stop()
	}
	if bow.state != nil {
		bow.state.ReleaseResources()
	}
	if bow.history != nil {
		// With the default MemoryHistory, drop only this tab's ownership.
		// Entries other tabs are still using survive until the last tab
		// closes or the states are otherwise removed.
		if shared, ok := bow.history.(*jar.MemoryHistory); ok {
			if shared.Detach() {
				shared.Clear()
			}
		} else {
			bow.history.Clear()
		}
	}
	return nil
}

// ResolveUrl returns an absolute URL for a possibly relative URL.
func (bow *Browser) ResolveUrl(u *url.URL) *url.URL {
	return bow.RelativeUrl().ResolveReference(u)
}

// ResolveStringUrl works just like ResolveUrl, but the argument and return value are strings.
func (bow *Browser) ResolveStringUrl(u string) (string, error) {
	pu, err := url.Parse(u)
	if err != nil {
		return "", err
	}
	pu = bow.RelativeUrl().ResolveReference(pu)
	return pu.String(), nil
}

// RelativeUrl returns URL relative to which all the others work. It is the
// page's own base URL, derived from its final document URL and <base> tag;
// pages without a <base> tag never inherit the base of a previous page.
func (bow *Browser) RelativeUrl() *url.URL {
	if bow.state != nil && bow.state.Base != nil {
		return bow.state.Base
	}
	return bow.Url()
}

// Download writes the contents of the document to the given writer. The
// bytes are the decoded response body captured when the page was loaded, so
// after Back they belong to the restored page rather than the previous one.
func (bow *Browser) Download(o io.Writer) (int64, error) {
	var body []byte
	if bow.state != nil {
		body = bow.state.Body
	}
	return io.Copy(o, bytes.NewReader(body))
}

// Url returns the page URL as a string.
func (bow *Browser) Url() *url.URL {
	if bow.state == nil {
		return nil
	}
	if bow.state.Response != nil {
		return bow.state.Response.Request.URL
	}
	// there is a possibility that we issued a request, but for
	// whatever reason the request failed.
	if bow.state.Request != nil {
		return bow.state.Request.URL
	}
	return nil
}

// StatusCode returns the response status code.
func (bow *Browser) StatusCode() int {
	return bow.state.Response.StatusCode
}

// Title returns the page title.
func (bow *Browser) Title() string {
	return bow.state.Dom.Find("title").Text()
}

// ResponseHeaders returns the page headers.
func (bow *Browser) ResponseHeaders() http.Header {
	return bow.state.Response.Header
}

// Body returns the page body as a string of html.
func (bow *Browser) Body() string {
	body, _ := bow.state.Dom.Find("body").Html()
	return body
}

// Dom returns the inner *goquery.Selection.
func (bow *Browser) Dom() *goquery.Selection {
	return bow.state.Dom.First()
}

// Find returns the dom selections matching the given expression.
func (bow *Browser) Find(expr string) *goquery.Selection {
	return bow.state.Dom.Find(expr)
}

// NewTab creates a new Browser sharing the configuration, state and jars.
//
// The current page and every history entry retain their replay resources for
// both tabs: closing one tab releases only its references, and the backing
// temporary files are removed when the last owning tab releases them.
func (bow *Browser) NewTab() (b *Browser) {
	b = &Browser{}
	*b = *bow
	b.closed = false

	if bow.state != nil {
		bow.state.RetainResources()
	}
	if shared, ok := bow.history.(*jar.MemoryHistory); ok {
		shared.Attach()
	}
	return b
}

// buildRequest creates and returns a *http.Request type.
// Sets any headers that need to be sent with the request.
func (bow *Browser) buildRequest(method, url string, ref *url.URL, body io.Reader) (*http.Request, error) {
	req, err := http.NewRequest(method, url, body)
	if err != nil {
		return nil, err
	}
	req.Header = copyHeaders(bow.headers)

	if host := req.Header.Get("Host"); host != "" {
		req.Host = host
	}
	req.Header.Set("User-Agent", bow.userAgent)
	if bow.attributes[SendReferer] && ref != nil {
		req.Header.Set("Referer", ref.String())
	}
	if os.Getenv("SURF_DEBUG_HEADERS") != "" {
		d, _ := httputil.DumpRequest(req, false)
		fmt.Fprintln(os.Stderr, "===== [DUMP] =====\n", string(d))
	}

	return req, nil
}

func copyHeaders(h http.Header) http.Header {
	if h == nil {
		return nil
	}
	h2 := make(http.Header, len(h))
	for k, v := range h {
		h2[k] = v
	}
	return h2
}

// replayStorage returns the configured replay storage, creating a default one
// for browsers that were not initialized through surf.NewBrowser.
func (bow *Browser) replayStorage() *jar.ReplayStorage {
	if bow.replay == nil {
		bow.replay = jar.NewReplayStorage(jar.DefaultReplayMemoryLimit, "")
	}
	return bow.replay
}

// httpGET makes an HTTP GET request for the given URL.
// When via is not nil, and AttributeSendReferer is true, the Referer header will
// be set to ref.
func (bow *Browser) httpGET(u *url.URL, ref *url.URL) error {
	req, replay, err := bow.prepareRequest("GET", u, ref, "", nil)
	if err != nil {
		return err
	}
	return bow.sendPrepared(req, replay, false)
}

// httpHEAD makes an HTTP HEAD request for the given URL.
// When via is not nil, and AttributeSendReferer is true, the Referer header will
// be set to ref.
func (bow *Browser) httpHEAD(u *url.URL, ref *url.URL) error {
	req, replay, err := bow.prepareRequest("HEAD", u, ref, "", nil)
	if err != nil {
		return err
	}
	return bow.sendPrepared(req, replay, false)
}

// httpPOST makes an HTTP POST request for the given URL.
// When via is not nil, and AttributeSendReferer is true, the Referer header will
// be set to ref.
func (bow *Browser) httpPOST(u *url.URL, ref *url.URL, contentType string, body io.Reader) error {
	req, replay, err := bow.prepareRequest("POST", u, ref, contentType, body)
	if err != nil {
		return err
	}
	return bow.sendPrepared(req, replay, false)
}

// prepareRequest captures the request body in the replay storage and builds
// the request that will actually be sent. On success the returned replay
// body carries one owner reference, transferred to the resulting page state
// (or released on failure).
func (bow *Browser) prepareRequest(method string, u *url.URL, ref *url.URL, contentType string, body io.Reader) (*http.Request, *jar.ReplayBody, error) {
	if bow.closed {
		return nil, nil, errors.New("Browser has been closed.")
	}

	replay, err := bow.replayStorage().Prepare(body)
	if err != nil {
		return nil, nil, fmt.Errorf("surf: preparing request body for %s %s: %w", method, u.String(), err)
	}

	var sendBody io.ReadCloser
	if replay != nil {
		reader, err := replay.NewReader()
		if err != nil {
			replay.Release()
			return nil, nil, err
		}
		sendBody = reader
	}

	req, err := bow.buildRequest(method, u.String(), ref, sendBody)
	if err != nil {
		if sendBody != nil {
			_ = sendBody.Close()
		}
		if replay != nil {
			replay.Release()
		}
		return nil, nil, err
	}
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	if replay != nil {
		req.ContentLength = replay.Size()
		// A 307/308 redirect may need to rewind the body; every rewind gets
		// its own independent reader over the stored bytes.
		req.GetBody = func() (io.ReadCloser, error) {
			return replay.NewReader()
		}
	}
	return req, replay, nil
}

// sendPrepared sends a fully prepared request and, on success, atomically
// commits the resulting page state. replace reports whether the request
// reloads the current page (history must not grow). Any failure before the
// commit leaves the current page, its Download bytes, base and history
// untouched and releases the replay reference passed in.
func (bow *Browser) sendPrepared(req *http.Request, replay *jar.ReplayBody, replace bool) (err error) {
	if bow.client == nil {
		bow.client = bow.buildClient()
	}
	bow.preSend()

	resp, err := bow.client.Do(req)
	if err != nil {
		if replay != nil {
			replay.Release()
		}
		return fmt.Errorf("surf: sending %s %s: %w", req.Method, req.URL.String(), err)
	}
	defer resp.Body.Close()
	// The cookie jar appends the Cookie header for this send. It must not be
	// pinned onto the stored request: every replay recomputes cookies from
	// the jar against the request URL.
	req.Header.Del("Cookie")

	reader, err := encodedBodyReader(resp)
	if err != nil {
		if replay != nil {
			replay.Release()
		}
		return fmt.Errorf("surf: decoding response body for %s: %w", req.URL.String(), err)
	}

	page, err := io.ReadAll(reader)
	if err != nil {
		if replay != nil {
			replay.Release()
		}
		return fmt.Errorf("surf: reading response body for %s: %w", req.URL.String(), err)
	}

	dom, err := goquery.NewDocumentFromReader(bytes.NewReader(page))
	if err != nil {
		if replay != nil {
			replay.Release()
		}
		return fmt.Errorf("surf: parsing response body for %s: %w", req.URL.String(), err)
	}

	next := &jar.State{
		Request:  req,
		Response: resp,
		Dom:      dom,
		Body:     page,
		Base:     pageBase(resp.Request.URL, dom),
		Replay:   replay,
	}
	bow.commit(next, replace)
	bow.postSend()
	return nil
}

// commit installs next as the current page. For a reload the previous page is
// replaced and the history is left untouched. Otherwise the previous
// completed page moves into the history.
func (bow *Browser) commit(next *jar.State, replace bool) {
	old := bow.state
	bow.state = next
	bow.body = next.Body
	if old == nil {
		return
	}
	if replace || old.Response == nil {
		// Reload drops the previous record; the empty initial state has
		// never been part of the history.
		old.ReleaseResources()
		return
	}
	bow.history.Push(old)
}

// encodedBodyReader wraps the response body in the matching decompressor.
func encodedBodyReader(resp *http.Response) (io.Reader, error) {
	switch resp.Header.Get("Content-Encoding") {
	case "gzip":
		return gzip.NewReader(resp.Body)
	case "deflate":
		return flate.NewReader(resp.Body), nil
	default:
		return resp.Body, nil
	}
}

// pageBase resolves the base URL of a page from its final document URL and
// its <base> tag, if any. The computation never consults a previously loaded
// page and is independent of the MetaRefreshHandling attribute.
func pageBase(docURL *url.URL, dom *goquery.Document) *url.URL {
	base := docURL
	if sel := dom.Find("base[href]"); sel.Length() > 0 {
		if href, ok := sel.First().Attr("href"); ok && href != "" {
			if ref, err := url.Parse(href); err == nil {
				base = docURL.ResolveReference(ref)
			}
		}
	}
	return base
}

// closeOnce closes body when it implements io.Closer.
func closeOnce(body io.Reader) {
	if closer, ok := body.(io.Closer); ok {
		_ = closer.Close()
	}
}

// preSend sets browser state before sending a request.
func (bow *Browser) preSend() {
	if bow.refresh != nil {
		bow.refresh.Stop()
	}
}

// postSend sets browser state after sending a request.
func (bow *Browser) postSend() {
	if isContentTypeHtml(bow.state.Response) && bow.attributes[MetaRefreshHandling] {
		sel := bow.Find("meta[http-equiv='refresh']")
		if sel.Length() > 0 {
			attr, ok := sel.Attr("content")
			if ok {
				dur, err := time.ParseDuration(attr + "s")
				if err == nil {
					bow.refresh = time.NewTimer(dur)
					go func() {
						<-bow.refresh.C
						_ = bow.Reload()
					}()
				}
			}
		}
	}
}

// shouldRedirect is used as the value to http.Client.CheckRedirect.
func (bow *Browser) shouldRedirect(req *http.Request, _ []*http.Request) error {
	if bow.attributes[FollowRedirects] {
		req.Header.Set("User-Agent", bow.userAgent)
		return nil
	}
	return errors.NewLocation(
		"Redirects are disabled. Cannot follow '%s'.", req.URL.String())
}

// attributeToUrl reads an attribute from an element and returns a url.
func (bow *Browser) attrToResolvedUrl(name string, sel *goquery.Selection) (*url.URL, error) {
	src, ok := sel.Attr(name)
	if !ok {
		return nil, errors.NewAttributeNotFound(
			"Attribute '%s' not found.", name)
	}
	ur, err := url.Parse(src)
	if err != nil {
		return nil, err
	}

	return bow.ResolveUrl(ur), nil
}

// attributeOrDefault reads an attribute and returns it or the default value when it's empty.
func (bow *Browser) attrOrDefault(name string, def string, sel *goquery.Selection) string {
	a, ok := sel.Attr(name)
	if ok {
		return a
	}
	return def
}

// isContentTypeHtml returns true when the given response sent the "text/html" content type.
func isContentTypeHtml(res *http.Response) bool {
	if res != nil {
		ct := res.Header.Get("Content-Type")
		return ct == "" || strings.Contains(ct, "text/html")
	}
	return false
}
