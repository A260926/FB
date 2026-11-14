package gohlslib_test

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"net/http"
	"reflect"
	"sync"
	"testing"

	"github.com/bluenviron/gohlslib/v2"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts"
	tscodecs "github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts/codecs"
)

type testBaselineTransport func(*http.Request) (*http.Response, error)

func (f testBaselineTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

// This exercises the existing complete MPEG-TS client pipeline. It has no
// adaptive selection policy, future API, or target assertions.
func TestBaselineFlow(t *testing.T) {
	segments := make(map[string][]byte)
	for i := 0; i < 2; i++ {
		var b bytes.Buffer
		track := &mpegts.Track{Codec: &tscodecs.H264{}}
		w := &mpegts.Writer{W: &b, Tracks: []*mpegts.Track{track}}
		if err := w.Initialize(); err != nil {
			t.Fatal(err)
		}
		au := [][]byte{{7, 1, 2, 3}, {8}, {5, byte(10 + i)}}
		if err := w.WriteH264(track, 90000+int64(i)*900, 90000+int64(i)*900, au); err != nil {
			t.Fatal(err)
		}
		segments[fmt.Sprintf("/v/s%d.ts", i)] = append([]byte(nil), b.Bytes()...)
	}
	var mu sync.Mutex
	var paths []string
	var pts []int64
	var marks []byte
	trackCalls := 0
	transport := testBaselineTransport(func(r *http.Request) (*http.Response, error) {
		mu.Lock()
		paths = append(paths, r.URL.Path)
		mu.Unlock()
		var data []byte
		switch r.URL.Path {
		case "/master.m3u8":
			data = []byte("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100000,CODECS=\"avc1.42e01e\"\nv/list.m3u8\n")
		case "/v/list.m3u8":
			data = []byte("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:20\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXTINF:0.01,\ns0.ts\n#EXTINF:0.01,\ns1.ts\n#EXT-X-ENDLIST\n")
		default:
			var ok bool
			data, ok = segments[r.URL.Path]
			if !ok {
				return nil, fmt.Errorf("unexpected path %s", r.URL.Path)
			}
		}
		return &http.Response{StatusCode: 200, Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(data)), Request: r}, nil
	})
	client := &gohlslib.Client{URI: "http://fixture.invalid/master.m3u8", HTTPClient: &http.Client{Transport: transport}}
	client.OnDecodeError = func(err error) { t.Errorf("decode: %v", err) }
	client.OnTracks = func(tracks []*gohlslib.Track) error {
		mu.Lock()
		trackCalls++
		mu.Unlock()
		if len(tracks) != 1 {
			return fmt.Errorf("tracks=%d", len(tracks))
		}
		client.OnDataH26x(tracks[0], func(p, d int64, au [][]byte) {
			mu.Lock()
			defer mu.Unlock()
			pts = append(pts, p)
			marks = append(marks, au[len(au)-1][1])
		})
		return nil
	}
	if err := client.Start(); err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	if err := <-client.Wait(); !errors.Is(err, gohlslib.ErrClientEOS) {
		t.Fatal(err)
	}
	mu.Lock()
	defer mu.Unlock()
	if trackCalls != 1 || !reflect.DeepEqual(pts, []int64{0, 900}) || !bytes.Equal(marks, []byte{10, 11}) {
		t.Fatalf("tracks=%d pts=%v marks=%v", trackCalls, pts, marks)
	}
	media := []string{}
	for _, p := range paths {
		if p == "/v/s0.ts" || p == "/v/s1.ts" {
			media = append(media, p)
		}
	}
	if !reflect.DeepEqual(media, []string{"/v/s0.ts", "/v/s1.ts"}) {
		t.Fatal(media)
	}
	t.Log("CHECK_BASELINE_MPEGTS samples=2 tracks=1 pts=0,900 payload=10,11 eos=1")
}
