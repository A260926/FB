package selenium_test

// These tests exercise FullPageScreenshot without a real browser. A stateful
// http.RoundTripper acts as a WebDriver remote end: it keeps the actual scroll
// position, serves the metrics script, applies a scroll model, and returns
// viewport PNG tiles whose pixels encode their global canvas position.
//
// The expected images are built independently in this test using the same
// public quantization rule Q(t) = floor(t*devicePixelRatio + 0.5). The
// production planner is never consulted for the expected tile positions.

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"math"
	"net/http"
	"strings"
	"sync"
	"testing"

	"github.com/tebeka/selenium"
)

type fpageTransport func(*http.Request) (*http.Response, error)

func (f fpageTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func fpageJSON(v interface{}) *http.Response {
	data, err := json.Marshal(v)
	if err != nil {
		panic(err)
	}
	return &http.Response{
		StatusCode: 200,
		Header:     http.Header{"Content-Type": []string{"application/json"}},
		Body:       io.NopCloser(bytes.NewReader(data)),
	}
}

// fpageConfig describes a fixture page. All length fields are CSS coordinates.
type fpageConfig struct {
	width          float64
	height         float64
	viewportWidth  float64
	viewportHeight float64
	dpr            float64
	startX         float64
	startY         float64
}

// fpageQ is the independent quantization used to build expected images and
// fixture tiles.
func fpageQ(t *testing.T, v, r float64) uint64 {
	t.Helper()
	q := math.Floor(v*r + 0.5)
	if q < 0 || q > 1<<40 {
		t.Fatalf("fixture: cannot quantize %v*%v", v, r)
	}
	return uint64(q)
}

// fpageColor deterministically encodes a global canvas coordinate.
func fpageColor(x, y uint64) color.NRGBA {
	return color.NRGBA{
		R: byte(x*37 + y*11),
		G: byte(x*5 + y*29),
		B: byte(x*13 + y*3),
		A: 255,
	}
}

type fpageServer struct {
	t   *testing.T
	w3c bool

	mu sync.Mutex
	// cfg and x/y are the live layout and actual scroll position.
	cfg  fpageConfig
	x, y float64

	executePaths       []string
	scrollRequests     [][2]float64
	scrollActuals      [][2]float64
	scrollCalls        int
	metricsCalls       int
	screenshotCalls    int
	screenshotOrigins  [][2]uint64
	lastScreenshotSize [2]uint64

	// Hooks run with mu held and may modify cfg or the state.
	scrollHook   func(s *fpageServer, n int, reqX, reqY float64) (float64, float64, error)
	metricsHook  func(s *fpageServer, n int)
	metricsReply func(s *fpageServer, n int) map[string]interface{}
	// screenshotHook returns nil data with nil error to request the default
	// tile.
	screenshotHook func(s *fpageServer, n int) ([]byte, error)
}

func newFpageServer(t *testing.T, cfg fpageConfig, w3c bool) *fpageServer {
	t.Helper()
	return &fpageServer{t: t, w3c: w3c, cfg: cfg, x: cfg.startX, y: cfg.startY}
}

func (s *fpageServer) defaultScroll(reqX, reqY float64) (float64, float64) {
	maxX := s.cfg.width - s.cfg.viewportWidth
	if maxX < 0 {
		maxX = 0
	}
	maxY := s.cfg.height - s.cfg.viewportHeight
	if maxY < 0 {
		maxY = 0
	}
	x := math.Min(math.Max(reqX, 0), maxX)
	y := math.Min(math.Max(reqY, 0), maxY)
	return x, y
}

func (s *fpageServer) errorResponse(format string, args ...interface{}) *http.Response {
	msg := fmt.Sprintf(format, args...)
	if s.w3c {
		return fpageJSON(map[string]interface{}{
			"value": map[string]interface{}{"error": "unknown error", "message": msg},
		})
	}
	return fpageJSON(map[string]interface{}{
		"status": 13,
		"value":  map[string]interface{}{"message": msg},
	})
}

func (s *fpageServer) valueResponse(v interface{}) *http.Response {
	return fpageJSON(map[string]interface{}{"value": v})
}

func (s *fpageServer) roundTrip(r *http.Request) (*http.Response, error) {
	switch {
	case r.Method == "POST" && r.URL.Path == "/session":
		if s.w3c {
			return fpageJSON(map[string]interface{}{
				"value": map[string]interface{}{
					"sessionId":    "s",
					"capabilities": map[string]interface{}{"browserName": "chrome", "browserVersion": "120.0.0"},
				},
			}), nil
		}
		return fpageJSON(map[string]interface{}{
			"status":    0,
			"sessionId": "s",
			"value":     map[string]interface{}{"browserName": "chrome", "version": "120.0.0"},
		}), nil
	case r.Method == "POST" && strings.HasPrefix(r.URL.Path, "/session/s/execute"):
		return s.execute(r), nil
	case r.Method == "GET" && r.URL.Path == "/session/s/screenshot":
		return s.screenshot(), nil
	}
	return nil, fmt.Errorf("fpage fixture: unexpected request %s %s", r.Method, r.URL.Path)
}

func (s *fpageServer) execute(r *http.Request) *http.Response {
	var body struct {
		Script string        `json:"script"`
		Args   []interface{} `json:"args"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		return s.errorResponse("fixture: cannot decode execute body: %v", err)
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	s.executePaths = append(s.executePaths, r.URL.Path)

	switch {
	case strings.Contains(body.Script, "window.scrollTo"):
		if len(body.Args) != 2 {
			return s.errorResponse("fixture: scroll script got %d args", len(body.Args))
		}
		reqX, okX := body.Args[0].(float64)
		reqY, okY := body.Args[1].(float64)
		if !okX || !okY {
			return s.errorResponse("fixture: scroll arguments are not numbers")
		}
		s.scrollCalls++
		n := s.scrollCalls
		var actualX, actualY float64
		var err error
		if s.scrollHook != nil {
			actualX, actualY, err = s.scrollHook(s, n, reqX, reqY)
		} else {
			actualX, actualY = s.defaultScroll(reqX, reqY)
		}
		if err != nil {
			return s.errorResponse("fixture: scroll call %d: %v", n, err)
		}
		s.scrollRequests = append(s.scrollRequests, [2]float64{reqX, reqY})
		s.scrollActuals = append(s.scrollActuals, [2]float64{actualX, actualY})
		s.x, s.y = actualX, actualY
		return s.valueResponse(nil)
	case strings.Contains(body.Script, "document.documentElement.scrollWidth"):
		s.metricsCalls++
		n := s.metricsCalls
		if s.metricsHook != nil {
			s.metricsHook(s, n)
		}
		if s.metricsReply != nil {
			if reply := s.metricsReply(s, n); reply != nil {
				return s.valueResponse(reply)
			}
		}
		return s.valueResponse(map[string]interface{}{
			"width":          s.cfg.width,
			"height":         s.cfg.height,
			"viewportWidth":  s.cfg.viewportWidth,
			"viewportHeight": s.cfg.viewportHeight,
			"x":              s.x,
			"y":              s.y,
			"dpr":            s.cfg.dpr,
		})
	}
	return s.errorResponse("fixture: unexpected script")
}

func (s *fpageServer) screenshot() *http.Response {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.screenshotCalls++
	n := s.screenshotCalls
	originX := fpageQ(s.t, s.x, s.cfg.dpr)
	originY := fpageQ(s.t, s.y, s.cfg.dpr)
	s.screenshotOrigins = append(s.screenshotOrigins, [2]uint64{originX, originY})

	var data []byte
	var err error
	if s.screenshotHook != nil {
		data, err = s.screenshotHook(s, n)
	}
	if err != nil {
		return s.errorResponse("fixture: screenshot call %d: %v", n, err)
	}
	if data == nil {
		data = s.tilePNG(originX, originY)
	}
	s.lastScreenshotSize = [2]uint64{
		fpageQ(s.t, s.cfg.viewportWidth, s.cfg.dpr),
		fpageQ(s.t, s.cfg.viewportHeight, s.cfg.dpr),
	}
	return s.valueResponse(base64.StdEncoding.EncodeToString(data))
}

// tilePNG renders a viewport tile. Pixel (u,v) is colored by the global canvas
// coordinate (originX+u, originY+v), following the documented Q origin
// convention.
func (s *fpageServer) tilePNG(originX, originY uint64) []byte {
	width := fpageQ(s.t, s.cfg.viewportWidth, s.cfg.dpr)
	height := fpageQ(s.t, s.cfg.viewportHeight, s.cfg.dpr)
	img := image.NewNRGBA(image.Rect(0, 0, int(width), int(height)))
	for v := uint64(0); v < height; v++ {
		for u := uint64(0); u < width; u++ {
			c := fpageColor(originX+u, originY+v)
			offset := int(v)*img.Stride + int(u)*4
			img.Pix[offset] = c.R
			img.Pix[offset+1] = c.G
			img.Pix[offset+2] = c.B
			img.Pix[offset+3] = c.A
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		s.t.Fatalf("fixture: encoding tile: %v", err)
	}
	return buf.Bytes()
}

func (s *fpageServer) install(t *testing.T) {
	t.Helper()
	old := selenium.HTTPClient
	selenium.HTTPClient = &http.Client{Transport: fpageTransport(s.roundTrip)}
	t.Cleanup(func() { selenium.HTTPClient = old })
}

func (s *fpageServer) newRemote(t *testing.T) selenium.WebDriver {
	t.Helper()
	wd, err := selenium.NewRemote(selenium.Capabilities{"browserName": "chrome"}, "http://fpage.invalid")
	if err != nil {
		t.Fatalf("NewRemote: %v", err)
	}
	if got := wd.SessionID(); got != "s" {
		t.Fatalf("SessionID = %q, want %q", got, "s")
	}
	return wd
}

func (s *fpageServer) snapshot() (x, y float64, scrolls, metrics, shots int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.x, s.y, s.scrollCalls, s.metricsCalls, s.screenshotCalls
}

// fpageCheckCanvas compares the complete decoded output against an
// independently rendered reference image, pixel by pixel.
func fpageCheckCanvas(t *testing.T, data []byte, cfg fpageConfig) {
	t.Helper()
	img, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("decoding output PNG: %v", err)
	}
	wantW := fpageQ(t, cfg.width, cfg.dpr)
	wantH := fpageQ(t, cfg.height, cfg.dpr)
	if got := img.Bounds(); got != image.Rect(0, 0, int(wantW), int(wantH)) {
		t.Fatalf("output bounds = %v, want %dx%d", got, wantW, wantH)
	}
	for y := uint64(0); y < wantH; y++ {
		for x := uint64(0); x < wantW; x++ {
			want := fpageColor(x, y)
			got := color.NRGBAModel.Convert(img.At(int(x), int(y))).(color.NRGBA)
			if got != want {
				t.Fatalf("pixel (%d,%d) = %+v, want %+v", x, y, got, want)
			}
		}
	}
}

func fpageEncodePNG(t *testing.T, img image.Image) []byte {
	t.Helper()
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatalf("encoding fixture PNG: %v", err)
	}
	return buf.Bytes()
}

func TestFullPageGeometry(t *testing.T) {
	const generousPixels = 1 << 20

	cases := []struct {
		name string
		cfg  fpageConfig
		// maxPixels/maxTiles override generous defaults when non-zero.
		maxPixels uint64
		maxTiles  int
		configure func(*fpageServer)
		// wantRequestMismatch asserts at least one scroll request differed
		// from the position the browser actually reached.
		wantRequestMismatch bool
	}{
		{
			name: "wide_and_tall",
			cfg:  fpageConfig{width: 7, height: 5, viewportWidth: 3, viewportHeight: 2, dpr: 1, startX: 2, startY: 1},
		},
		{
			name: "bottom_right_clamp_dpr2",
			cfg:  fpageConfig{width: 8, height: 6, viewportWidth: 5, viewportHeight: 4, dpr: 2, startX: 1, startY: 2},
			configure: func(s *fpageServer) {
				// The browser reports a half-pixel snap on the first scroll,
				// so the position it actually reaches differs from the one
				// that was requested.
				s.scrollHook = func(srv *fpageServer, n int, reqX, reqY float64) (float64, float64, error) {
					x, y := srv.defaultScroll(reqX, reqY)
					if n == 1 {
						x = math.Min(x+0.5, srv.cfg.width-srv.cfg.viewportWidth)
					}
					return x, y, nil
				}
			},
			wantRequestMismatch: true,
		},
		{
			name: "dpr_1_25_non_divisible",
			cfg:  fpageConfig{width: 6.6, height: 4.6, viewportWidth: 2.5, viewportHeight: 1.7, dpr: 1.25, startX: 0.5, startY: 0.5},
		},
		{
			name: "dpr_1_5_non_divisible",
			cfg:  fpageConfig{width: 5.2, height: 3.3, viewportWidth: 1.9, viewportHeight: 1.3, dpr: 1.5},
		},
		{
			name: "width_only",
			cfg:  fpageConfig{width: 8, height: 3, viewportWidth: 3, viewportHeight: 3, dpr: 1, startX: 1, startY: 0},
		},
		{
			name: "height_only",
			cfg:  fpageConfig{width: 3, height: 8, viewportWidth: 3, viewportHeight: 2, dpr: 1},
		},
		{
			name: "content_smaller_than_viewport",
			cfg:  fpageConfig{width: 2.4, height: 1.6, viewportWidth: 5, viewportHeight: 3, dpr: 1, startX: 0, startY: 0},
		},
		{
			name: "quantized_scroll_and_gap_compensation",
			cfg:  fpageConfig{width: 8.5, height: 6.5, viewportWidth: 2.5, viewportHeight: 1.5, dpr: 1.5, startX: 0, startY: 0},
			configure: func(s *fpageServer) {
				// Snap the actual scroll position to whole CSS pixels. This
				// makes the reached position differ from the request and can
				// land a tile short of, or past, the pixel that still needs
				// coverage.
				s.scrollHook = func(srv *fpageServer, n int, reqX, reqY float64) (float64, float64, error) {
					maxX := srv.cfg.width - srv.cfg.viewportWidth
					if maxX < 0 {
						maxX = 0
					}
					maxY := srv.cfg.height - srv.cfg.viewportHeight
					if maxY < 0 {
						maxY = 0
					}
					x := math.Min(math.Max(math.Round(reqX), 0), maxX)
					y := math.Min(math.Max(math.Round(reqY), 0), maxY)
					return x, y, nil
				}
			},
			wantRequestMismatch: true,
		},
		{
			name: "unreachable_request_positions",
			cfg:  fpageConfig{width: 7, height: 5, viewportWidth: 3, viewportHeight: 2, dpr: 1},
			configure: func(s *fpageServer) {
				// Refuse odd scroll positions, as a page with scroll snapping
				// might, except at the maximum clamp position. Requests at odd
				// coordinates must be compensated for so that every pixel
				// still gets a source tile.
				s.scrollHook = func(srv *fpageServer, n int, reqX, reqY float64) (float64, float64, error) {
					x, y := srv.defaultScroll(reqX, reqY)
					maxX := srv.cfg.width - srv.cfg.viewportWidth
					maxY := srv.cfg.height - srv.cfg.viewportHeight
					if x < maxX {
						x = math.Floor(x/2) * 2
					}
					if y < maxY {
						y = math.Floor(y/2) * 2
					}
					return x, y, nil
				}
			},
			wantRequestMismatch: true,
		},
	}

	for _, tc := range cases {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			server := newFpageServer(t, tc.cfg, false)
			if tc.configure != nil {
				tc.configure(server)
			}
			server.install(t)
			wd := server.newRemote(t)

			maxPixels := tc.maxPixels
			if maxPixels == 0 {
				maxPixels = generousPixels
			}
			maxTiles := tc.maxTiles
			if maxTiles == 0 {
				maxTiles = 1000
			}
			data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{
				MaxPixels: maxPixels,
				MaxTiles:  maxTiles,
			})
			if err != nil {
				t.Fatalf("FullPageScreenshot: %v", err)
			}
			fpageCheckCanvas(t, data, tc.cfg)

			server.mu.Lock()
			x, y := server.x, server.y
			shots := server.screenshotCalls
			scrolls := server.scrollCalls
			requests := append([][2]float64(nil), server.scrollRequests...)
			actuals := append([][2]float64(nil), server.scrollActuals...)
			origins := append([][2]uint64(nil), server.screenshotOrigins...)
			lastReq := requests[len(requests)-1]
			server.mu.Unlock()

			if x != tc.cfg.startX || y != tc.cfg.startY {
				t.Fatalf("scroll position after capture = (%g,%g), want original (%g,%g)", x, y, tc.cfg.startX, tc.cfg.startY)
			}
			if lastReq != [2]float64{tc.cfg.startX, tc.cfg.startY} {
				t.Fatalf("last scroll request = %v, want restore to original %v", lastReq, [2]float64{tc.cfg.startX, tc.cfg.startY})
			}
			if shots < 1 || shots > maxTiles {
				t.Fatalf("screenshot calls = %d, want between 1 and %d", shots, maxTiles)
			}
			if shots != len(origins) {
				t.Fatalf("recorded %d screenshot origins for %d calls", len(origins), shots)
			}
			// Every screenshot must have been positioned at Q(actual scroll)
			// for the position the browser reported at that moment.
			for i, origin := range origins {
				if i >= len(actuals) {
					t.Fatalf("screenshot %d has no scroll actual", i)
				}
				wantX := fpageQ(t, actuals[i][0], tc.cfg.dpr)
				wantY := fpageQ(t, actuals[i][1], tc.cfg.dpr)
				if origin != [2]uint64{wantX, wantY} {
					t.Fatalf("screenshot %d origin = %v, want Q(actual %v) = (%d,%d)", i, origin, actuals[i], wantX, wantY)
				}
			}
			if tc.wantRequestMismatch {
				mismatch := false
				for i := 0; i < len(requests) && i < len(actuals); i++ {
					if requests[i] != actuals[i] {
						mismatch = true
						break
					}
				}
				if !mismatch {
					t.Fatalf("fixture never made the actual position differ from the request")
				}
			}
			if scrolls != len(requests) {
				t.Fatalf("scroll calls = %d but %d requests recorded", scrolls, len(requests))
			}
			// The final scroll is the restoration, and it must not be the
			// position of the last capture request.
			if len(requests) < 2 {
				t.Fatalf("expected at least one capture scroll and a restore, got %d scrolls", len(requests))
			}
			if lastReq == requests[len(requests)-2] && lastReq != [2]float64{tc.cfg.startX, tc.cfg.startY} {
				t.Fatalf("restore duplicated the last capture request %v", lastReq)
			}
		})
	}
}

func TestFullPageFailure(t *testing.T) {
	base := fpageConfig{width: 6, height: 4, viewportWidth: 3, viewportHeight: 2, dpr: 1}

	t.Run("invalid_options", func(t *testing.T) {
		server := newFpageServer(t, base, false)
		server.install(t)
		wd := server.newRemote(t)
		for _, opts := range []selenium.FullPageScreenshotOptions{
			{MaxPixels: 0, MaxTiles: 1},
			{MaxPixels: 1, MaxTiles: 0},
			{MaxPixels: 0, MaxTiles: 0},
		} {
			data, err := wd.FullPageScreenshot(opts)
			if err == nil || data != nil {
				t.Fatalf("FullPageScreenshot(%+v) = %v bytes, %v; want nil, error", opts, len(data), err)
			}
		}
		if _, _, scrolls, metrics, shots := server.snapshot(); scrolls != 0 || metrics != 0 || shots != 0 {
			t.Fatalf("invalid options touched the remote end: scrolls=%d metrics=%d shots=%d", scrolls, metrics, shots)
		}
	})

	t.Run("max_pixels_rejected_before_scroll", func(t *testing.T) {
		server := newFpageServer(t, base, false)
		server.install(t)
		wd := server.newRemote(t)
		data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 10, MaxTiles: 20})
		if err == nil || data != nil {
			t.Fatalf("got %v bytes, %v; want nil, error", len(data), err)
		}
		if _, _, scrolls, _, shots := server.snapshot(); scrolls != 0 || shots != 0 {
			t.Fatalf("scrolls=%d shots=%d after MaxPixels rejection, want 0/0", scrolls, shots)
		}
	})

	t.Run("oversized_viewport_rejected_before_scroll", func(t *testing.T) {
		// The content fits in MaxPixels, but the viewport tile does not.
		cfg := fpageConfig{width: 2, height: 2, viewportWidth: 10, viewportHeight: 10, dpr: 1}
		server := newFpageServer(t, cfg, false)
		server.install(t)
		wd := server.newRemote(t)
		data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 50, MaxTiles: 20})
		if err == nil || data != nil {
			t.Fatalf("got %v bytes, %v; want nil, error", len(data), err)
		}
		if _, _, scrolls, _, shots := server.snapshot(); scrolls != 0 || shots != 0 {
			t.Fatalf("scrolls=%d shots=%d after viewport rejection, want 0/0", scrolls, shots)
		}
	})

	t.Run("non_finite_metrics", func(t *testing.T) {
		overrides := []map[string]interface{}{
			{"width": nil, "height": 4.0, "viewportWidth": 3.0, "viewportHeight": 2.0, "x": 0.0, "y": 0.0, "dpr": 1.0},
			{"width": 6.0, "height": 4.0, "viewportWidth": 3.0, "viewportHeight": 2.0, "x": 0.0, "y": 0.0, "dpr": 0.0},
			{"width": 6.0, "height": 4.0, "viewportWidth": 3.0, "viewportHeight": 2.0, "x": -1.0, "y": 0.0, "dpr": 1.0},
			{"width": 6.0, "height": 4.0, "viewportWidth": 3.0, "viewportHeight": 2.0, "x": 0.0, "y": 0.0, "dpr": "NaN"},
		}
		for i, override := range overrides {
			server := newFpageServer(t, base, false)
			server.metricsReply = func(s *fpageServer, n int) map[string]interface{} { return override }
			server.install(t)
			wd := server.newRemote(t)
			data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 1000, MaxTiles: 20})
			if err == nil || data != nil {
				t.Fatalf("override %d: got %v bytes, %v; want nil, error", i, len(data), err)
			}
			if _, _, scrolls, _, shots := server.snapshot(); scrolls != 0 || shots != 0 {
				t.Fatalf("override %d touched scrolling: scrolls=%d shots=%d", i, scrolls, shots)
			}
		}
	})

	t.Run("max_tiles_exceeded", func(t *testing.T) {
		server := newFpageServer(t, base, false)
		server.install(t)
		wd := server.newRemote(t)
		data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 1000, MaxTiles: 1})
		if err == nil || data != nil {
			t.Fatalf("got %v bytes, %v; want nil, error", len(data), err)
		}
		if _, _, _, _, shots := server.snapshot(); shots != 1 {
			t.Fatalf("screenshot calls = %d, want exactly MaxTiles (1)", shots)
		}
		if x, y, _, _, _ := server.snapshot(); x != 0 || y != 0 {
			t.Fatalf("position after failure = (%g,%g), want (0,0)", x, y)
		}
	})

	t.Run("failed_calls_count_towards_max_tiles", func(t *testing.T) {
		server := newFpageServer(t, base, false)
		server.screenshotHook = func(s *fpageServer, n int) ([]byte, error) {
			return nil, errors.New("synthetic screenshot failure")
		}
		server.install(t)
		wd := server.newRemote(t)
		data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 1000, MaxTiles: 3})
		if err == nil || data != nil {
			t.Fatalf("got %v bytes, %v; want nil, error", len(data), err)
		}
		if _, _, _, _, shots := server.snapshot(); shots != 1 {
			t.Fatalf("screenshot calls = %d, want 1 because the call failed immediately", shots)
		}
	})

	t.Run("bad_png", func(t *testing.T) {
		server := newFpageServer(t, base, false)
		server.screenshotHook = func(s *fpageServer, n int) ([]byte, error) {
			if n == 1 {
				return []byte("this is not a PNG"), nil
			}
			return nil, nil
		}
		server.install(t)
		wd := server.newRemote(t)
		data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 1000, MaxTiles: 20})
		if err == nil || data != nil {
			t.Fatalf("got %v bytes, %v; want nil, error", len(data), err)
		}
		if _, _, _, _, shots := server.snapshot(); shots != 1 {
			t.Fatalf("screenshot calls = %d, want 1", shots)
		}
		if x, y, _, _, _ := server.snapshot(); x != 0 || y != 0 {
			t.Fatalf("position after failure = (%g,%g), want (0,0)", x, y)
		}
	})

	t.Run("wrong_png_size_not_scaled", func(t *testing.T) {
		server := newFpageServer(t, base, false)
		server.screenshotHook = func(s *fpageServer, n int) ([]byte, error) {
			if n == 1 {
				return fpageEncodePNG(t, image.NewNRGBA(image.Rect(0, 0, 4, 2))), nil
			}
			return nil, nil
		}
		server.install(t)
		wd := server.newRemote(t)
		data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 1000, MaxTiles: 20})
		if err == nil || data != nil {
			t.Fatalf("got %v bytes, %v; want nil, error", len(data), err)
		}
		if x, y, _, _, _ := server.snapshot(); x != 0 || y != 0 {
			t.Fatalf("position after failure = (%g,%g), want (0,0)", x, y)
		}
	})

	t.Run("layout_change", func(t *testing.T) {
		cfg := fpageConfig{width: 6, height: 4, viewportWidth: 3, viewportHeight: 2, dpr: 1, startX: 1, startY: 1}
		server := newFpageServer(t, cfg, false)
		server.metricsHook = func(s *fpageServer, n int) {
			if n >= 2 {
				s.cfg.width = 9
			}
		}
		server.install(t)
		wd := server.newRemote(t)
		data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 1000, MaxTiles: 20})
		if err == nil || data != nil {
			t.Fatalf("got %v bytes, %v; want nil, error", len(data), err)
		}
		x, y, scrolls, _, shots := server.snapshot()
		if shots != 0 {
			t.Fatalf("screenshot calls = %d, want 0: the layout change must be detected before the screenshot", shots)
		}
		if scrolls != 2 {
			t.Fatalf("scroll calls = %d, want one capture scroll and one restore", scrolls)
		}
		if x != cfg.startX || y != cfg.startY {
			t.Fatalf("position after layout change = (%g,%g), want original (%g,%g)", x, y, cfg.startX, cfg.startY)
		}
	})

	t.Run("dpr_change", func(t *testing.T) {
		cfg := fpageConfig{width: 6, height: 4, viewportWidth: 3, viewportHeight: 2, dpr: 1, startX: 1, startY: 1}
		server := newFpageServer(t, cfg, false)
		server.metricsHook = func(s *fpageServer, n int) {
			if n >= 2 {
				s.cfg.dpr = 2
			}
		}
		server.install(t)
		wd := server.newRemote(t)
		data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 1000, MaxTiles: 20})
		if err == nil || data != nil {
			t.Fatalf("got %v bytes, %v; want nil, error", len(data), err)
		}
		x, y, scrolls, _, shots := server.snapshot()
		if shots != 0 {
			t.Fatalf("screenshot calls = %d, want 0: the DPR change must be detected before the screenshot", shots)
		}
		if scrolls != 2 {
			t.Fatalf("scroll calls = %d, want one capture scroll and one restore", scrolls)
		}
		if x != cfg.startX || y != cfg.startY {
			t.Fatalf("position after DPR change = (%g,%g), want original (%g,%g)", x, y, cfg.startX, cfg.startY)
		}
	})

	t.Run("persistent_no_progress", func(t *testing.T) {
		server := newFpageServer(t, base, false)
		server.scrollHook = func(s *fpageServer, n int, reqX, reqY float64) (float64, float64, error) {
			// The page refuses to scroll at all.
			return 0, 0, nil
		}
		server.install(t)
		wd := server.newRemote(t)
		data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 1000, MaxTiles: 200})
		if err == nil || data != nil {
			t.Fatalf("got %v bytes, %v; want nil, error", len(data), err)
		}
		x, y, _, _, shots := server.snapshot()
		if shots < 2 || shots > 200 {
			t.Fatalf("screenshot calls = %d, want bounded retries within MaxTiles", shots)
		}
		if x != 0 || y != 0 {
			t.Fatalf("position after failure = (%g,%g), want (0,0)", x, y)
		}
	})

	t.Run("screenshot_failure_and_restore_failure", func(t *testing.T) {
		cfg := fpageConfig{width: 6, height: 4, viewportWidth: 3, viewportHeight: 2, dpr: 1, startX: 1, startY: 1}
		server := newFpageServer(t, cfg, false)
		server.screenshotHook = func(s *fpageServer, n int) ([]byte, error) {
			return nil, errors.New("synthetic screenshot failure")
		}
		server.scrollHook = func(s *fpageServer, n int, reqX, reqY float64) (float64, float64, error) {
			if n == 1 {
				x, y := s.defaultScroll(reqX, reqY)
				return x, y, nil
			}
			// The restore scroll fails too.
			return 0, 0, errors.New("synthetic restore failure")
		}
		server.install(t)
		wd := server.newRemote(t)
		data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 1000, MaxTiles: 20})
		if err == nil || data != nil {
			t.Fatalf("got %v bytes, %v; want nil, error", len(data), err)
		}
		msg := err.Error()
		if !strings.Contains(msg, "synthetic screenshot failure") || !strings.Contains(msg, "synthetic restore failure") {
			t.Fatalf("error %q does not retain both the capture and the restore failure", msg)
		}
		if _, _, scrolls, _, _ := server.snapshot(); scrolls != 2 {
			t.Fatalf("scroll calls = %d, want one capture scroll and one restore attempt", scrolls)
		}
	})

	t.Run("successful_capture_but_restore_mismatch", func(t *testing.T) {
		cfg := fpageConfig{width: 6, height: 4, viewportWidth: 3, viewportHeight: 2, dpr: 1, startX: 1, startY: 1}
		server := newFpageServer(t, cfg, false)
		restoreCall := 0
		server.scrollHook = func(s *fpageServer, n int, reqX, reqY float64) (float64, float64, error) {
			x, y := s.defaultScroll(reqX, reqY)
			// Capture never requests the original (1,1) position, so a
			// request for it after the first scroll is the restore. Silently
			// ignore it, which leaves the page at its last captured position.
			if n > 1 && reqX == cfg.startX && reqY == cfg.startY {
				restoreCall = n
				return s.x, s.y, nil
			}
			return x, y, nil
		}
		server.install(t)
		wd := server.newRemote(t)
		data, err := wd.FullPageScreenshot(selenium.FullPageScreenshotOptions{MaxPixels: 1000, MaxTiles: 20})
		if err == nil || data != nil {
			t.Fatalf("got %v bytes, %v; want nil, error", len(data), err)
		}
		if restoreCall == 0 {
			t.Fatalf("restore scroll was never attempted")
		}
		if !strings.Contains(err.Error(), "restore") {
			t.Fatalf("error %q does not describe the restore failure", err)
		}
	})
}

func TestFullPageDialects(t *testing.T) {
	cfg := fpageConfig{width: 6, height: 4, viewportWidth: 3, viewportHeight: 2, dpr: 1, startX: 1, startY: 1}

	for _, w3c := range []bool{false, true} {
		w3c := w3c
		name := "json_wire"
		wantExecutePath := "/session/s/execute"
		if w3c {
			name = "w3c"
			wantExecutePath = "/session/s/execute/sync"
		}
		t.Run(name, func(t *testing.T) {
			server := newFpageServer(t, cfg, w3c)
			server.install(t)
			wd := server.newRemote(t)
			opts := selenium.FullPageScreenshotOptions{MaxPixels: 1000, MaxTiles: 100}

			first, err := wd.FullPageScreenshot(opts)
			if err != nil {
				t.Fatalf("first FullPageScreenshot: %v", err)
			}
			fpageCheckCanvas(t, first, cfg)

			// Every execute, including both the capture scrolls and the
			// restore, must use this session's dialect.
			server.mu.Lock()
			paths := append([]string(nil), server.executePaths...)
			firstShots := server.screenshotCalls
			restored := [2]float64{server.x, server.y}
			server.mu.Unlock()
			for _, path := range paths {
				if path != wantExecutePath {
					t.Fatalf("execute path = %q, want %q for this dialect", path, wantExecutePath)
				}
			}
			if restored != [2]float64{cfg.startX, cfg.startY} {
				t.Fatalf("position after first call = %v, want %v", restored, [2]float64{cfg.startX, cfg.startY})
			}

			// A second consecutive call must produce the same image and must
			// not inherit any planner or scroll state from the first.
			second, err := wd.FullPageScreenshot(opts)
			if err != nil {
				t.Fatalf("second FullPageScreenshot: %v", err)
			}
			fpageCheckCanvas(t, second, cfg)
			if !bytes.Equal(first, second) {
				t.Fatalf("consecutive calls produced different PNGs")
			}
			server.mu.Lock()
			secondShots := server.screenshotCalls
			restored = [2]float64{server.x, server.y}
			paths = append([]string(nil), server.executePaths...)
			server.mu.Unlock()
			if secondShots <= firstShots {
				t.Fatalf("second call took no new screenshots: %d then %d", firstShots, secondShots)
			}
			if restored != [2]float64{cfg.startX, cfg.startY} {
				t.Fatalf("position after second call = %v, want %v", restored, [2]float64{cfg.startX, cfg.startY})
			}

			// A failed call must restore the scroll position and must not
			// poison the next call.
			failAt := secondShots + 1
			server.mu.Lock()
			server.screenshotHook = func(s *fpageServer, n int) ([]byte, error) {
				if n == failAt {
					return nil, errors.New("injected dialect failure")
				}
				return nil, nil
			}
			server.mu.Unlock()
			data, err := wd.FullPageScreenshot(opts)
			if err == nil || data != nil {
				t.Fatalf("injected failure returned %v bytes, %v; want nil, error", len(data), err)
			}
			server.mu.Lock()
			restored = [2]float64{server.x, server.y}
			paths = append([]string(nil), server.executePaths...)
			server.mu.Unlock()
			if restored != [2]float64{cfg.startX, cfg.startY} {
				t.Fatalf("position after failed call = %v, want %v", restored, [2]float64{cfg.startX, cfg.startY})
			}
			for _, path := range paths {
				if path != wantExecutePath {
					t.Fatalf("execute path after failure = %q, want %q", path, wantExecutePath)
				}
			}

			server.mu.Lock()
			server.screenshotHook = nil
			server.mu.Unlock()
			third, err := wd.FullPageScreenshot(opts)
			if err != nil {
				t.Fatalf("call after failure: %v", err)
			}
			fpageCheckCanvas(t, third, cfg)
			if !bytes.Equal(first, third) {
				t.Fatalf("call after failure produced a different PNG")
			}
			server.mu.Lock()
			restored = [2]float64{server.x, server.y}
			server.mu.Unlock()
			if restored != [2]float64{cfg.startX, cfg.startY} {
				t.Fatalf("position after recovery call = %v, want %v", restored, [2]float64{cfg.startX, cfg.startY})
			}

			// A separate session gets a separate fixture and must not observe
			// any state from this one.
			fresh := newFpageServer(t, cfg, w3c)
			fresh.install(t)
			freshWD := fresh.newRemote(t)
			if freshWD.SessionID() == "" {
				t.Fatalf("fresh session has no ID")
			}
			freshData, err := freshWD.FullPageScreenshot(opts)
			if err != nil {
				t.Fatalf("fresh session FullPageScreenshot: %v", err)
			}
			fpageCheckCanvas(t, freshData, cfg)
			fresh.mu.Lock()
			if fresh.metricsCalls == 0 || fresh.screenshotCalls == 0 {
				t.Fatalf("fresh session reused no fixture state: metrics=%d shots=%d", fresh.metricsCalls, fresh.screenshotCalls)
			}
			if fresh.x != cfg.startX || fresh.y != cfg.startY {
				t.Fatalf("fresh session position = (%g,%g), want original", fresh.x, fresh.y)
			}
			fresh.mu.Unlock()
		})
	}
}
