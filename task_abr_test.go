package gohlslib_test

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"reflect"
	"strings"
	"sync"
	"testing"

	"github.com/bluenviron/mediacommon/v2/pkg/codecs/mpeg4audio"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts"
	tscodecs "github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts/codecs"

	"github.com/bluenviron/gohlslib/v2"
	"github.com/bluenviron/gohlslib/v2/pkg/codecs"
	"github.com/bluenviron/gohlslib/v2/task_support/taskcheck"
)

// abrBarrier blocks a response until the test releases it.
// entered is closed when the first waiter arrives.
type abrBarrier struct {
	entered chan struct{}
	release chan struct{}
	once    sync.Once
}

func newAbrBarrier() *abrBarrier {
	return &abrBarrier{
		entered: make(chan struct{}),
		release: make(chan struct{}),
	}
}

func (b *abrBarrier) waitCtx(ctx context.Context) error {
	b.once.Do(func() { close(b.entered) })
	select {
	case <-b.release:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

// abrRecorder records the path (and byte range, when present) of every request.
type abrRecorder struct {
	mu       sync.Mutex
	requests []string
}

func (r *abrRecorder) record(req *http.Request) {
	entry := req.URL.Path
	if br := req.Header.Get("Range"); br != "" {
		entry += " " + br
	}
	r.mu.Lock()
	r.requests = append(r.requests, entry)
	r.mu.Unlock()
}

func (r *abrRecorder) snapshot() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]string(nil), r.requests...)
}

type abrTransport struct {
	recorder *abrRecorder
	handler  func(req *http.Request) (*http.Response, error)
}

func (t *abrTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	t.recorder.record(req)
	return t.handler(req)
}

func abrHTTPResponse(req *http.Request, status int, body []byte) *http.Response {
	return &http.Response{
		StatusCode: status,
		Header:     make(http.Header),
		Body:       io.NopCloser(bytes.NewReader(body)),
		Request:    req,
	}
}

// abrVideoSegment generates a H264 MPEG-TS segment whose IDR carries a mark byte.
func abrVideoSegment(t *testing.T, rawPTS int64, mark byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	track := &mpegts.Track{Codec: &tscodecs.H264{}}
	w := &mpegts.Writer{W: &buf, Tracks: []*mpegts.Track{track}}
	if err := w.Initialize(); err != nil {
		t.Fatal(err)
	}
	err := w.WriteH264(track, rawPTS, rawPTS, [][]byte{{7, 1, 2, 3}, {8}, {5, mark}})
	if err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// abrVideoSegments generates one video segment per mark; segment i has
// raw PTS/DTS 90000+i*900, so the media timeline is continuous and aligned
// across variants.
func abrVideoSegments(t *testing.T, marks []byte) [][]byte {
	t.Helper()
	ret := make([][]byte, len(marks))
	for i, m := range marks {
		ret[i] = abrVideoSegment(t, 90000+int64(i)*900, m)
	}
	return ret
}

// abrAudioSegment generates a MPEG-4 Audio MPEG-TS segment whose access unit
// carries a mark byte.
func abrAudioSegment(t *testing.T, rawPTS int64, mark byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	track := &mpegts.Track{Codec: &tscodecs.MPEG4Audio{
		Config: mpeg4audio.AudioSpecificConfig{
			Type:          2,
			SampleRate:    44100,
			ChannelConfig: 2,
			ChannelCount:  2, //nolint:staticcheck
		},
	}}
	w := &mpegts.Writer{W: &buf, Tracks: []*mpegts.Track{track}}
	if err := w.Initialize(); err != nil {
		t.Fatal(err)
	}
	if err := w.WriteMPEG4Audio(track, rawPTS, [][]byte{{mark}}); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// abrMediaPlaylist builds a VOD media playlist with MEDIA-SEQUENCE 0.
// Each entry is a segment line (URI, optionally prefixed by a BYTERANGE tag).
func abrMediaPlaylist(segmentLines []string) string {
	var b strings.Builder
	b.WriteString("#EXTM3U\n" +
		"#EXT-X-VERSION:3\n" +
		"#EXT-X-TARGETDURATION:1\n" +
		"#EXT-X-MEDIA-SEQUENCE:0\n" +
		"#EXT-X-PLAYLIST-TYPE:VOD\n")
	for _, line := range segmentLines {
		b.WriteString("#EXTINF:0.01,\n" + line + "\n")
	}
	b.WriteString("#EXT-X-ENDLIST\n")
	return b.String()
}

func abrContains(list []string, v string) bool {
	for _, e := range list {
		if e == v {
			return true
		}
	}
	return false
}

func abrFilterPaths(requests []string, prefixes ...string) []string {
	var ret []string
	for _, r := range requests {
		for _, p := range prefixes {
			if strings.HasPrefix(r, p) {
				ret = append(ret, r)
				break
			}
		}
	}
	return ret
}

// abrCollector gathers tracks, samples and decode errors produced by a client.
type abrCollector struct {
	mu         sync.Mutex
	trackCalls int
	trackCount int
	videoPTS   []int64
	videoDTS   []int64
	videoMarks []byte
	audioPTS   []int64
	audioMarks []byte
	decodeErrs []error

	// onVideoSample, when set, is called after each video sample is recorded.
	onVideoSample func(mark byte)

	firstVideoSample chan struct{}
	videoSampleOnce  sync.Once
}

func newAbrCollector() *abrCollector {
	return &abrCollector{
		firstVideoSample: make(chan struct{}),
	}
}

// abrSetupCallbacks installs silent download callbacks and collecting
// track/data callbacks on the client.
func abrSetupCallbacks(client *gohlslib.Client, col *abrCollector) {
	client.OnDownloadPrimaryPlaylist = func(string) {}
	client.OnDownloadStreamPlaylist = func(string) {}
	client.OnDownloadSegment = func(string) {}
	client.OnDownloadPart = func(string) {}
	client.OnDecodeError = func(err error) {
		col.mu.Lock()
		col.decodeErrs = append(col.decodeErrs, err)
		col.mu.Unlock()
	}
	client.OnTracks = func(tracks []*gohlslib.Track) error {
		col.mu.Lock()
		col.trackCalls++
		col.trackCount = len(tracks)
		col.mu.Unlock()
		for _, track := range tracks {
			switch track.Codec.(type) {
			case *codecs.H264:
				client.OnDataH26x(track, func(pts int64, dts int64, au [][]byte) {
					mark := au[len(au)-1][1]
					col.mu.Lock()
					col.videoPTS = append(col.videoPTS, pts)
					col.videoDTS = append(col.videoDTS, dts)
					col.videoMarks = append(col.videoMarks, mark)
					col.mu.Unlock()
					col.videoSampleOnce.Do(func() { close(col.firstVideoSample) })
					if col.onVideoSample != nil {
						col.onVideoSample(mark)
					}
				})

			case *codecs.MPEG4Audio:
				client.OnDataMPEG4Audio(track, func(pts int64, aus [][]byte) {
					col.mu.Lock()
					col.audioPTS = append(col.audioPTS, pts)
					col.audioMarks = append(col.audioMarks, aus[len(aus)-1][0])
					col.mu.Unlock()
				})
			}
		}
		return nil
	}
}

// TestABRSelection covers initial variant selection with a bandwidth limit.
func TestABRSelection(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const mainMaster = "#EXTM3U\n" +
			"#EXT-X-STREAM-INF:BANDWIDTH=800000,CODECS=\"avc1.42e01e\"\n" +
			"mid/list.m3u8\n" +
			"#EXT-X-STREAM-INF:BANDWIDTH=9000000,CODECS=\"vp09.00.10.08\"\n" +
			"vp9/list.m3u8\n" +
			"#EXT-X-STREAM-INF:BANDWIDTH=100000,CODECS=\"avc1.42e01e\"\n" +
			"low/list.m3u8\n" +
			"#EXT-X-STREAM-INF:BANDWIDTH=3000000,CODECS=\"avc1.42e01e\"\n" +
			"high/list.m3u8\n"

		const tieMaster = "#EXTM3U\n" +
			"#EXT-X-STREAM-INF:BANDWIDTH=500000,CODECS=\"avc1.42e01e\"\n" +
			"a/list.m3u8\n" +
			"#EXT-X-STREAM-INF:BANDWIDTH=500000,CODECS=\"avc1.42e01e\"\n" +
			"b/list.m3u8\n"

		marks := map[string]byte{
			"mid": 1, "vp9": 2, "low": 3, "high": 4, "a": 5, "b": 6,
		}

		segments := make(map[string][]byte)
		playlists := make(map[string]string)
		for v, mark := range marks {
			segments[v] = abrVideoSegment(t, 90000, mark)
			playlists[v] = abrMediaPlaylist([]string{"s0.ts"})
		}

		scenarios := []struct {
			name   string
			master string
			limits []uint64
			expect string
		}{
			{"unset keeps greatest bandwidth", mainMaster, nil, "high"},
			{"greatest bandwidth within limit", mainMaster, []uint64{1000000}, "mid"},
			{"all over limit picks lowest", mainMaster, []uint64{50000}, "low"},
			{"zero means unlimited", mainMaster, []uint64{0}, "high"},
			{"limit is inclusive", mainMaster, []uint64{3000000}, "high"},
			{"unsupported codecs filtered first", mainMaster, []uint64{9000000}, "high"},
			{"only latest limit is kept", mainMaster, []uint64{50000, 1000000}, "mid"},
			{"limit removed before start", mainMaster, []uint64{1000000, 0}, "high"},
			{"bandwidth tie keeps master order", tieMaster, nil, "a"},
			{"all over limit tie keeps master order", tieMaster, []uint64{400000}, "a"},
		}

		for _, sc := range scenarios {
			recorder := &abrRecorder{}
			transport := &abrTransport{
				recorder: recorder,
				handler: func(req *http.Request) (*http.Response, error) {
					if req.URL.Path == "/master.m3u8" {
						return abrHTTPResponse(req, 200, []byte(sc.master)), nil
					}
					parts := strings.Split(req.URL.Path, "/")
					if len(parts) == 3 {
						if pl, ok := playlists[parts[1]]; ok && parts[2] == "list.m3u8" {
							return abrHTTPResponse(req, 200, []byte(pl)), nil
						}
						if seg, ok := segments[parts[1]]; ok && parts[2] == "s0.ts" {
							return abrHTTPResponse(req, 200, seg), nil
						}
					}
					return abrHTTPResponse(req, 404, nil), nil
				},
			}

			col := newAbrCollector()
			client := &gohlslib.Client{
				URI:        "http://fixture.invalid/master.m3u8",
				HTTPClient: &http.Client{Transport: transport},
			}
			abrSetupCallbacks(client, col)
			for _, limit := range sc.limits {
				client.SetBandwidthLimit(limit)
			}

			err := client.Start()
			c.Check(err == nil, "%s: start error: %v", sc.name, err)
			if err != nil {
				continue
			}
			err = client.Wait2()
			client.Close()
			c.Check(errors.Is(err, gohlslib.ErrClientEOS), "%s: wait error: %v", sc.name, err)

			v := sc.expect
			c.Check(reflect.DeepEqual(recorder.snapshot(), []string{
				"/master.m3u8",
				"/" + v + "/list.m3u8",
				"/" + v + "/s0.ts",
				"/" + v + "/list.m3u8",
			}), "%s: requests=%v", sc.name, recorder.snapshot())
			c.Check(col.trackCalls == 1, "%s: track calls=%d", sc.name, col.trackCalls)
			c.Check(bytes.Equal(col.videoMarks, []byte{marks[v]}),
				"%s: marks=%v", sc.name, col.videoMarks)
			c.Check(reflect.DeepEqual(col.videoPTS, []int64{0}),
				"%s: pts=%v", sc.name, col.videoPTS)
			c.Check(len(col.decodeErrs) == 0, "%s: decode errors=%v", sc.name, col.decodeErrs)
		}
	})
}

// TestABRCommit covers runtime switching: failed candidates must not disturb
// the current variant, and a switch commits variant, redirected playlist URL
// and sequence together.
func TestABRCommit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// scenario 1: candidate playlist and candidate segment failures
		// fall back to the old variant at the same sequence; a later
		// candidate commits, honoring redirect and implicit byte-ranges.
		func() {
			lowSegs := abrVideoSegments(t, []byte{0, 1, 2, 3})
			highSegs := abrVideoSegments(t, []byte{100, 101, 102, 103})

			var highAll []byte
			highLengths := make([]int, len(highSegs))
			for i, seg := range highSegs {
				highLengths[i] = len(seg)
				highAll = append(highAll, seg...)
			}

			// the high variant serves segments as byte ranges of a single
			// file; only the first range has an explicit offset.
			highSegLines := make([]string, len(highLengths))
			for i, l := range highLengths {
				if i == 0 {
					highSegLines[i] = fmt.Sprintf("#EXT-X-BYTERANGE:%d@0\nall.ts", l)
				} else {
					highSegLines[i] = fmt.Sprintf("#EXT-X-BYTERANGE:%d\nall.ts", l)
				}
			}
			highPlaylist := abrMediaPlaylist(highSegLines)
			lowPlaylist := abrMediaPlaylist([]string{"s0.ts", "s1.ts", "s2.ts", "s3.ts"})

			const master = "#EXTM3U\n" +
				"#EXT-X-STREAM-INF:BANDWIDTH=900000,CODECS=\"avc1.42e01e\"\n" +
				"high/list.m3u8\n" +
				"#EXT-X-STREAM-INF:BANDWIDTH=100000,CODECS=\"avc1.42e01e\"\n" +
				"low/list.m3u8\n"

			s0Barrier := newAbrBarrier()
			highListFails := 1
			highSegFails := 1
			recorder := &abrRecorder{}
			transport := &abrTransport{
				recorder: recorder,
				handler: func(req *http.Request) (*http.Response, error) {
					switch req.URL.Path {
					case "/master.m3u8":
						return abrHTTPResponse(req, 200, []byte(master)), nil
					case "/low/list.m3u8":
						return abrHTTPResponse(req, 200, []byte(lowPlaylist)), nil
					case "/low/s0.ts":
						if err := s0Barrier.waitCtx(req.Context()); err != nil {
							return nil, err
						}
						return abrHTTPResponse(req, 200, lowSegs[0]), nil
					case "/low/s1.ts":
						return abrHTTPResponse(req, 200, lowSegs[1]), nil
					case "/low/s2.ts":
						return abrHTTPResponse(req, 200, lowSegs[2]), nil
					case "/low/s3.ts":
						return abrHTTPResponse(req, 200, lowSegs[3]), nil
					case "/high/list.m3u8":
						if highListFails > 0 {
							highListFails--
							return abrHTTPResponse(req, 500, nil), nil
						}
						res := abrHTTPResponse(req, 307, nil)
						res.Header.Set("Location", "/high/r/list.m3u8")
						return res, nil
					case "/high/r/list.m3u8":
						return abrHTTPResponse(req, 200, []byte(highPlaylist)), nil
					case "/high/r/all.ts":
						if highSegFails > 0 {
							highSegFails--
							return abrHTTPResponse(req, 500, nil), nil
						}
						var start, end int
						fmt.Sscanf(req.Header.Get("Range"), "bytes=%d-%d", &start, &end)
						return abrHTTPResponse(req, 206, highAll[start:end+1]), nil
					}
					return abrHTTPResponse(req, 404, nil), nil
				},
			}

			var cbMu sync.Mutex
			var playlistCallbacks []string
			var segmentCallbacks []string

			col := newAbrCollector()
			client := &gohlslib.Client{
				URI:        "http://fixture.invalid/master.m3u8",
				HTTPClient: &http.Client{Transport: transport},
			}
			abrSetupCallbacks(client, col)
			client.OnDownloadStreamPlaylist = func(u string) {
				cbMu.Lock()
				playlistCallbacks = append(playlistCallbacks, u)
				cbMu.Unlock()
			}
			client.OnDownloadSegment = func(u string) {
				cbMu.Lock()
				segmentCallbacks = append(segmentCallbacks, u)
				cbMu.Unlock()
			}
			client.SetBandwidthLimit(150000)

			err := client.Start()
			c.Check(err == nil, "commit: start error: %v", err)
			if err != nil {
				return
			}

			<-s0Barrier.entered
			// raise the limit while the old variant segment is still downloading
			client.SetBandwidthLimit(0)
			close(s0Barrier.release)

			err = client.Wait2()
			client.Close()
			c.Check(errors.Is(err, gohlslib.ErrClientEOS), "commit: wait error: %v", err)

			l0, l1, l2, l3 := highLengths[0], highLengths[1], highLengths[2], highLengths[3]
			expectedRequests := []string{
				"/master.m3u8",
				"/low/list.m3u8",
				"/low/s0.ts",
				"/low/list.m3u8",
				"/high/list.m3u8", // candidate playlist: fails
				"/low/s1.ts",      // old variant keeps downloading the same sequence
				"/low/list.m3u8",
				"/high/list.m3u8",   // candidate playlist: redirect
				"/high/r/list.m3u8", //
				fmt.Sprintf("/high/r/all.ts bytes=%d-%d", l0+l1, l0+l1+l2-1), // candidate segment: fails
				"/low/s2.ts", // old variant keeps downloading the same sequence
				"/low/list.m3u8",
				"/high/list.m3u8",
				"/high/r/list.m3u8",
				fmt.Sprintf("/high/r/all.ts bytes=%d-%d", l0+l1+l2, l0+l1+l2+l3-1), // committed
				"/high/r/list.m3u8", // playlist refresh uses the committed redirected URL
			}
			c.Check(reflect.DeepEqual(recorder.snapshot(), expectedRequests),
				"commit: requests=%v", recorder.snapshot())

			c.Check(reflect.DeepEqual(col.videoPTS, []int64{0, 900, 1800, 2700}),
				"commit: pts=%v", col.videoPTS)
			c.Check(bytes.Equal(col.videoMarks, []byte{0, 1, 2, 103}),
				"commit: marks=%v", col.videoMarks)
			c.Check(col.trackCalls == 1, "commit: track calls=%d", col.trackCalls)
			c.Check(len(col.decodeErrs) == 0, "commit: decode errors=%v", col.decodeErrs)

			c.Check(abrContains(playlistCallbacks, "http://fixture.invalid/high/list.m3u8"),
				"commit: playlist callbacks=%v", playlistCallbacks)
			c.Check(abrContains(segmentCallbacks, "http://fixture.invalid/high/r/all.ts"),
				"commit: segment callbacks=%v", segmentCallbacks)
		}()

		// scenario 2: the candidate playlist does not have the next sequence
		// yet; the old variant delivers it and the switch succeeds at a
		// later boundary.
		func() {
			lowSegs := abrVideoSegments(t, []byte{0, 1, 2, 3})
			highSegs := abrVideoSegments(t, []byte{100, 101, 102, 103})
			lowPlaylist := abrMediaPlaylist([]string{"s0.ts", "s1.ts", "s2.ts", "s3.ts"})
			highPlaylist := abrMediaPlaylist([]string{"s0.ts", "s1.ts", "s2.ts", "s3.ts"})
			highPartial := "#EXTM3U\n" +
				"#EXT-X-VERSION:3\n" +
				"#EXT-X-TARGETDURATION:1\n" +
				"#EXT-X-MEDIA-SEQUENCE:0\n" +
				"#EXTINF:0.01,\n" +
				"s0.ts\n"

			const master = "#EXTM3U\n" +
				"#EXT-X-STREAM-INF:BANDWIDTH=900000,CODECS=\"avc1.42e01e\"\n" +
				"high2/list.m3u8\n" +
				"#EXT-X-STREAM-INF:BANDWIDTH=100000,CODECS=\"avc1.42e01e\"\n" +
				"low2/list.m3u8\n"

			s0Barrier := newAbrBarrier()
			highListReqs := 0
			recorder := &abrRecorder{}
			transport := &abrTransport{
				recorder: recorder,
				handler: func(req *http.Request) (*http.Response, error) {
					switch req.URL.Path {
					case "/master2.m3u8":
						return abrHTTPResponse(req, 200, []byte(master)), nil
					case "/low2/list.m3u8":
						return abrHTTPResponse(req, 200, []byte(lowPlaylist)), nil
					case "/low2/s0.ts":
						if err := s0Barrier.waitCtx(req.Context()); err != nil {
							return nil, err
						}
						return abrHTTPResponse(req, 200, lowSegs[0]), nil
					case "/low2/s1.ts":
						return abrHTTPResponse(req, 200, lowSegs[1]), nil
					case "/low2/s2.ts":
						return abrHTTPResponse(req, 200, lowSegs[2]), nil
					case "/low2/s3.ts":
						return abrHTTPResponse(req, 200, lowSegs[3]), nil
					case "/high2/list.m3u8":
						highListReqs++
						if highListReqs == 1 {
							return abrHTTPResponse(req, 200, []byte(highPartial)), nil
						}
						return abrHTTPResponse(req, 200, []byte(highPlaylist)), nil
					case "/high2/s0.ts":
						return abrHTTPResponse(req, 200, highSegs[0]), nil
					case "/high2/s1.ts":
						return abrHTTPResponse(req, 200, highSegs[1]), nil
					case "/high2/s2.ts":
						return abrHTTPResponse(req, 200, highSegs[2]), nil
					case "/high2/s3.ts":
						return abrHTTPResponse(req, 200, highSegs[3]), nil
					}
					return abrHTTPResponse(req, 404, nil), nil
				},
			}

			col := newAbrCollector()
			client := &gohlslib.Client{
				URI:        "http://fixture.invalid/master2.m3u8",
				HTTPClient: &http.Client{Transport: transport},
			}
			abrSetupCallbacks(client, col)
			client.SetBandwidthLimit(150000)

			err := client.Start()
			c.Check(err == nil, "missing-seq: start error: %v", err)
			if err != nil {
				return
			}

			<-s0Barrier.entered
			// raise the limit while the old variant segment is still downloading
			client.SetBandwidthLimit(0)
			close(s0Barrier.release)

			err = client.Wait2()
			client.Close()
			c.Check(errors.Is(err, gohlslib.ErrClientEOS), "missing-seq: wait error: %v", err)

			expectedRequests := []string{
				"/master2.m3u8",
				"/low2/list.m3u8",
				"/low2/s0.ts",
				"/low2/list.m3u8",
				"/high2/list.m3u8", // candidate does not have sequence 1 yet
				"/low2/s1.ts",      // old variant keeps downloading the same sequence
				"/low2/list.m3u8",
				"/high2/list.m3u8", // candidate now has sequence 2
				"/high2/s2.ts",     // committed
				"/high2/list.m3u8",
				"/high2/s3.ts",
				"/high2/list.m3u8",
			}
			c.Check(reflect.DeepEqual(recorder.snapshot(), expectedRequests),
				"missing-seq: requests=%v", recorder.snapshot())

			c.Check(reflect.DeepEqual(col.videoPTS, []int64{0, 900, 1800, 2700}),
				"missing-seq: pts=%v", col.videoPTS)
			c.Check(bytes.Equal(col.videoMarks, []byte{0, 1, 102, 103}),
				"missing-seq: marks=%v", col.videoMarks)
			c.Check(col.trackCalls == 1, "missing-seq: track calls=%d", col.trackCalls)
			c.Check(len(col.decodeErrs) == 0, "missing-seq: decode errors=%v", col.decodeErrs)
		}()

		// scenario 3: closing the client while a candidate download is in
		// flight must propagate cancellation and terminate the client.
		func() {
			lowSegs := abrVideoSegments(t, []byte{0, 1})
			highSegs := abrVideoSegments(t, []byte{100, 101})
			lowPlaylist := abrMediaPlaylist([]string{"s0.ts", "s1.ts"})
			highPlaylist := abrMediaPlaylist([]string{"s0.ts", "s1.ts"})

			const master = "#EXTM3U\n" +
				"#EXT-X-STREAM-INF:BANDWIDTH=900000,CODECS=\"avc1.42e01e\"\n" +
				"ch/list.m3u8\n" +
				"#EXT-X-STREAM-INF:BANDWIDTH=100000,CODECS=\"avc1.42e01e\"\n" +
				"cl/list.m3u8\n"

			s0Barrier := newAbrBarrier()
			candBarrier := newAbrBarrier()
			recorder := &abrRecorder{}
			transport := &abrTransport{
				recorder: recorder,
				handler: func(req *http.Request) (*http.Response, error) {
					switch req.URL.Path {
					case "/master3.m3u8":
						return abrHTTPResponse(req, 200, []byte(master)), nil
					case "/cl/list.m3u8":
						return abrHTTPResponse(req, 200, []byte(lowPlaylist)), nil
					case "/cl/s0.ts":
						if err := s0Barrier.waitCtx(req.Context()); err != nil {
							return nil, err
						}
						return abrHTTPResponse(req, 200, lowSegs[0]), nil
					case "/cl/s1.ts":
						return abrHTTPResponse(req, 200, lowSegs[1]), nil
					case "/ch/list.m3u8":
						if err := candBarrier.waitCtx(req.Context()); err != nil {
							return nil, err
						}
						return abrHTTPResponse(req, 200, []byte(highPlaylist)), nil
					case "/ch/s0.ts":
						return abrHTTPResponse(req, 200, highSegs[0]), nil
					case "/ch/s1.ts":
						return abrHTTPResponse(req, 200, highSegs[1]), nil
					}
					return abrHTTPResponse(req, 404, nil), nil
				},
			}

			col := newAbrCollector()
			client := &gohlslib.Client{
				URI:        "http://fixture.invalid/master3.m3u8",
				HTTPClient: &http.Client{Transport: transport},
			}
			abrSetupCallbacks(client, col)
			client.SetBandwidthLimit(150000)

			err := client.Start()
			c.Check(err == nil, "cancel: start error: %v", err)
			if err != nil {
				return
			}

			<-s0Barrier.entered
			// raise the limit while the old variant segment is still downloading
			client.SetBandwidthLimit(0)
			close(s0Barrier.release)

			// wait for the candidate playlist request, then close the client
			<-candBarrier.entered
			// the first segment has been delivered by now; make sure its
			// sample reached the callback before closing
			<-col.firstVideoSample
			client.Close()

			err = client.Wait2()
			c.Check(err != nil && !errors.Is(err, gohlslib.ErrClientEOS),
				"cancel: wait error: %v", err)

			c.Check(reflect.DeepEqual(recorder.snapshot(), []string{
				"/master3.m3u8",
				"/cl/list.m3u8",
				"/cl/s0.ts",
				"/cl/list.m3u8",
				"/ch/list.m3u8", // candidate attempt aborted by Close
			}), "cancel: requests=%v", recorder.snapshot())
			c.Check(bytes.Equal(col.videoMarks, []byte{0}), "cancel: marks=%v", col.videoMarks)
			c.Check(col.trackCalls == 1, "cancel: track calls=%d", col.trackCalls)
		}()
	})
}

// TestABRContinuity covers runtime switching with real MPEG-TS samples:
// tracks, audio rendition and PTS conversion keep using the original
// instances across switches.
func TestABRContinuity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const master = "#EXTM3U\n" +
			"#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES," +
			"URI=\"audio/list.m3u8\"\n" +
			"#EXT-X-STREAM-INF:BANDWIDTH=900000,CODECS=\"avc1.42e01e,mp4a.40.2\",AUDIO=\"aac\"\n" +
			"high/list.m3u8\n" +
			"#EXT-X-STREAM-INF:BANDWIDTH=100000,CODECS=\"avc1.42e01e,mp4a.40.2\",AUDIO=\"aac\"\n" +
			"low/list.m3u8\n"

		highSegs := abrVideoSegments(t, []byte{100, 101, 102, 103})
		lowSegs := abrVideoSegments(t, []byte{0, 1, 2, 3})
		audioSegs := make([][]byte, 4)
		for i := range audioSegs {
			audioSegs[i] = abrAudioSegment(t, 90000+int64(i)*1024, byte(50+i))
		}

		videoPlaylist := abrMediaPlaylist([]string{"s0.ts", "s1.ts", "s2.ts", "s3.ts"})
		audioPlaylist := abrMediaPlaylist([]string{"a0.ts", "a1.ts", "a2.ts", "a3.ts"})

		s0Barrier := newAbrBarrier() // /high/s0.ts
		s2Barrier := newAbrBarrier() // /low/s2.ts
		recorder := &abrRecorder{}
		transport := &abrTransport{
			recorder: recorder,
			handler: func(req *http.Request) (*http.Response, error) {
				switch req.URL.Path {
				case "/master.m3u8":
					return abrHTTPResponse(req, 200, []byte(master)), nil
				case "/high/list.m3u8", "/low/list.m3u8":
					return abrHTTPResponse(req, 200, []byte(videoPlaylist)), nil
				case "/audio/list.m3u8":
					return abrHTTPResponse(req, 200, []byte(audioPlaylist)), nil
				case "/high/s0.ts":
					if err := s0Barrier.waitCtx(req.Context()); err != nil {
						return nil, err
					}
					return abrHTTPResponse(req, 200, highSegs[0]), nil
				case "/high/s1.ts":
					return abrHTTPResponse(req, 200, highSegs[1]), nil
				case "/high/s2.ts":
					return abrHTTPResponse(req, 200, highSegs[2]), nil
				case "/high/s3.ts":
					return abrHTTPResponse(req, 200, highSegs[3]), nil
				case "/low/s0.ts":
					return abrHTTPResponse(req, 200, lowSegs[0]), nil
				case "/low/s1.ts":
					return abrHTTPResponse(req, 200, lowSegs[1]), nil
				case "/low/s2.ts":
					if err := s2Barrier.waitCtx(req.Context()); err != nil {
						return nil, err
					}
					return abrHTTPResponse(req, 200, lowSegs[2]), nil
				case "/low/s3.ts":
					return abrHTTPResponse(req, 200, lowSegs[3]), nil
				case "/audio/a0.ts":
					return abrHTTPResponse(req, 200, audioSegs[0]), nil
				case "/audio/a1.ts":
					return abrHTTPResponse(req, 200, audioSegs[1]), nil
				case "/audio/a2.ts":
					return abrHTTPResponse(req, 200, audioSegs[2]), nil
				case "/audio/a3.ts":
					return abrHTTPResponse(req, 200, audioSegs[3]), nil
				}
				return abrHTTPResponse(req, 404, nil), nil
			},
		}

		col := newAbrCollector()
		client := &gohlslib.Client{
			URI:        "http://fixture.invalid/master.m3u8",
			HTTPClient: &http.Client{Transport: transport},
		}
		abrSetupCallbacks(client, col)

		// the second limit change is performed from inside a data callback,
		// gated so that it happens exactly while the /low/s2.ts request of
		// the old variant is still in flight.
		allowRestore := make(chan struct{})
		restored := make(chan struct{})
		var restoreOnce sync.Once
		col.onVideoSample = func(mark byte) {
			if mark == 1 {
				<-allowRestore
				client.SetBandwidthLimit(0)
				restoreOnce.Do(func() { close(restored) })
			}
		}

		err := client.Start()
		c.Check(err == nil, "continuity: start error: %v", err)
		if err != nil {
			return
		}

		<-s0Barrier.entered
		// lower the limit while the first segment is still downloading
		client.SetBandwidthLimit(150000)
		close(s0Barrier.release)

		<-s2Barrier.entered
		// let the data callback restore the limit while /low/s2.ts of the
		// old variant is still downloading
		close(allowRestore)
		<-restored
		close(s2Barrier.release)

		err = client.Wait2()
		client.Close()
		c.Check(errors.Is(err, gohlslib.ErrClientEOS), "continuity: wait error: %v", err)

		// video segments came from high, then low, then high again
		c.Check(reflect.DeepEqual(
			abrFilterPaths(recorder.snapshot(), "/high/s", "/low/s"),
			[]string{"/high/s0.ts", "/low/s1.ts", "/low/s2.ts", "/high/s3.ts"}),
			"continuity: video segments=%v", abrFilterPaths(recorder.snapshot(), "/high/s", "/low/s"))

		// tracks were provided once, by the original instances
		c.Check(col.trackCalls == 1, "continuity: track calls=%d", col.trackCalls)
		c.Check(col.trackCount == 2, "continuity: tracks=%d", col.trackCount)

		// samples are continuous: no duplicates, no PTS reset
		c.Check(reflect.DeepEqual(col.videoPTS, []int64{0, 900, 1800, 2700}),
			"continuity: video pts=%v", col.videoPTS)
		c.Check(reflect.DeepEqual(col.videoDTS, []int64{0, 900, 1800, 2700}),
			"continuity: video dts=%v", col.videoDTS)
		c.Check(bytes.Equal(col.videoMarks, []byte{100, 1, 2, 103}),
			"continuity: video marks=%v", col.videoMarks)

		// the audio rendition kept flowing from its original stream
		c.Check(reflect.DeepEqual(col.audioPTS, []int64{0, 1024, 2048, 3072}),
			"continuity: audio pts=%v", col.audioPTS)
		c.Check(bytes.Equal(col.audioMarks, []byte{50, 51, 52, 53}),
			"continuity: audio marks=%v", col.audioMarks)

		c.Check(len(col.decodeErrs) == 0, "continuity: decode errors=%v", col.decodeErrs)
	})
}
