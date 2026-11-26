package gohlslib

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"time"

	"github.com/bluenviron/mediacommon/v2/pkg/limitedreader"
	"golang.org/x/sync/errgroup"

	"github.com/bluenviron/gohlslib/v2/pkg/playlist"
)

func findSegmentWithInvPosition(segments []*playlist.MediaSegment, invPos int) (*playlist.MediaSegment, int) {
	index := len(segments) - invPos
	if index < 0 {
		return nil, 0
	}

	return segments[index], index
}

func findSegmentWithID(seqNo int, segments []*playlist.MediaSegment, id int) (*playlist.MediaSegment, int, int) {
	index := int(int64(id) - int64(seqNo))
	if index < 0 || index >= len(segments) {
		return nil, 0, 0
	}

	return segments[index], index, len(segments) - index
}

// segmentByteRangeStart returns the start of the byte range of the segment at position pos.
// When the offset is not present, the byte range begins at the next byte
// following the byte range of the previous segment.
func segmentByteRangeStart(segments []*playlist.MediaSegment, pos int) uint64 {
	var start uint64

	for segments[pos].ByteRangeStart == nil && pos > 0 && segments[pos-1].ByteRangeLength != nil {
		pos--
		start += *segments[pos].ByteRangeLength
	}

	if segments[pos].ByteRangeStart != nil {
		start += *segments[pos].ByteRangeStart
	}

	return start
}

func dateTimeOfPreloadHint(pl *playlist.Media) *time.Time {
	if len(pl.Segments) == 0 {
		return nil
	}

	lastSeg := pl.Segments[len(pl.Segments)-1]
	if lastSeg.DateTime == nil {
		return nil
	}

	d := lastSeg.DateTime.Add(lastSeg.Duration)

	for _, part := range pl.Parts {
		d = d.Add(part.Duration)
	}

	return &d
}

type clientStreamDownloaderParent interface {
	onProcessorError(errGroupCtx context.Context, err error)
	setTimeConv(ts clientTimeConv)
	waitTimeConv(errGroupCtx context.Context) (clientTimeConv, bool)
}

type clientStreamDownloader struct {
	isLeading                bool
	startDistance            int
	maxDistance              int
	httpClient               *http.Client
	epochManager             *clientFMP4EpochManager
	onRequest                ClientOnRequestFunc
	onDownloadStreamPlaylist ClientOnDownloadStreamPlaylistFunc
	onDownloadSegment        ClientOnDownloadSegmentFunc
	onDownloadPart           ClientOnDownloadPartFunc
	onDecodeError            ClientOnDecodeErrorFunc
	playlistURL              *url.URL
	rendition                *playlist.MultivariantRendition
	firstPlaylist            *playlist.Media
	errGroup                 *errgroup.Group
	parent                   clientStreamDownloaderParent

	segmentQueue *clientSegmentQueue
	curSegmentID *int

	// out
	chTracks chan []*Track

	// in
	chStartStreaming chan map[*Track]*clientTrack
}

func (d *clientStreamDownloader) initialize() {
	d.chTracks = make(chan []*Track)
	d.chStartStreaming = make(chan map[*Track]*clientTrack)
}

func (d *clientStreamDownloader) run(errGroupCtx context.Context) error {
	if d.firstPlaylist == nil {
		var err error
		d.firstPlaylist, err = d.downloadPlaylist(errGroupCtx, false)
		if err != nil {
			return err
		}
	}

	d.segmentQueue = &clientSegmentQueue{}
	d.segmentQueue.initialize()

	// discontinuity generations are only handled by the traditional path;
	// the low-latency one keeps its existing behavior.
	lowLatency := d.firstPlaylist.ServerControl != nil &&
		d.firstPlaylist.ServerControl.CanBlockReload &&
		d.firstPlaylist.PreloadHint != nil

	var epochManager *clientFMP4EpochManager
	if !lowLatency {
		epochManager = d.epochManager
	}

	if d.firstPlaylist.Map != nil && d.firstPlaylist.Map.URI != "" {
		initFile, err := d.downloadSegment(
			errGroupCtx,
			d.firstPlaylist.Map.URI,
			d.firstPlaylist.Map.ByteRangeStart,
			d.firstPlaylist.Map.ByteRangeLength)
		if err != nil {
			return err
		}

		proc := &clientStreamProcessorFMP4{
			errGroupCtx:  errGroupCtx,
			isLeading:    d.isLeading,
			rendition:    d.rendition,
			initFile:     initFile,
			segmentQueue: d.segmentQueue,
			errGroup:     d.errGroup,
			parent:       d,
			epochManager: epochManager,
		}
		proc.initialize()

		d.errGroup.Go(func() error {
			return proc.run(errGroupCtx)
		})
	} else {
		proc := &clientStreamProcessorMPEGTS{
			onDecodeError: d.onDecodeError,
			isLeading:     d.isLeading,
			segmentQueue:  d.segmentQueue,
			errGroup:      d.errGroup,
			parent:        d,
			epochManager:  d.epochManager,
		}
		proc.initialize()

		d.errGroup.Go(func() error {
			return proc.run(errGroupCtx)
		})
	}

	var err error

	if lowLatency {
		err = d.runLowLatency(errGroupCtx)
	} else {
		err = d.runTraditional(errGroupCtx)
	}

	d.segmentQueue.push(&segmentData{
		err: err,
	})
	<-errGroupCtx.Done()

	return fmt.Errorf("terminated")
}

func (d *clientStreamDownloader) runLowLatency(errGroupCtx context.Context) error {
	pl := d.firstPlaylist

	for {
		byts, err := d.downloadPreloadHint(errGroupCtx, pl.PreloadHint)
		if err != nil {
			return err
		}

		d.segmentQueue.push(&segmentData{
			dateTime: dateTimeOfPreloadHintValue(pl),
			hasPDT:   dateTimeOfPreloadHint(pl) != nil,
			payload:  byts,
		})

		pl, err = d.downloadPlaylist(errGroupCtx, d.firstPlaylist.ServerControl.CanSkipUntil != nil)
		if err != nil {
			return err
		}

		if pl.PreloadHint == nil {
			return fmt.Errorf("preload hint disappeared")
		}
	}
}

func dateTimeValue(v *time.Time) time.Time {
	if v != nil {
		return *v
	}
	return time.Time{}
}

func dateTimeOfPreloadHintValue(pl *playlist.Media) time.Time {
	if v := dateTimeOfPreloadHint(pl); v != nil {
		return *v
	}
	return time.Time{}
}

func (d *clientStreamDownloader) runTraditional(errGroupCtx context.Context) error {
	pl := d.firstPlaylist

	for {
		seg, payload, epoch, err := d.downloadNextSegment(errGroupCtx, pl)
		if err != nil {
			return err
		}

		d.segmentQueue.push(&segmentData{
			dateTime: dateTimeValue(seg.DateTime),
			hasPDT:   seg.DateTime != nil,
			epoch:    epoch,
			payload:  payload,
		})

		ok := d.segmentQueue.waitUntilSizeIsBelow(errGroupCtx, 1)
		if !ok {
			return fmt.Errorf("terminated")
		}

		pl, err = d.downloadPlaylist(errGroupCtx, false)
		if err != nil {
			return err
		}
	}
}

func (d *clientStreamDownloader) downloadPlaylist(
	errGroupCtx context.Context,
	skipUntil bool,
) (*playlist.Media, error) {
	ur := d.playlistURL

	if skipUntil {
		newUR := cloneURL(ur)
		q := newUR.Query()
		q.Add("_HLS_skip", "YES")
		newUR.RawQuery = q.Encode()
		ur = newUR
	}

	d.onDownloadStreamPlaylist(ur.String())

	finalURL, pl, err := downloadPlaylist(errGroupCtx, d.httpClient, d.onRequest, ur)
	if err != nil {
		return nil, err
	}

	if skipUntil {
		q := finalURL.Query()
		q.Del("_HLS_skip")
		finalURL.RawQuery = q.Encode()
	}

	d.playlistURL = finalURL

	plt, ok := pl.(*playlist.Media)
	if !ok {
		return nil, fmt.Errorf("invalid playlist")
	}

	return plt, nil
}

func (d *clientStreamDownloader) downloadPreloadHint(
	errGroupCtx context.Context,
	preloadHint *playlist.MediaPreloadHint,
) ([]byte, error) {
	u, err := clientAbsoluteURL(d.playlistURL, preloadHint.URI)
	if err != nil {
		return nil, err
	}

	d.onDownloadPart(u.String())

	req, err := http.NewRequestWithContext(errGroupCtx, http.MethodGet, u.String(), nil)
	if err != nil {
		return nil, err
	}

	if preloadHint.ByteRangeLength != nil {
		req.Header.Add("Range", "bytes="+strconv.FormatUint(preloadHint.ByteRangeStart, 10)+
			"-"+strconv.FormatUint(preloadHint.ByteRangeStart+*preloadHint.ByteRangeLength-1, 10))
	}

	d.onRequest(req)

	res, err := d.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()

	if res.StatusCode != http.StatusOK && res.StatusCode != http.StatusPartialContent {
		return nil, fmt.Errorf("bad status code: %d", res.StatusCode)
	}

	byts, err := io.ReadAll(&limitedreader.Reader{R: res.Body, N: clientMaxInboundPartSize})
	if err != nil {
		return nil, err
	}

	return byts, nil
}

func (d *clientStreamDownloader) downloadSegment(
	errGroupCtx context.Context,
	uri string,
	start *uint64,
	length *uint64,
) ([]byte, error) {
	u, err := clientAbsoluteURL(d.playlistURL, uri)
	if err != nil {
		return nil, err
	}

	d.onDownloadSegment(u.String())

	req, err := http.NewRequestWithContext(errGroupCtx, http.MethodGet, u.String(), nil)
	if err != nil {
		return nil, err
	}

	if length != nil {
		if start == nil {
			start = new(uint64(0))
		}
		req.Header.Add("Range", "bytes="+strconv.FormatUint(*start, 10)+
			"-"+strconv.FormatUint(*start+*length-1, 10))
	}

	d.onRequest(req)

	res, err := d.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()

	if res.StatusCode != http.StatusOK && res.StatusCode != http.StatusPartialContent {
		return nil, fmt.Errorf("bad status code: %d", res.StatusCode)
	}

	byts, err := io.ReadAll(&limitedreader.Reader{R: res.Body, N: clientMaxInboundSegmentSize})
	if err != nil {
		return nil, err
	}

	return byts, nil
}

func equalUint64Ptr(a *uint64, b *uint64) bool {
	if a == nil || b == nil {
		return a == b
	}
	return *a == *b
}

func (d *clientStreamDownloader) downloadNextSegment(
	errGroupCtx context.Context,
	pl *playlist.Media,
) (*playlist.MediaSegment, []byte, int64, error) {
	// discontinuities do not allow switching the initialization object
	if d.epochManager != nil && pl.Map != nil && d.firstPlaylist.Map != nil {
		if pl.Map.URI != d.firstPlaylist.Map.URI ||
			!equalUint64Ptr(pl.Map.ByteRangeStart, d.firstPlaylist.Map.ByteRangeStart) ||
			!equalUint64Ptr(pl.Map.ByteRangeLength, d.firstPlaylist.Map.ByteRangeLength) {
			return nil, nil, 0, fmt.Errorf("changing the initialization object at a discontinuity is not supported")
		}
	}

	var seg *playlist.MediaSegment
	var segPos int

	if d.curSegmentID == nil {
		if (d.firstPlaylist.PlaylistType != nil &&
			*d.firstPlaylist.PlaylistType == playlist.MediaPlaylistTypeVOD) || d.firstPlaylist.Endlist {
			// VOD stream: start from the beginning
			if len(pl.Segments) == 0 {
				return nil, nil, 0, fmt.Errorf("no segments found")
			}
			seg = pl.Segments[0]
		} else {
			// live stream: start from clientLiveInitialDistance
			seg, segPos = findSegmentWithInvPosition(pl.Segments, d.startDistance)
			if seg == nil {
				return nil, nil, 0, fmt.Errorf("there aren't enough segments to fill the buffer")
			}
		}
	} else {
		var invPos int
		seg, segPos, invPos = findSegmentWithID(pl.MediaSequence, pl.Segments, *d.curSegmentID+1)
		if seg == nil {
			if pl.Endlist {
				return nil, nil, 0, ErrClientEOS
			}
			return nil, nil, 0, fmt.Errorf("next segment not found or not ready yet")
		}

		if !pl.Endlist && invPos > d.maxDistance {
			return nil, nil, 0, fmt.Errorf("playback is too late")
		}
	}

	d.curSegmentID = new(pl.MediaSequence + segPos)

	epoch := discontinuityEpochOf(pl, segPos)

	byts, err := d.downloadSegment(errGroupCtx, seg.URI,
		new(segmentByteRangeStart(pl.Segments, segPos)), seg.ByteRangeLength)
	if err != nil {
		return nil, nil, 0, err
	}

	return seg, byts, epoch, nil
}

func (d *clientStreamDownloader) startStreaming(
	errGroupCtx context.Context,
	allTracks map[*Track]*clientTrack,
) bool {
	select {
	case d.chStartStreaming <- allTracks:
		return true
	case <-errGroupCtx.Done():
		return false
	}
}

func (d *clientStreamDownloader) onProcessorError(errGroupCtx context.Context, err error) {
	d.parent.onProcessorError(errGroupCtx, err)
}

func (d *clientStreamDownloader) setTimeConv(ts clientTimeConv) {
	d.parent.setTimeConv(ts)
}

func (d *clientStreamDownloader) waitTimeConv(errGroupCtx context.Context) (clientTimeConv, bool) {
	return d.parent.waitTimeConv(errGroupCtx)
}

func (d *clientStreamDownloader) setTracks(errGroupCtx context.Context, tracks []*Track) ([]*clientTrack, bool) {
	select {
	case d.chTracks <- tracks:
	case <-errGroupCtx.Done():
		return nil, false
	}

	var allTracks map[*Track]*clientTrack

	select {
	case allTracks = <-d.chStartStreaming:
	case <-errGroupCtx.Done():
		return nil, false
	}

	streamTracks := make([]*clientTrack, len(tracks))
	for i, track := range tracks {
		streamTracks[i] = allTracks[track]
	}

	return streamTracks, true
}
