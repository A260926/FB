package selenium

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"math"
)

// FullPageScreenshotOptions configures FullPageScreenshot.
//
// Both fields must be positive. MaxPixels bounds the number of pixels in the
// returned image as well as the size of any single decoded tile, so that a
// hostile or broken remote end cannot force a huge allocation. MaxTiles bounds
// the number of Screenshot protocol calls, including failed calls.
type FullPageScreenshotOptions struct {
	MaxPixels uint64
	MaxTiles  int
}

const (
	// fullPageMetricsScript reads the metrics used to plan and validate the
	// capture. It is an ordinary top-level document script; frames are not
	// entered.
	fullPageMetricsScript = `return {
	width: document.documentElement.scrollWidth,
	height: document.documentElement.scrollHeight,
	viewportWidth: window.innerWidth,
	viewportHeight: window.innerHeight,
	x: window.scrollX,
	y: window.scrollY,
	dpr: window.devicePixelRatio
};`

	// fullPageScrollScript requests a scroll position. The remote end is free
	// to clamp it; callers must read back the actual position.
	fullPageScrollScript = `window.scrollTo(arguments[0], arguments[1]); return null;`

	// fullPageMaxAttempts bounds the number of retries for a single uncovered
	// output pixel while compensating for a remote end that clamps or
	// quantizes scroll positions. Together with MaxTiles this keeps a
	// pathological page from causing an unbounded loop.
	fullPageMaxAttempts = 32

	// fullPageMaxExactInt is the largest integer for which float64 arithmetic
	// can represent every integer exactly. Above this, a value returned by the
	// browser is no longer reliable as a pixel boundary.
	fullPageMaxExactInt = 1 << 53
)

// fullPageMetrics is a snapshot of the metrics that FullPageScreenshot relies
// on. All values are CSS coordinates, except dpr which is a ratio.
type fullPageMetrics struct {
	width          float64
	height         float64
	viewportWidth  float64
	viewportHeight float64
	x              float64
	y              float64
	dpr            float64
}

// FullPageScreenshot captures the whole top-level document as a single PNG.
//
// The capture is built from ordinary viewport screenshots obtained through the
// WebDriver screenshot command. The page is scrolled in row-major order and
// the resulting tiles are stitched into a canvas whose dimensions are derived
// from the document size and the device pixel ratio:
//
//	Q(t) = floor(t*r + 0.5)
//	canvas = Q(width) x Q(height)
//	tile   = Q(viewportWidth) x Q(viewportHeight)
//
// Local pixel (u, v) of a tile whose actual scroll position is (x, y) maps to
// canvas pixel (Q(x)+u, Q(y)+v). Pixels are copied only where the tile and the
// canvas overlap. Overlapping pixels belong to the first tile, in capture
// order, that covers them; they are never blended. The last row and column and
// pages smaller than a viewport use the same quantization rule; tail and
// overlap widths are never rounded separately.
//
// The capture assumes a static page: document layout and viewport dimensions
// must not change, there must be no fixed or sticky elements, scroll
// animations, smooth scrolling or scroll snapping, no iframe traversal and no
// browser zoom animation or mobile visualViewport handling. Scrolling is
// performed with ExecuteScript, so both the original JSON Wire protocol and
// W3C protocol sessions are supported without CDP.
//
// The original scroll position is restored before returning. On any failure,
// including failure to restore the scroll position, the result is nil.
func (wd *remoteWD) FullPageScreenshot(options FullPageScreenshotOptions) (result []byte, retErr error) {
	if options.MaxPixels == 0 {
		return nil, fmt.Errorf("full page screenshot: MaxPixels must be positive")
	}
	if options.MaxTiles <= 0 {
		return nil, fmt.Errorf("full page screenshot: MaxTiles must be positive")
	}

	initial, err := wd.fullPageReadMetrics()
	if err != nil {
		return nil, fmt.Errorf("full page screenshot: reading initial metrics: %w", err)
	}
	if err := initial.validate(); err != nil {
		return nil, fmt.Errorf("full page screenshot: invalid initial metrics: %w", err)
	}

	canvasWidth, canvasHeight, err := fullPageCanvasSize(initial, options.MaxPixels)
	if err != nil {
		return nil, fmt.Errorf("full page screenshot: %w", err)
	}
	tileSize, err := fullPageViewportPixels(initial, options.MaxPixels)
	if err != nil {
		return nil, fmt.Errorf("full page screenshot: %w", err)
	}
	maxScrollX := initial.width - initial.viewportWidth
	if maxScrollX < 0 {
		maxScrollX = 0
	}
	maxScrollY := initial.height - initial.viewportHeight
	if maxScrollY < 0 {
		maxScrollY = 0
	}

	// All checks that can be performed without touching the page have passed.
	// Allocate the output canvas and its coverage record now; both are bounded
	// by MaxPixels, and no tile copies are retained.
	canvas := image.NewNRGBA(image.Rect(0, 0, int(canvasWidth), int(canvasHeight)))
	totalPixels := canvasWidth * canvasHeight
	coverage := newFullPageCoverage(totalPixels)

	originalX, originalY := initial.x, initial.y
	startedScrolling := false
	var coveredPixels uint64

	defer func() {
		if !startedScrolling {
			return
		}
		restoreErr := wd.fullPageRestoreScroll(originalX, originalY)
		if restoreErr == nil {
			return
		}
		// Never deliver a partial or unverified image. Keep both errors when
		// the capture and the restore both failed.
		result = nil
		if retErr == nil {
			retErr = fmt.Errorf("full page screenshot: could not restore scroll position: %w", restoreErr)
		} else {
			retErr = fmt.Errorf("%v; additionally could not restore scroll position: %w", retErr, restoreErr)
		}
	}()

	cursor := uint64(0)
	retryCursor := uint64(math.MaxUint64)
	var biasX, biasY int64
	attempts := 0
	screenshotCalls := 0

	for coveredPixels < totalPixels {
		if screenshotCalls >= options.MaxTiles {
			return nil, fmt.Errorf("full page screenshot: coverage incomplete after %d of %d screenshot calls", screenshotCalls, options.MaxTiles)
		}
		for cursor < totalPixels && coverage.has(cursor) {
			cursor++
		}
		if cursor >= totalPixels {
			break
		}
		if cursor != retryCursor {
			retryCursor = cursor
			biasX, biasY = 0, 0
			attempts = 0
		}
		if attempts >= fullPageMaxAttempts {
			tx, ty := cursor%canvasWidth, cursor/canvasWidth
			return nil, fmt.Errorf("full page screenshot: could not cover pixel (%d,%d) after %d attempts", tx, ty, attempts)
		}

		targetX, targetY := cursor%canvasWidth, cursor/canvasWidth
		requestX := fullPageScrollRequest(targetX, biasX, initial.dpr, maxScrollX)
		requestY := fullPageScrollRequest(targetY, biasY, initial.dpr, maxScrollY)

		// From here on the page may have moved: restoration is mandatory no
		// matter how the capture ends.
		startedScrolling = true
		if err := wd.fullPageScroll(requestX, requestY); err != nil {
			return nil, fmt.Errorf("full page screenshot: scroll request to (%g, %g): %w", requestX, requestY, err)
		}
		current, err := wd.fullPageReadMetrics()
		if err != nil {
			return nil, fmt.Errorf("full page screenshot: reading metrics after scroll: %w", err)
		}
		if err := current.validate(); err != nil {
			return nil, fmt.Errorf("full page screenshot: invalid metrics after scroll: %w", err)
		}
		if current.width != initial.width || current.height != initial.height ||
			current.viewportWidth != initial.viewportWidth || current.viewportHeight != initial.viewportHeight ||
			current.dpr != initial.dpr {
			return nil, fmt.Errorf("full page screenshot: document layout or device pixel ratio changed during capture")
		}

		// The tile is positioned using the position the browser actually
		// reached, not the position that was requested.
		originX, err := fullPageQuantize(current.x, initial.dpr)
		if err != nil {
			return nil, fmt.Errorf("full page screenshot: invalid actual scroll x: %w", err)
		}
		originY, err := fullPageQuantize(current.y, initial.dpr)
		if err != nil {
			return nil, fmt.Errorf("full page screenshot: invalid actual scroll y: %w", err)
		}

		// MaxTiles counts actual calls, including ones that fail.
		screenshotCalls++
		data, err := wd.Screenshot()
		if err != nil {
			return nil, fmt.Errorf("full page screenshot: screenshot call %d: %w", screenshotCalls, err)
		}
		tile, err := fullPageDecodeTile(data, tileSize, options.MaxPixels)
		if err != nil {
			return nil, fmt.Errorf("full page screenshot: screenshot call %d: %w", screenshotCalls, err)
		}

		coveredPixels += fullPageBlit(canvas, coverage, canvasWidth, canvasHeight, originX, originY, tile)
		if !coverage.has(cursor) {
			// No progress for the pixel that is currently being targeted.
			// Compensate for clamping or quantization by adjusting the
			// position requested for this same pixel on the next attempt.
			attempts++
			fullPageAdjustBias(&biasX, targetX, originX, tileSize.width)
			fullPageAdjustBias(&biasY, targetY, originY, tileSize.height)
		}
	}

	if coveredPixels != totalPixels {
		return nil, fmt.Errorf("full page screenshot: coverage incomplete: %d of %d pixels covered", coveredPixels, totalPixels)
	}

	var buf bytes.Buffer
	if err := png.Encode(&buf, canvas); err != nil {
		return nil, fmt.Errorf("full page screenshot: encoding PNG: %w", err)
	}
	return buf.Bytes(), nil
}

type fullPageViewport struct {
	width  uint64
	height uint64
}

func (m fullPageMetrics) validate() error {
	values := []struct {
		name string
		v    float64
	}{
		{"width", m.width},
		{"height", m.height},
		{"viewportWidth", m.viewportWidth},
		{"viewportHeight", m.viewportHeight},
		{"devicePixelRatio", m.dpr},
	}
	for _, f := range values {
		if !fullPageFinite(f.v) || f.v <= 0 {
			return fmt.Errorf("%s must be a finite positive number, got %v", f.name, f.v)
		}
	}
	for _, f := range []struct {
		name string
		v    float64
	}{{"x", m.x}, {"y", m.y}} {
		if !fullPageFinite(f.v) || f.v < 0 {
			return fmt.Errorf("%s must be a finite non-negative number, got %v", f.name, f.v)
		}
	}
	return nil
}

func fullPageFinite(v float64) bool {
	return !math.IsNaN(v) && !math.IsInf(v, 0)
}

// fullPageQuantize implements Q(t) = floor(t*r + 0.5). It rejects values that
// cannot be represented exactly as an integer in a float64.
func fullPageQuantize(t, r float64) (uint64, error) {
	if !fullPageFinite(t) || t < 0 {
		return 0, fmt.Errorf("cannot quantize %v", t)
	}
	if !fullPageFinite(r) || r <= 0 {
		return 0, fmt.Errorf("cannot quantize with device pixel ratio %v", r)
	}
	scaled := t * r
	if !fullPageFinite(scaled) {
		return 0, fmt.Errorf("quantization overflow for %v*%v", t, r)
	}
	q := math.Floor(scaled + 0.5)
	if !fullPageFinite(q) || q < 0 || q >= float64(fullPageMaxExactInt) {
		return 0, fmt.Errorf("quantized value %v is out of integer range", q)
	}
	return uint64(q), nil
}

// fullPageCanvasSize validates the output dimensions and arithmetic before any
// allocation or scrolling.
func fullPageCanvasSize(m fullPageMetrics, maxPixels uint64) (uint64, uint64, error) {
	width, err := fullPageQuantize(m.width, m.dpr)
	if err != nil {
		return 0, 0, fmt.Errorf("invalid output width: %w", err)
	}
	height, err := fullPageQuantize(m.height, m.dpr)
	if err != nil {
		return 0, 0, fmt.Errorf("invalid output height: %w", err)
	}
	if width == 0 || height == 0 {
		return 0, 0, fmt.Errorf("output dimensions %dx%d must be positive", width, height)
	}
	if width > maxPixels || height > maxPixels || height > maxPixels/width {
		return 0, 0, fmt.Errorf("output %dx%d exceeds MaxPixels %d", width, height, maxPixels)
	}
	total := width * height
	maxInt := uint64(^uint(0) >> 1)
	if width > maxInt || height > maxInt || total > maxInt/4 {
		return 0, 0, fmt.Errorf("output %dx%d does not fit in the platform integer range", width, height)
	}
	return width, height, nil
}

func fullPageViewportPixels(m fullPageMetrics, maxPixels uint64) (fullPageViewport, error) {
	width, err := fullPageQuantize(m.viewportWidth, m.dpr)
	if err != nil {
		return fullPageViewport{}, fmt.Errorf("invalid viewport width: %w", err)
	}
	height, err := fullPageQuantize(m.viewportHeight, m.dpr)
	if err != nil {
		return fullPageViewport{}, fmt.Errorf("invalid viewport height: %w", err)
	}
	if width == 0 || height == 0 {
		return fullPageViewport{}, fmt.Errorf("viewport dimensions %dx%d must be positive", width, height)
	}
	if width > maxPixels || height > maxPixels || height > maxPixels/width {
		return fullPageViewport{}, fmt.Errorf("viewport tile %dx%d exceeds allocation limit MaxPixels %d", width, height, maxPixels)
	}
	maxInt := uint64(^uint(0) >> 1)
	if width > maxInt || height > maxInt || width*height > maxInt/4 {
		return fullPageViewport{}, fmt.Errorf("viewport tile %dx%d does not fit in the platform integer range", width, height)
	}
	return fullPageViewport{width: width, height: height}, nil
}

func fullPageScrollRequest(target uint64, bias int64, dpr, maxScroll float64) float64 {
	value := int64(target) - bias
	if value < 0 {
		value = 0
	}
	request := float64(value) / dpr
	if request < 0 {
		request = 0
	}
	if request > maxScroll {
		request = maxScroll
	}
	return request
}

func fullPageAdjustBias(bias *int64, target, origin, viewport uint64) {
	t, o, v := int64(target), int64(origin), int64(viewport)
	switch {
	case o > t:
		// The browser is landing after the pixel that still needs coverage.
		*bias += (o - t) + 1
	case o+v <= t:
		// The browser is landing so far before the pixel that the tile does
		// not reach it.
		*bias -= (t - (o + v)) + 1
	default:
		return
	}
	const maxBias = int64(1) << 40
	if *bias > maxBias {
		*bias = maxBias
	}
	if *bias < -maxBias {
		*bias = -maxBias
	}
}

// fullPageCoverage is a compact record of which output pixels have a source
// tile. It is the only per-pixel bookkeeping kept besides the canvas itself.
type fullPageCoverage struct {
	bits []uint64
}

func newFullPageCoverage(pixels uint64) *fullPageCoverage {
	return &fullPageCoverage{bits: make([]uint64, (pixels+63)/64)}
}

func (c *fullPageCoverage) has(index uint64) bool {
	return c.bits[index/64]&(uint64(1)<<(index%64)) != 0
}

func (c *fullPageCoverage) set(index uint64) {
	c.bits[index/64] |= uint64(1) << (index % 64)
}

// fullPageBlit copies the in-bounds part of a tile into the canvas. Pixels
// that already have a source are left untouched, so the first tile in capture
// order owns overlapping pixels. It returns the number of newly covered
// pixels.
func fullPageBlit(canvas *image.NRGBA, coverage *fullPageCoverage, canvasWidth, canvasHeight, originX, originY uint64, tile image.Image) uint64 {
	bounds := tile.Bounds()
	added := uint64(0)
	for v := 0; v < bounds.Dy(); v++ {
		cy := originY + uint64(v)
		if cy >= canvasHeight {
			continue
		}
		for u := 0; u < bounds.Dx(); u++ {
			cx := originX + uint64(u)
			if cx >= canvasWidth {
				continue
			}
			index := cy*canvasWidth + cx
			if coverage.has(index) {
				continue
			}
			pixel := color.NRGBAModel.Convert(tile.At(bounds.Min.X+u, bounds.Min.Y+v)).(color.NRGBA)
			offset := int(cy)*canvas.Stride + int(cx)*4
			canvas.Pix[offset+0] = pixel.R
			canvas.Pix[offset+1] = pixel.G
			canvas.Pix[offset+2] = pixel.B
			canvas.Pix[offset+3] = pixel.A
			coverage.set(index)
			added++
		}
	}
	return added
}

// fullPageDecodeTile validates the PNG header before decoding. The header
// dimensions must be exactly the expected viewport size, and the implied
// allocation must fit both MaxPixels and the platform integer range.
func fullPageDecodeTile(data []byte, want fullPageViewport, maxPixels uint64) (image.Image, error) {
	config, err := png.DecodeConfig(bytes.NewReader(data))
	if err != nil {
		return nil, fmt.Errorf("invalid PNG header: %w", err)
	}
	if config.Width <= 0 || config.Height <= 0 {
		return nil, fmt.Errorf("invalid PNG dimensions %dx%d", config.Width, config.Height)
	}
	if uint64(config.Width) != want.width || uint64(config.Height) != want.height {
		return nil, fmt.Errorf("screenshot is %dx%d, want %dx%d", config.Width, config.Height, want.width, want.height)
	}
	pixels := uint64(config.Width) * uint64(config.Height)
	maxInt := uint64(^uint(0) >> 1)
	if pixels > maxPixels || pixels > maxInt/4 {
		return nil, fmt.Errorf("screenshot %dx%d exceeds the allocation limit", config.Width, config.Height)
	}
	tile, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		return nil, fmt.Errorf("invalid PNG: %w", err)
	}
	return tile, nil
}

func (wd *remoteWD) fullPageReadMetrics() (fullPageMetrics, error) {
	value, err := wd.ExecuteScript(fullPageMetricsScript, nil)
	if err != nil {
		return fullPageMetrics{}, err
	}
	object, ok := value.(map[string]interface{})
	if !ok {
		return fullPageMetrics{}, fmt.Errorf("metrics script returned %T, want an object", value)
	}
	var m fullPageMetrics
	fields := []struct {
		names []string
		dst   *float64
	}{
		{[]string{"width", "scrollWidth", "documentWidth"}, &m.width},
		{[]string{"height", "scrollHeight", "documentHeight"}, &m.height},
		{[]string{"viewportWidth", "innerWidth"}, &m.viewportWidth},
		{[]string{"viewportHeight", "innerHeight"}, &m.viewportHeight},
		{[]string{"x", "scrollX"}, &m.x},
		{[]string{"y", "scrollY"}, &m.y},
		{[]string{"dpr", "devicePixelRatio"}, &m.dpr},
	}
	for _, field := range fields {
		var raw interface{}
		var ok bool
		for _, name := range field.names {
			if raw, ok = object[name]; ok {
				break
			}
		}
		if !ok {
			return fullPageMetrics{}, fmt.Errorf("metrics script did not return %q", field.names[0])
		}
		number, ok := raw.(float64)
		if !ok {
			return fullPageMetrics{}, fmt.Errorf("metric %q has type %T, want a number", field.names[0], raw)
		}
		*field.dst = number
	}
	return m, nil
}

func (wd *remoteWD) fullPageScroll(x, y float64) error {
	_, err := wd.ExecuteScript(fullPageScrollScript, []interface{}{x, y})
	return err
}

// fullPageRestoreScroll restores and verifies the original scroll position. It
// always uses ExecuteScript, so it uses the same protocol dialect as the rest
// of the capture.
func (wd *remoteWD) fullPageRestoreScroll(x, y float64) error {
	if err := wd.fullPageScroll(x, y); err != nil {
		return fmt.Errorf("scroll request: %w", err)
	}
	current, err := wd.fullPageReadMetrics()
	if err != nil {
		return fmt.Errorf("reading position after restore: %w", err)
	}
	if err := current.validate(); err != nil {
		return fmt.Errorf("invalid metrics after restore: %w", err)
	}
	const epsilon = 1e-6
	if math.Abs(current.x-x) > epsilon || math.Abs(current.y-y) > epsilon {
		return fmt.Errorf("position after restore is (%g, %g), want (%g, %g)", current.x, current.y, x, y)
	}
	return nil
}
