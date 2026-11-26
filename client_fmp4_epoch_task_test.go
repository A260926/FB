package gohlslib

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/bluenviron/mediacommon/v2/pkg/codecs/h264"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4/seekablebuffer"
	mc "github.com/bluenviron/mediacommon/v2/pkg/formats/mp4/codecs"
	"github.com/stretchr/testify/require"

	"github.com/bluenviron/gohlslib/v2/pkg/codecs"
	"github.com/bluenviron/gohlslib/v2/task_support/taskcheck"
)

const (
	epochVideoTimescale int64 = 90000
	epochAudioTimescale int64 = 48000
)

var epochH264SPS = []byte{
	0x67, 0x42, 0xc0, 0x28, 0xd9, 0x00, 0x78, 0x02,
	0x27, 0xe5, 0x84, 0x00, 0x00, 0x03, 0x00, 0x04,
	0x00, 0x00, 0x03, 0x00, 0xf0, 0x3c, 0x60, 0xc9,
	0x20,
}

var epochH264PPS = []byte{0x08}

var epochUTC = time.Date(2024, 1, 2, 3, 4, 5, 0, time.UTC)

// epochSample returns a valid AVCC payload that embeds a marker byte.
func epochSample(marker byte) []byte {
	byts, err := h264.AVCC{{0x09, marker}}.Marshal()
	if err != nil {
		panic(err)
	}
	return byts
}

func epochSampleMarker(data []byte) byte {
	// video tracks receive the decoded NALU, audio tracks the raw payload
	if len(data) == 2 && data[0] == 0x09 {
		return data[1]
	}
	if len(data) == 6 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 2 && data[4] == 0x09 {
		return data[5]
	}
	return 0
}

func epochInitFile(t *testing.T, tracks []*fmp4.InitTrack) []byte {
	t.Helper()

	var buf seekablebuffer.Buffer
	err := (fmp4.Init{Tracks: tracks}).Marshal(&buf)
	require.NoError(t, err)

	return append([]byte(nil), buf.Bytes()...)
}

func epochPartFile(t *testing.T, parts ...*fmp4.Part) []byte {
	t.Helper()

	var buf seekablebuffer.Buffer
	err := (fmp4.Parts)(parts).Marshal(&buf)
	require.NoError(t, err)

	return append([]byte(nil), buf.Bytes()...)
}

// epochServer is an in-memory http.RoundTripper. A test can hold a response on
// a channel, keyed by path, which makes delivery ordering deterministic
// without sleeps or timing thresholds.
type epochServer struct {
	mutex    sync.Mutex
	objects  map[string][]byte
	versions map[string][][]byte
	gates    map[string]chan struct{}
	held     chan string
}

func newEpochServer() *epochServer {
	return &epochServer{
		objects:  make(map[string][]byte),
		versions: make(map[string][][]byte),
		gates:    make(map[string]chan struct{}),
		held:     make(chan string, 16),
	}
}

func (s *epochServer) set(path string, byts []byte) {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	s.objects[path] = byts
	delete(s.versions, path)
}

// pushQueue queues successive responses for a path; the last one is served
// for all further requests.
func (s *epochServer) pushQueue(path string, byts ...[]byte) {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	s.versions[path] = append(s.versions[path], byts...)
}

func (s *epochServer) gate(path string) chan struct{} {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	ch := make(chan struct{})
	s.gates[path] = ch

	return ch
}

func (s *epochServer) release(path string) {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	if ch, ok := s.gates[path]; ok {
		close(ch)
		delete(s.gates, path)
	}
}

func (s *epochServer) RoundTrip(r *http.Request) (*http.Response, error) {
	s.mutex.Lock()
	path := r.URL.Path
	var byts []byte
	var ok bool
	if versions := s.versions[path]; len(versions) != 0 {
		byts, ok = versions[0], true
		if len(versions) > 1 {
			s.versions[path] = versions[1:]
		}
	} else {
		byts, ok = s.objects[path]
	}
	gate := s.gates[path]
	s.mutex.Unlock()

	if !ok {
		return nil, fmt.Errorf("unlisted object %s", path)
	}

	if gate != nil {
		select {
		case s.held <- path:
		default:
		}

		select {
		case <-gate:
		case <-r.Context().Done():
			return nil, r.Context().Err()
		}
	}

	return &http.Response{
		StatusCode: http.StatusOK,
		Header:     make(http.Header),
		Body:       io.NopCloser(bytes.NewReader(byts)),
		Request:    r,
	}, nil
}

type epochDelivery struct {
	pts  int64
	dts  int64
	data []byte
	at   time.Time
}

type epochRecorder struct {
	mutex        sync.Mutex
	trackLen     int
	onTracksRuns int
	delivered    map[string][]epochDelivery
}

func newEpochRecorder() *epochRecorder {
	return &epochRecorder{delivered: make(map[string][]epochDelivery)}
}

func (r *epochRecorder) append(name string, d epochDelivery) {
	r.mutex.Lock()
	defer r.mutex.Unlock()

	r.delivered[name] = append(r.delivered[name], d)
}

func (r *epochRecorder) get(name string) []epochDelivery {
	r.mutex.Lock()
	defer r.mutex.Unlock()

	return append([]epochDelivery(nil), r.delivered[name]...)
}

func (r *epochRecorder) count(name string) int {
	r.mutex.Lock()
	defer r.mutex.Unlock()

	return len(r.delivered[name])
}

func (r *epochRecorder) total() int {
	r.mutex.Lock()
	defer r.mutex.Unlock()

	var n int
	for _, v := range r.delivered {
		n += len(v)
	}
	return n
}

// epochTrackNames maps track IDs to recorder names, in track order.
func epochTrackNames(ids ...string) []string {
	return ids
}

// configureEpochRecorder hooks OnTracks and the per-codec callbacks.
func configureEpochRecorder(c *Client, rec *epochRecorder, names []string) {
	c.OnTracks = func(tracks []*Track) error {
		rec.mutex.Lock()
		rec.trackLen = len(tracks)
		rec.onTracksRuns++
		rec.mutex.Unlock()

		for i, track := range tracks {
			name := names[i]

			if _, ok := track.Codec.(*codecs.H264); ok {
				c.OnDataH26x(track, func(pts int64, dts int64, au [][]byte) {
					var payload []byte
					if len(au) != 0 {
						payload = au[0]
					}

					at, _ := c.AbsoluteTime(track)
					rec.append(name, epochDelivery{pts: pts, dts: dts, data: payload, at: at})
				})
			} else {
				c.OnDataOpus(track, func(pts int64, packets [][]byte) {
					var payload []byte
					if len(packets) != 0 {
						payload = packets[0]
					}

					at, _ := c.AbsoluteTime(track)
					rec.append(name, epochDelivery{pts: pts, dts: pts, data: payload, at: at})
				})
			}
		}

		return nil
	}
}

// playlist builders

type epochSegSpec struct {
	discontinuity bool
	dateTime      *time.Time
	uri           string
}

func epochPlaylist(
	mediaSequence int,
	discoSequence *int,
	endlist bool,
	mapURI string,
	segs []epochSegSpec,
) []byte {
	var b strings.Builder

	b.WriteString("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n")
	b.WriteString(fmt.Sprintf("#EXT-X-MEDIA-SEQUENCE:%d\n", mediaSequence))
	if discoSequence != nil {
		b.WriteString(fmt.Sprintf("#EXT-X-DISCONTINUITY-SEQUENCE:%d\n", *discoSequence))
	}
	b.WriteString("#EXT-X-MAP:URI=\"" + mapURI + "\"\n")

	for _, seg := range segs {
		if seg.discontinuity {
			b.WriteString("#EXT-X-DISCONTINUITY\n")
		}
		if seg.dateTime != nil {
			b.WriteString("#EXT-X-PROGRAM-DATE-TIME:" + seg.dateTime.Format(time.RFC3339Nano) + "\n")
		}
		b.WriteString("#EXTINF:0.001,\n" + seg.uri + "\n")
	}

	if endlist {
		b.WriteString("#EXT-X-ENDLIST\n")
	}

	return []byte(b.String())
}

func epochDT(offset time.Duration) *time.Time {
	v := epochUTC.Add(offset)
	return &v
}

func epochIntPtr(v int) *int {
	return &v
}

// runEpochClient starts a client against an in-memory server and waits for it.
func runEpochClient(t *testing.T, c *Client, srv *epochServer, rec *epochRecorder) error {
	t.Helper()

	c.HTTPClient = &http.Client{Transport: srv}
	c.EnableFMP4Discontinuities = true

	require.NoError(t, c.Start())

	errCh := make(chan error, 1)
	go func() {
		errCh <- c.Wait2()
	}()

	select {
	case err := <-errCh:
		c.Close()
		return err
	case <-time.After(10 * time.Second):
		c.Close()
		require.FailNow(t, "client did not terminate")
		return nil
	}
}

func TestEpochTimeline(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		videoInit := []*fmp4.InitTrack{{
			ID:        1,
			TimeScale: uint32(epochVideoTimescale),
			Codec:     &mc.H264{SPS: epochH264SPS, PPS: epochH264PPS},
		}}

		audioInit := []*fmp4.InitTrack{{
			ID:        1,
			TimeScale: uint32(epochAudioTimescale),
			Codec:     &mc.Opus{ChannelCount: 2},
		}}

		srv := newEpochServer()
		srv.set("/vinit.mp4", epochInitFile(t, videoInit))
		srv.set("/ainit.mp4", epochInitFile(t, audioInit))

		// Video: discontinuity sequence 5, two segments in the same epoch.
		// Segment 0 drops the tfdt to 0; DTS must stay continuous.
		videoSeg0 := epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID:       1,
				BaseTime: 0,
				Samples: []*fmp4.Sample{
					{Duration: 900, PTSOffset: 0, Payload: epochSample(0x40)},
					{Duration: 900, PTSOffset: 0, Payload: epochSample(0x41)},
				},
			}},
		})
		videoSeg1 := epochPartFile(t, &fmp4.Part{
			SequenceNumber: 2,
			Tracks: []*fmp4.PartTrack{{
				ID:       1,
				BaseTime: 1800,
				Samples: []*fmp4.Sample{
					{Duration: 900, Payload: epochSample(0x42)},
				},
			}},
		})
		videoSeg2 := epochPartFile(t, &fmp4.Part{
			SequenceNumber: 3,
			Tracks: []*fmp4.PartTrack{{
				ID:       1,
				BaseTime: 2700,
				Samples: []*fmp4.Sample{
					{Duration: 900, Payload: epochSample(0x43)},
				},
			}},
		})
		srv.set("/v0.mp4", videoSeg0)
		srv.set("/v1.mp4", videoSeg1)
		srv.set("/v2.mp4", videoSeg2)

		// Audio rendition: 48 kHz, same epoch as video, but with its own raw
		// tfdt origin (1920 ticks, i.e. 40 ms relative to the leading base) and
		// a non-zero PTS offset. This exercises a rendition-specific media
		// origin that differs from the leading one.
		audioSeg0 := epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID:       1,
				BaseTime: 1920,
				Samples: []*fmp4.Sample{
					{Duration: 480, PTSOffset: 240, Payload: epochSample(0x50)},
					{Duration: 480, PTSOffset: 0, Payload: epochSample(0x51)},
				},
			}},
		})
		srv.set("/a0.mp4", audioSeg0)

		audioSeg1 := epochPartFile(t, &fmp4.Part{
			SequenceNumber: 2,
			Tracks: []*fmp4.PartTrack{{
				ID:       1,
				BaseTime: 2880,
				Samples: []*fmp4.Sample{
					{Duration: 480, PTSOffset: 0, Payload: epochSample(0x52)},
					{Duration: 480, PTSOffset: 0, Payload: epochSample(0x53)},
				},
			}},
		})
		srv.set("/a1.mp4", audioSeg1)

		srv.set("/main.m3u8", epochPlaylist(7, epochIntPtr(5), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "v0.mp4"},
			{dateTime: epochDT(10 * time.Millisecond), uri: "v1.mp4"},
			{dateTime: epochDT(20 * time.Millisecond), uri: "v2.mp4"},
		}))
		// The audio rendition's PDT differs slightly from the leading one; it
		// is its own epoch anchor, while T0 remains the leading one.
		srv.set("/audio.m3u8", epochPlaylist(7, epochIntPtr(5), true, "ainit.mp4", []epochSegSpec{
			{dateTime: epochDT(2 * time.Millisecond), uri: "a0.mp4"},
			{dateTime: epochDT(22 * time.Millisecond), uri: "a1.mp4"},
		}))

		primary := []byte("#EXTM3U\n#EXT-X-VERSION:7\n" +
			"#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\"audio.m3u8\"\n" +
			"#EXT-X-STREAM-INF:BANDWIDTH=1000,CODECS=\"avc1.42c028,opus\",AUDIO=\"aud\"\n" +
			"video.m3u8\n")
		srv.set("/primary.m3u8", primary)
		srv.set("/video.m3u8", epochPlaylist(7, epochIntPtr(5), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "v0.mp4"},
			{dateTime: epochDT(10 * time.Millisecond), uri: "v1.mp4"},
			{dateTime: epochDT(20 * time.Millisecond), uri: "v2.mp4"},
		}))

		rec := newEpochRecorder()

		client := &Client{
			URI:           "http://epoch.invalid/primary.m3u8",
			StartDistance: 1,
			MaxDistance:   1,
		}
		configureEpochRecorder(client, rec, epochTrackNames("video", "audio"))

		err := runEpochClient(t, client, srv, rec)
		c.Check(err == ErrClientEOS, "client ended with %v", err)
		c.Check(rec.trackLen == 2, "tracks=%d", rec.trackLen)

		video := rec.get("video")
		c.Check(len(video) == 4, "video samples=%d", len(video))
		if len(video) == 4 {
			c.Check(video[0].dts == 0, "first video dts=%d", video[0].dts)
			c.Check(video[1].dts == 900, "second video dts=%d", video[1].dts)
			c.Check(video[2].dts == 1800, "third video dts=%d", video[2].dts)
			c.Check(video[3].dts == 2700, "fourth video dts=%d", video[3].dts)
			c.Check(video[0].pts == 0 && video[1].pts == 900 && video[2].pts == 1800 && video[3].pts == 2700,
				"video pts=%d,%d,%d,%d", video[0].pts, video[1].pts, video[2].pts, video[3].pts)
			c.Check(epochSampleMarker(video[0].data) == 0x40 &&
				epochSampleMarker(video[1].data) == 0x41 &&
				epochSampleMarker(video[2].data) == 0x42 &&
				epochSampleMarker(video[3].data) == 0x43,
				"video payloads=%x,%x,%x,%x", video[0].data, video[1].data, video[2].data, video[3].data)
			c.Check(video[0].at.Equal(epochUTC), "first video absolute=%v", video[0].at)
			c.Check(video[2].at.Equal(epochUTC.Add(20*time.Millisecond)),
				"third video absolute=%v", video[2].at)
			c.Check(video[3].at.Equal(epochUTC.Add(30*time.Millisecond)),
				"fourth video absolute=%v", video[3].at)
		}

		audio := rec.get("audio")
		c.Check(len(audio) == 4, "audio samples=%d", len(audio))
		if len(audio) == 4 {
			// the rendition's own raw tfdt (1920) is subtracted from the
			// rendition's own media origin, so PTS starts at the offset
			// between the rendition's PDT (+2 ms) and the global T0, and the
			// first sample's signed PTSOffset (+240) is preserved.
			c.Check(audio[0].pts == 336, "first audio pts=%d", audio[0].pts)
			c.Check(audio[1].pts == 576, "second audio pts=%d", audio[1].pts)
			c.Check(audio[2].pts == 1056, "third audio pts=%d", audio[2].pts)
			c.Check(audio[3].pts == 1536, "fourth audio pts=%d", audio[3].pts)
			c.Check(epochSampleMarker(audio[0].data) == 0x50 &&
				epochSampleMarker(audio[1].data) == 0x51 &&
				epochSampleMarker(audio[2].data) == 0x52 &&
				epochSampleMarker(audio[3].data) == 0x53,
				"audio payloads=%x,%x,%x,%x", audio[0].data, audio[1].data, audio[2].data, audio[3].data)
			c.Check(audio[0].at.Equal(epochUTC.Add(2*time.Millisecond)),
				"first audio absolute=%v", audio[0].at)
			c.Check(audio[1].at.Equal(epochUTC.Add(12*time.Millisecond)),
				"second audio absolute=%v", audio[1].at)
			c.Check(audio[3].at.Equal(epochUTC.Add(32*time.Millisecond)),
				"fourth audio absolute=%v", audio[3].at)
		}

		// Live sliding window: the playlist is re-read between segments. The
		// continuation playlist no longer contains the discontinuity tag, and
		// its discontinuity sequence has advanced accordingly. The epoch
		// identity must follow the absolute sequence and must not be
		// re-anchored when the tag moves out of the window.
		liveVideoInit := videoInit
		srv3 := newEpochServer()
		srv3.set("/vinit.mp4", epochInitFile(t, liveVideoInit))

		liveSeg := func(seq uint32, base int, marker byte) []byte {
			return epochPartFile(t, &fmp4.Part{
				SequenceNumber: seq,
				Tracks: []*fmp4.PartTrack{{
					ID: 1, BaseTime: uint64(base),
					Samples: []*fmp4.Sample{
						{Duration: 900, Payload: epochSample(marker)},
						{Duration: 900, Payload: epochSample(marker + 1)},
					},
				}},
			})
		}
		srv3.set("/l1.mp4", liveSeg(2, 0, 0x62))
		srv3.set("/l2.mp4", liveSeg(3, 1800, 0x64))
		srv3.set("/l3.mp4", liveSeg(4, 3600, 0x66))

		// The discontinuity tag is no longer in the window of the continuation
		// playlists: it has been folded into EXT-X-DISCONTINUITY-SEQUENCE
		// (5 -> 6). Re-reading the playlist must not re-anchor l1.
		pl2 := epochPlaylist(2, epochIntPtr(6), false, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(20 * time.Millisecond), uri: "l1.mp4"},
			{dateTime: epochDT(40 * time.Millisecond), uri: "l2.mp4"},
		})
		pl3 := epochPlaylist(3, epochIntPtr(6), false, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(40 * time.Millisecond), uri: "l2.mp4"},
			{dateTime: epochDT(60 * time.Millisecond), uri: "l3.mp4"},
		})
		pl4 := epochPlaylist(4, epochIntPtr(6), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(60 * time.Millisecond), uri: "l3.mp4"},
		})

		srv3.pushQueue("/video.m3u8", pl2, pl3, pl4)

		rec3 := newEpochRecorder()
		client3 := &Client{
			URI:           "http://epoch.invalid/video.m3u8",
			StartDistance: 2,
			MaxDistance:   2,
		}
		configureEpochRecorder(client3, rec3, epochTrackNames("video"))
		client3.HTTPClient = &http.Client{Transport: srv3}
		client3.EnableFMP4Discontinuities = true

		require.NoError(t, client3.Start())
		err = runEpochClientWait(client3)
		c.Check(err == ErrClientEOS, "live client ended with %v", err)

		// the initial window starts at l1, which is already in epoch 6: the
		// discontinuity of the first read has been folded into the sequence.
		live := rec3.get("video")
		c.Check(len(live) == 6, "live samples=%d", len(live))
		if len(live) == 6 {
			wantDTS := []int64{0, 900, 1800, 2700, 3600, 4500}
			wantAbs := []time.Duration{
				20 * time.Millisecond, 30 * time.Millisecond,
				40 * time.Millisecond, 50 * time.Millisecond,
				60 * time.Millisecond, 70 * time.Millisecond,
			}
			for i := range live {
				c.Check(live[i].dts == wantDTS[i], "live dts[%d]=%d", i, live[i].dts)
				c.Check(live[i].at.Equal(epochUTC.Add(wantAbs[i])), "live abs[%d]=%v", i, live[i].at)
				c.Check(epochSampleMarker(live[i].data) == byte(0x62+i), "live payload[%d]=%x", i, live[i].data)
			}
		}
	})
}

func TestEpochRenditions(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		videoInit := []*fmp4.InitTrack{{
			ID:        1,
			TimeScale: uint32(epochVideoTimescale),
			Codec:     &mc.H264{SPS: epochH264SPS, PPS: epochH264PPS},
		}}

		audioInit := []*fmp4.InitTrack{{
			ID:        1,
			TimeScale: uint32(epochAudioTimescale),
			Codec:     &mc.Opus{ChannelCount: 2},
		}}

		srv := newEpochServer()
		srv.set("/vinit.mp4", epochInitFile(t, videoInit))
		srv.set("/ainit.mp4", epochInitFile(t, audioInit))

		// Epoch A starts with the leading rendition.
		srv.set("/v0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x40)}},
			}},
		}))

		// The leading rendition enters epoch B (tfdt restarts at 0) while the
		// audio is still in epoch A; the audio then enters epoch B late.
		srv.set("/v1.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 2,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x41)}},
			}},
		}))
		srv.set("/v2.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 3,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 900,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x42)}},
			}},
		}))

		srv.set("/a0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 480, Payload: epochSample(0x50)}},
			}},
		}))
		srv.set("/a1.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 2,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 480, Payload: epochSample(0x51)}},
			}},
		}))

		srv.set("/audio.m3u8", epochPlaylist(3, epochIntPtr(5), true, "ainit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "a0.mp4"},
			{discontinuity: true, dateTime: epochDT(10 * time.Millisecond), uri: "a1.mp4"},
		}))

		srv.set("/video.m3u8", epochPlaylist(5, epochIntPtr(5), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "v0.mp4"},
			{discontinuity: true, dateTime: epochDT(10 * time.Millisecond), uri: "v1.mp4"},
			{dateTime: epochDT(20 * time.Millisecond), uri: "v2.mp4"},
		}))

		srv.set("/primary.m3u8", []byte("#EXTM3U\n#EXT-X-VERSION:7\n"+
			"#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\"audio.m3u8\"\n"+
			"#EXT-X-STREAM-INF:BANDWIDTH=1000,CODECS=\"avc1.42c028,opus\",AUDIO=\"aud\"\n"+
			"video.m3u8\n"))

		rec := newEpochRecorder()
		client := &Client{
			URI:           "http://epoch.invalid/primary.m3u8",
			StartDistance: 1,
			MaxDistance:   1,
		}
		configureEpochRecorder(client, rec, epochTrackNames("video", "audio"))

		err := runEpochClient(t, client, srv, rec)
		c.Check(err == ErrClientEOS, "client ended with %v", err)
		c.Check(rec.trackLen == 2, "tracks=%d", rec.trackLen)

		video := rec.get("video")
		c.Check(len(video) == 3, "video samples=%d", len(video))
		if len(video) == 3 {
			// tfdt restarted at 0 in the second epoch: DTS must not go back.
			c.Check(video[0].dts == 0, "v0 dts=%d", video[0].dts)
			c.Check(video[1].dts == 900, "v1 dts=%d", video[1].dts)
			c.Check(video[2].dts == 1800, "v2 dts=%d", video[2].dts)
			c.Check(video[1].pts == 900, "v1 pts=%d", video[1].pts)
			c.Check(video[0].at.Equal(epochUTC), "v0 abs=%v", video[0].at)
			c.Check(video[1].at.Equal(epochUTC.Add(10*time.Millisecond)), "v1 abs=%v", video[1].at)
			c.Check(video[2].at.Equal(epochUTC.Add(20*time.Millisecond)), "v2 abs=%v", video[2].at)
			c.Check(video[1].at.After(video[0].at), "absolute time went backwards")
		}

		audio := rec.get("audio")
		c.Check(len(audio) == 2, "audio samples=%d", len(audio))
		if len(audio) == 2 {
			// Audio enters the next generation later, but still lands at the
			// discontinuity's PROGRAM-DATE-TIME, with a continuous DTS.
			c.Check(audio[0].pts == 0, "a0 pts=%d", audio[0].pts)
			c.Check(audio[1].pts == 480, "a1 pts=%d", audio[1].pts)
			c.Check(audio[0].at.Equal(epochUTC), "a0 abs=%v", audio[0].at)
			c.Check(audio[1].at.Equal(epochUTC.Add(10*time.Millisecond)), "a1 abs=%v", audio[1].at)
		}

		// Two-generation limit: the leading rendition tries to enter a third
		// generation while the audio is still in the first one. It must wait
		// for the audio to leave the oldest generation, then proceed.
		videoInit2 := videoInit
		audioInit2 := audioInit

		srv2 := newEpochServer()
		srv2.set("/vinit.mp4", epochInitFile(t, videoInit2))
		srv2.set("/ainit.mp4", epochInitFile(t, audioInit2))

		vSeg := func(seq uint32, base int, marker byte) []byte {
			return epochPartFile(t, &fmp4.Part{
				SequenceNumber: seq,
				Tracks: []*fmp4.PartTrack{{
					ID: 1, BaseTime: uint64(base),
					Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(marker)}},
				}},
			})
		}
		srv2.set("/v0.mp4", vSeg(1, 0, 0x40))
		srv2.set("/v1.mp4", vSeg(2, 0, 0x41))
		srv2.set("/v2.mp4", vSeg(3, 0, 0x42))
		// v3 continues epoch C, whose tfdt restarted at 0, so it starts at 900.
		srv2.set("/v3.mp4", vSeg(4, 900, 0x43))

		aSeg := func(seq uint32, marker byte) []byte {
			return epochPartFile(t, &fmp4.Part{
				SequenceNumber: seq,
				Tracks: []*fmp4.PartTrack{{
					ID: 1, BaseTime: 0,
					Samples: []*fmp4.Sample{{Duration: 480, Payload: epochSample(marker)}},
				}},
			})
		}
		srv2.set("/a0.mp4", aSeg(1, 0x50))
		srv2.set("/a1.mp4", aSeg(2, 0x51))
		srv2.set("/a2.mp4", aSeg(3, 0x52))

		// video: epochs A -> B -> C (three generations)
		srv2.set("/video.m3u8", epochPlaylist(1, epochIntPtr(5), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "v0.mp4"},
			{discontinuity: true, dateTime: epochDT(10 * time.Millisecond), uri: "v1.mp4"},
			{discontinuity: true, dateTime: epochDT(20 * time.Millisecond), uri: "v2.mp4"},
			{dateTime: epochDT(30 * time.Millisecond), uri: "v3.mp4"},
		}))

		// audio: epochs A -> B, and a1 is held on a gate so audio stays in
		// epoch A while the video wants to enter epoch C.
		audioGate := srv2.gate("/a1.mp4")
		srv2.set("/audio.m3u8", epochPlaylist(1, epochIntPtr(5), true, "ainit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "a0.mp4"},
			{discontinuity: true, dateTime: epochDT(10 * time.Millisecond), uri: "a1.mp4"},
		}))

		srv2.set("/primary.m3u8", []byte("#EXTM3U\n#EXT-X-VERSION:7\n"+
			"#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\"audio.m3u8\"\n"+
			"#EXT-X-STREAM-INF:BANDWIDTH=1000,CODECS=\"avc1.42c028,opus\",AUDIO=\"aud\"\n"+
			"video.m3u8\n"))

		rec2 := newEpochRecorder()
		client2 := &Client{
			URI:           "http://epoch.invalid/primary.m3u8",
			StartDistance: 1,
			MaxDistance:   1,
		}
		configureEpochRecorder(client2, rec2, epochTrackNames("video", "audio"))
		client2.HTTPClient = &http.Client{Transport: srv2}
		client2.EnableFMP4Discontinuities = true

		require.NoError(t, client2.Start())

		// Wait until the audio is held in the first generation.
		select {
		case <-srv2.held:
		case <-time.After(10 * time.Second):
			c.Check(false, "audio segment was never held")
		}

		// The video has delivered epochs A and B; it cannot publish epoch C
		// while the audio is still in epoch A. Make sure it is indeed blocked.
		waitForGenerationWait(t, client2)

		videoBefore := rec2.get("video")
		c.Check(len(videoBefore) == 2,
			"video delivered %d samples while waiting for the generation limit", len(videoBefore))

		// Release the audio into epoch B: the video may now enter epoch C.
		srv2.release("/a1.mp4")

		err = runEpochClientWait(client2)
		c.Check(err == ErrClientEOS, "backpressure client ended with %v", err)
		c.Check(rec2.onTracksRuns == 1, "OnTracks was called %d times", rec2.onTracksRuns)

		videoAll := rec2.get("video")
		c.Check(len(videoAll) == 4, "video samples=%d", len(videoAll))
		if len(videoAll) == 4 {
			c.Check(videoAll[0].dts == 0, "v0 dts=%d", videoAll[0].dts)
			c.Check(videoAll[1].dts == 900, "v1 dts=%d", videoAll[1].dts)
			c.Check(videoAll[2].dts == 1800, "v2 dts=%d", videoAll[2].dts)
			c.Check(videoAll[3].dts == 2700, "v3 dts=%d", videoAll[3].dts)
			c.Check(videoAll[2].at.Equal(epochUTC.Add(20*time.Millisecond)), "v2 abs=%v", videoAll[2].at)
			c.Check(videoAll[3].at.Equal(epochUTC.Add(30*time.Millisecond)), "v3 abs=%v", videoAll[3].at)
		}

		audioAll := rec2.get("audio")
		c.Check(len(audioAll) == 2, "audio samples=%d", len(audioAll))
		if len(audioAll) == 2 {
			c.Check(audioAll[0].pts == 0, "a0 pts=%d", audioAll[0].pts)
			c.Check(audioAll[1].pts == 480, "a1 pts=%d", audioAll[1].pts)
			c.Check(audioAll[1].at.Equal(epochUTC.Add(10*time.Millisecond)), "a1 abs=%v", audioAll[1].at)
		}
		_ = audioGate
	})
}

func waitForGenerationWait(t *testing.T, client *Client) {
	t.Helper()

	ch := client.epochManager.chGenerationWait

	select {
	case <-ch:
	case <-time.After(10 * time.Second):
	}
}

func runEpochClientWait(client *Client) error {
	ch := make(chan error, 1)
	go func() {
		ch <- client.Wait2()
	}()

	select {
	case err := <-ch:
		client.Close()
		return err
	case <-time.After(10 * time.Second):
		client.Close()
		return fmt.Errorf("timeout")
	}
}

func TestEpochCancel(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		videoInit := []*fmp4.InitTrack{{
			ID:        1,
			TimeScale: uint32(epochVideoTimescale),
			Codec:     &mc.H264{SPS: epochH264SPS, PPS: epochH264PPS},
		}}

		audioInit := []*fmp4.InitTrack{{
			ID:        1,
			TimeScale: uint32(epochAudioTimescale),
			Codec:     &mc.Opus{ChannelCount: 2},
		}}

		srv := newEpochServer()
		srv.set("/vinit.mp4", epochInitFile(t, videoInit))
		srv.set("/ainit.mp4", epochInitFile(t, audioInit))
		srv.set("/vinit2.mp4", epochInitFile(t, videoInit))
		srv.set("/ainit2.mp4", epochInitFile(t, audioInit))

		// The leading rendition produces a bad segment: its tfdt is far before
		// the end of the previously delivered segment. No sample from the bad
		// segment may be delivered, and the error must surface before any
		// anchor update.
		srv.set("/v0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x40)}},
			}},
		}))
		srv.set("/vbad.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 2,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{
					{Duration: 900, Payload: epochSample(0x41)},
					{Duration: 900, Payload: epochSample(0x42)},
				},
			}},
		}))

		srv.set("/a0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 480, Payload: epochSample(0x50)}},
			}},
		}))

		srv.set("/video.m3u8", epochPlaylist(1, epochIntPtr(5), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "v0.mp4"},
			{uri: "vbad.mp4"}, // no PROGRAM-DATE-TIME: must be rejected
		}))
		srv.set("/audio.m3u8", epochPlaylist(1, epochIntPtr(5), true, "ainit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "a0.mp4"},
		}))
		srv.set("/primary.m3u8", []byte("#EXTM3U\n#EXT-X-VERSION:7\n"+
			"#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\"audio.m3u8\"\n"+
			"#EXT-X-STREAM-INF:BANDWIDTH=1000,CODECS=\"avc1.42c028,opus\",AUDIO=\"aud\"\n"+
			"video.m3u8\n"))

		rec := newEpochRecorder()
		client := &Client{
			URI:           "http://epoch.invalid/primary.m3u8",
			StartDistance: 1,
			MaxDistance:   1,
		}
		configureEpochRecorder(client, rec, epochTrackNames("video", "audio"))

		err := runEpochClient(t, client, srv, rec)
		c.Check(err != nil, "bad client ended without error")
		c.Check(!errorsIsEOS(err), "bad client ended with EOS instead of an error")
		c.Check(rec.count("video") == 1, "bad segment delivered %d video samples", rec.count("video"))
		video := rec.get("video")
		if len(video) == 1 {
			c.Check(epochSampleMarker(video[0].data) == 0x40, "delivered %x", video[0].data)
			c.Check(video[0].dts == 0, "dts=%d", video[0].dts)
		}

		// Second scenario: context cancellation while the client waits for
		// the leading rendition's first anchor. The audio stream must not hang.
		srv2 := newEpochServer()
		srv2.set("/vinit.mp4", epochInitFile(t, videoInit))
		srv2.set("/ainit.mp4", epochInitFile(t, audioInit))
		srv2.set("/v0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x40)}},
			}},
		}))
		srv2.set("/a0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 480, Payload: epochSample(0x50)}},
			}},
		}))

		// Video playlist points at a segment that is held on a channel, so the
		// leading stream cannot publish an anchor yet.
		heldPath := "/vheld.mp4"
		srv2.set(heldPath, nil)

		srv2.set("/video.m3u8", epochPlaylist(1, epochIntPtr(5), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "vheld.mp4"},
		}))
		srv2.set("/audio.m3u8", epochPlaylist(1, epochIntPtr(5), true, "ainit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "a0.mp4"},
		}))
		srv2.set("/primary.m3u8", []byte("#EXTM3U\n#EXT-X-VERSION:7\n"+
			"#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\"audio.m3u8\"\n"+
			"#EXT-X-STREAM-INF:BANDWIDTH=1000,CODECS=\"avc1.42c028,opus\",AUDIO=\"aud\"\n"+
			"video.m3u8\n"))

		// Replace the placeholder object with the real one once the gate exists.
		_ = srv2.gate(heldPath)
		srv2.set(heldPath, epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x40)}},
			}},
		}))

		rec2 := newEpochRecorder()
		client2 := &Client{
			URI:           "http://epoch.invalid/primary.m3u8",
			StartDistance: 1,
			MaxDistance:   1,
		}
		configureEpochRecorder(client2, rec2, epochTrackNames("video", "audio"))
		client2.HTTPClient = &http.Client{Transport: srv2}
		client2.EnableFMP4Discontinuities = true

		require.NoError(t, client2.Start())

		// The leading rendition is blocked on the gated segment; the audio
		// rendition must be waiting for the anchor, not delivering samples.
		select {
		case <-srv2.held:
			c.Check(rec2.count("audio") == 0,
				"audio delivered %d samples while the leading anchor was missing", rec2.count("audio"))
		case <-time.After(10 * time.Second):
			c.Check(false, "the held segment was never requested")
		}

		client2.Close()
		c.Check(rec2.count("audio") == 0, "audio delivered after cancellation")

		// Third scenario: a corrupted segment must be rejected before any of
		// its samples (or its anchor) is published.
		srv3 := newEpochServer()
		srv3.set("/vinit.mp4", epochInitFile(t, videoInit))
		srv3.set("/v0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x40)}},
			}},
		}))
		srv3.set("/vcorrupt.mp4", []byte{0x00, 0x00, 0x00, 0x08, 'f', 'r', 'e', 'e'})

		srv3.set("/video.m3u8", epochPlaylist(1, epochIntPtr(5), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "v0.mp4"},
			{discontinuity: true, dateTime: epochDT(10 * time.Millisecond), uri: "vcorrupt.mp4"},
		}))

		rec3 := newEpochRecorder()
		client3 := &Client{
			URI:           "http://epoch.invalid/video.m3u8",
			StartDistance: 1,
			MaxDistance:   1,
		}
		configureEpochRecorder(client3, rec3, epochTrackNames("video"))

		err = runEpochClient(t, client3, srv3, rec3)
		c.Check(err != nil && !errorsIsEOS(err), "corrupted client ended with %v", err)
		c.Check(rec3.count("video") == 1, "corrupted segment delivered %d video samples", rec3.count("video"))

		// Fourth scenario: the discontinuity sequence jumps by two between two
		// reads of the playlist, e.g. when two generations slid out of the
		// window at once. The next segment must be rejected before delivery.
		srv4 := newEpochServer()
		srv4.set("/vinit.mp4", epochInitFile(t, videoInit))
		srv4.set("/j0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x40)}},
			}},
		}))
		srv4.set("/j1.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 2,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x41)}},
			}},
		}))
		jpl1 := epochPlaylist(1, epochIntPtr(5), false, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "j0.mp4"},
		})
		// the sequence advanced by two: generation 6 was skipped
		jpl2 := epochPlaylist(2, epochIntPtr(7), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(10 * time.Millisecond), uri: "j1.mp4"},
		})
		srv4.pushQueue("/video.m3u8", jpl1, jpl2)

		rec4 := newEpochRecorder()
		client4 := &Client{
			URI:           "http://epoch.invalid/video.m3u8",
			StartDistance: 1,
			MaxDistance:   1,
		}
		configureEpochRecorder(client4, rec4, epochTrackNames("video"))

		err = runEpochClient(t, client4, srv4, rec4)
		c.Check(err != nil && !errorsIsEOS(err), "jump client ended with %v", err)
		c.Check(rec4.count("video") == 1, "jump segment delivered %d video samples", rec4.count("video"))
		if rec4.count("video") == 1 {
			c.Check(epochSampleMarker(rec4.get("video")[0].data) == 0x40,
				"jump delivered wrong payload")
		}

		// Fifth scenario: the discontinuity sequence goes backwards when a
		// playlist is re-read. The first segment of the regressed read must
		// be rejected.
		srv5 := newEpochServer()
		srv5.set("/vinit.mp4", epochInitFile(t, videoInit))
		srv5.set("/b0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x40)}},
			}},
		}))
		srv5.set("/b1.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 2,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x41)}},
			}},
		}))
		srv5.set("/b2.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 3,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x42)}},
			}},
		}))
		// the first read covers epochs 6 -> 7
		bpl1 := epochPlaylist(1, epochIntPtr(6), false, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "b0.mp4"},
			{discontinuity: true, dateTime: epochDT(10 * time.Millisecond), uri: "b1.mp4"},
			{dateTime: epochDT(20 * time.Millisecond), uri: "b2.mp4"},
		})
		// the next read regresses the discontinuity sequence from 7 back to 6,
		// and re-lists the segment that was already anchored in epoch 7
		bpl2 := epochPlaylist(3, epochIntPtr(6), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(20 * time.Millisecond), uri: "b2.mp4"},
		})
		srv5.pushQueue("/video.m3u8", bpl1, bpl2)

		rec5 := newEpochRecorder()
		client5 := &Client{
			URI:           "http://epoch.invalid/video.m3u8",
			StartDistance: 2,
			MaxDistance:   2,
		}
		configureEpochRecorder(client5, rec5, epochTrackNames("video"))

		err = runEpochClient(t, client5, srv5, rec5)
		c.Check(err != nil && !errorsIsEOS(err), "backwards client ended with %v", err)
		c.Check(rec5.count("video") == 1, "backwards case delivered %d video samples", rec5.count("video"))
		if rec5.count("video") == 1 {
			c.Check(epochSampleMarker(rec5.get("video")[0].data) == 0x41,
				"backwards case delivered the wrong payload")
		}

		// Sixth scenario: the leading track data is missing from a segment.
		srv6 := newEpochServer()
		srv6.set("/vinit.mp4", epochInitFile(t, videoInit))
		srv6.set("/nolead.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 99, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x40)}},
			}},
		}))
		srv6.set("/video.m3u8", epochPlaylist(1, epochIntPtr(5), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "nolead.mp4"},
		}))

		rec6 := newEpochRecorder()
		client6 := &Client{
			URI:           "http://epoch.invalid/video.m3u8",
			StartDistance: 1,
			MaxDistance:   1,
		}
		configureEpochRecorder(client6, rec6, epochTrackNames("video"))

		err = runEpochClient(t, client6, srv6, rec6)
		c.Check(err != nil && !errorsIsEOS(err), "no-leading client ended with %v", err)
		c.Check(rec6.count("video") == 0, "no-leading segment delivered %d video samples", rec6.count("video"))

		// Seventh scenario: a segment whose tfdt regresses below the end of
		// the previously delivered segment within the same generation.
		srv7 := newEpochServer()
		srv7.set("/vinit.mp4", epochInitFile(t, videoInit))
		srv7.set("/r0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{
					{Duration: 900, Payload: epochSample(0x40)},
					{Duration: 900, Payload: epochSample(0x41)},
				},
			}},
		}))
		srv7.set("/r1.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 2,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 900,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x42)}},
			}},
		}))
		srv7.set("/video.m3u8", epochPlaylist(1, epochIntPtr(5), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "r0.mp4"},
			{dateTime: epochDT(10 * time.Millisecond), uri: "r1.mp4"},
		}))

		rec7 := newEpochRecorder()
		client7 := &Client{
			URI:           "http://epoch.invalid/video.m3u8",
			StartDistance: 1,
			MaxDistance:   2,
		}
		configureEpochRecorder(client7, rec7, epochTrackNames("video"))

		err = runEpochClient(t, client7, srv7, rec7)
		c.Check(err != nil && !errorsIsEOS(err), "regression client ended with %v", err)
		c.Check(rec7.count("video") == 2, "regression delivered %d video samples", rec7.count("video"))

		// Ninth scenario: the initialization object changes at a
		// discontinuity, which is not supported.
		srv9 := newEpochServer()
		srv9.set("/vinit.mp4", epochInitFile(t, videoInit))
		srv9.set("/vinit2.mp4", epochInitFile(t, videoInit))
		srv9.set("/m0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x40)}},
			}},
		}))
		srv9.set("/m1.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 2,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x41)}},
			}},
		}))
		mpl1 := epochPlaylist(1, epochIntPtr(5), false, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "m0.mp4"},
		})
		mpl2 := []byte("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n" +
			"#EXT-X-MEDIA-SEQUENCE:2\n#EXT-X-DISCONTINUITY-SEQUENCE:5\n" +
			"#EXT-X-MAP:URI=\"vinit2.mp4\"\n" +
			"#EXT-X-DISCONTINUITY\n#EXT-X-PROGRAM-DATE-TIME:" +
			epochDT(10*time.Millisecond).Format(time.RFC3339Nano) + "\n#EXTINF:0.001,\nm1.mp4\n" +
			"#EXT-X-ENDLIST\n")
		srv9.pushQueue("/video.m3u8", mpl1, mpl2)

		rec9 := newEpochRecorder()
		client9 := &Client{
			URI:           "http://epoch.invalid/video.m3u8",
			StartDistance: 1,
			MaxDistance:   1,
		}
		configureEpochRecorder(client9, rec9, epochTrackNames("video"))

		err = runEpochClient(t, client9, srv9, rec9)
		c.Check(err != nil && !errorsIsEOS(err), "map-change client ended with %v", err)

		// Eighth scenario: the audio rendition starts in a different
		// discontinuity generation than the leading one.
		srv8 := newEpochServer()
		srv8.set("/vinit.mp4", epochInitFile(t, videoInit))
		srv8.set("/ainit.mp4", epochInitFile(t, audioInit))
		srv8.set("/v0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 900, Payload: epochSample(0x40)}},
			}},
		}))
		srv8.set("/a0.mp4", epochPartFile(t, &fmp4.Part{
			SequenceNumber: 1,
			Tracks: []*fmp4.PartTrack{{
				ID: 1, BaseTime: 0,
				Samples: []*fmp4.Sample{{Duration: 480, Payload: epochSample(0x50)}},
			}},
		}))
		srv8.set("/video.m3u8", epochPlaylist(1, epochIntPtr(5), true, "vinit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "v0.mp4"},
		}))
		// the audio's first segment belongs to generation 6, not 5
		srv8.set("/audio.m3u8", epochPlaylist(1, epochIntPtr(6), true, "ainit.mp4", []epochSegSpec{
			{dateTime: epochDT(0), uri: "a0.mp4"},
		}))
		srv8.set("/primary.m3u8", []byte("#EXTM3U\n#EXT-X-VERSION:7\n"+
			"#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\"audio.m3u8\"\n"+
			"#EXT-X-STREAM-INF:BANDWIDTH=1000,CODECS=\"avc1.42c028,opus\",AUDIO=\"aud\"\n"+
			"video.m3u8\n"))

		rec8 := newEpochRecorder()
		client8 := &Client{
			URI:           "http://epoch.invalid/primary.m3u8",
			StartDistance: 1,
			MaxDistance:   1,
		}
		configureEpochRecorder(client8, rec8, epochTrackNames("video", "audio"))

		err = runEpochClient(t, client8, srv8, rec8)
		c.Check(err != nil && !errorsIsEOS(err), "initial-generation mismatch client ended with %v", err)
		c.Check(rec8.count("audio") == 0, "mismatched audio delivered %d samples", rec8.count("audio"))
	})
}

func errorsIsEOS(err error) bool {
	return err == ErrClientEOS
}
