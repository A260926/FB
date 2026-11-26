package gohlslib

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"sync"

	"github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4"
	"golang.org/x/sync/errgroup"

	"github.com/bluenviron/gohlslib/v2/pkg/playlist"
)

func fmp4PickLeadingTrack(init *fmp4.Init) int {
	// pick first video track
	for _, track := range init.Tracks {
		if track.Codec.IsVideo() {
			return track.ID
		}
	}

	// otherwise, pick first track
	return init.Tracks[0].ID
}

func findFirstPartTrackOfLeadingTrack(parts []*fmp4.Part, leadingTrackID int) *fmp4.PartTrack {
	for _, part := range parts {
		for _, partTrack := range part.Tracks {
			if partTrack.ID == leadingTrackID {
				return partTrack
			}
		}
	}
	return nil
}

func findTimeScaleOfLeadingTrack(tracks []*fmp4.InitTrack, leadingTrackID int) uint32 {
	for _, track := range tracks {
		if track.ID == leadingTrackID {
			return track.TimeScale
		}
	}
	return 0
}

type clientStreamProcessorFMP4 struct {
	errGroupCtx  context.Context
	isLeading    bool
	rendition    *playlist.MultivariantRendition
	initFile     []byte
	segmentQueue *clientSegmentQueue
	errGroup     *errgroup.Group
	parent       clientStreamProcessorParent
	epochManager *clientFMP4EpochManager
	epochStream  *clientFMP4EpochStream
	enableEpochs bool

	init               fmp4.Init
	leadingTrackID     int
	leadingTrackRate   int64
	trackProcessors    map[int]*clientTrackProcessorFMP4
	clientStreamTracks []*clientTrack
	timeConv           *clientTimeConvFMP4
	segmentWaitGroup   *sync.WaitGroup

	lastDeliveredEnd map[int]int64
	playbackStarted  bool
}

func (p *clientStreamProcessorFMP4) initialize() {
	if p.epochManager != nil {
		p.enableEpochs = true
		p.epochStream = p.epochManager.newStream(p.isLeading)
	}
}

func (p *clientStreamProcessorFMP4) run(errGroupCtx context.Context) error {
	err := p.init.Unmarshal(bytes.NewReader(p.initFile))
	if err != nil {
		return err
	}

	if !p.isLeading && len(p.init.Tracks) != 1 {
		return fmt.Errorf("rendition playlists with multiple tracks are not supported")
	}

	p.leadingTrackID = fmp4PickLeadingTrack(&p.init)

	tracks := make([]*Track, len(p.init.Tracks))

	for i, track := range p.init.Tracks {
		tracks[i] = &Track{
			Codec:     fromFMP4(track.Codec),
			ClockRate: int(track.TimeScale),
			Name: func() string {
				if !p.isLeading {
					return p.rendition.Name
				}
				return ""
			}(),
			Language: func() string {
				if !p.isLeading {
					return p.rendition.Language
				}
				return ""
			}(),
			IsDefault: func() bool {
				if !p.isLeading {
					return p.rendition.Default
				}
				return false
			}(),
		}
	}

	if len(tracks) > clientMaxTracksPerStream {
		return fmt.Errorf("too many tracks per stream")
	}

	var ok bool
	p.clientStreamTracks, ok = p.parent.setTracks(p.errGroupCtx, tracks)
	if !ok {
		return fmt.Errorf("terminated")
	}

	for {
		var seg *segmentData
		seg, ok = p.segmentQueue.pull(errGroupCtx)
		if !ok {
			return fmt.Errorf("terminated")
		}

		if seg.err != nil {
			if p.enableEpochs {
				if errors.Is(seg.err, ErrClientEOS) {
					p.epochManager.markEOS(p.epochStream)
				} else {
					p.epochManager.fail(seg.err)
				}
			}
			p.parent.onProcessorError(errGroupCtx, seg.err)
			<-errGroupCtx.Done()
			return fmt.Errorf("terminated")
		}

		err = p.processSegment(errGroupCtx, seg)
		if err != nil {
			if p.enableEpochs {
				p.epochManager.fail(err)
				p.epochManager.markEOS(p.epochStream)
			}
			return err
		}
	}
}

func (p *clientStreamProcessorFMP4) processSegment(errGroupCtx context.Context, seg *segmentData) error {
	var parts fmp4.Parts
	err := parts.Unmarshal(seg.payload)
	if err != nil {
		return err
	}

	leadingPartTrack := findFirstPartTrackOfLeadingTrack(parts, p.leadingTrackID)
	if leadingPartTrack == nil {
		return fmt.Errorf("could not find data of leading track")
	}

	if p.trackProcessors == nil {
		err = p.initializeTrackProcessors(errGroupCtx)
		if err != nil {
			return err
		}

		if !p.enableEpochs {
			err = p.initializeTimeConv(errGroupCtx, leadingPartTrack)
			if err != nil {
				return err
			}
		}
	}

	if p.enableEpochs {
		return p.processSegmentEpoch(errGroupCtx, seg, parts, leadingPartTrack)
	}

	if p.isLeading {
		if dateTime := seg.dateTimePtr(); dateTime != nil {
			leadingPartTrackProc := p.trackProcessors[leadingPartTrack.ID]
			dts := p.timeConv.
				convert(int64(leadingPartTrack.BaseTime), leadingPartTrackProc.track.track.ClockRate)
			p.timeConv.
				setNTP(*dateTime, dts, leadingPartTrackProc.track.track.ClockRate)
		}
		p.timeConv.setLeadingNTPReceived()
	}

	p.segmentWaitGroup = &sync.WaitGroup{}

	for _, part := range parts {
		for _, partTrack := range part.Tracks {
			trackProc, ok := p.trackProcessors[partTrack.ID]
			if !ok {
				continue
			}

			dts := p.timeConv.convert(int64(partTrack.BaseTime), trackProc.track.track.ClockRate)
			ntp := p.timeConv.getNTP(errGroupCtx, dts, trackProc.track.track.ClockRate)

			p.segmentWaitGroup.Add(1)

			err = trackProc.push(errGroupCtx, &procEntryFMP4{
				partTrack: partTrack,
				dts:       dts,
				ntp:       ntp,
			})
			if err != nil {
				p.segmentWaitGroup.Done()
				return err
			}
		}
	}

	p.segmentWaitGroup.Wait()

	return nil
}

func (p *clientStreamProcessorFMP4) onPartTrackProcessed() {
	p.segmentWaitGroup.Done()
}

// processSegmentEpoch converts and delivers a segment when
// EnableFMP4Discontinuities is active. All samples are validated and decoded
// before the segment's epoch anchor is committed, so that a bad segment is
// rejected without publishing any shared state.
func (p *clientStreamProcessorFMP4) processSegmentEpoch(
	errGroupCtx context.Context,
	seg *segmentData,
	parts fmp4.Parts,
	leadingPartTrack *fmp4.PartTrack,
) error {
	if p.leadingTrackRate == 0 {
		p.leadingTrackRate = int64(findTimeScaleOfLeadingTrack(p.init.Tracks, p.leadingTrackID))
	}

	if p.lastDeliveredEnd == nil {
		p.lastDeliveredEnd = make(map[int]int64)
	}

	plan, err := p.epochManager.resolveSegment(
		errGroupCtx,
		p.epochStream,
		seg.epoch,
		seg.dateTime,
		seg.hasPDT,
		p.leadingTrackRate,
		int64(leadingPartTrack.BaseTime))
	if err != nil {
		return err
	}

	// validate and decode every sample before publishing the segment anchor
	entries := make(map[int]*procEntryFMP4, len(p.trackProcessors))
	endTimes := make(map[int]int64, len(p.trackProcessors))

	for _, part := range parts {
		for _, partTrack := range part.Tracks {
			trackProc, ok := p.trackProcessors[partTrack.ID]
			if !ok {
				continue
			}

			if _, ok2 := entries[partTrack.ID]; ok2 {
				return fmt.Errorf("multiple parts of the same track in a single segment are not supported")
			}

			clockRate := int64(trackProc.track.track.ClockRate)

			dts := plan.anchor.firstDTS(int64(partTrack.BaseTime), clockRate)
			if last, hasLast := p.lastDeliveredEnd[partTrack.ID]; hasLast && dts < last {
				return fmt.Errorf("segment starts before the end of the previous segment")
			}

			samples, end, decErr := p.prepareEpochSamples(plan.anchor, partTrack, trackProc, clockRate, dts)
			if decErr != nil {
				return decErr
			}

			entries[partTrack.ID] = &procEntryFMP4{epochSamples: samples}
			endTimes[partTrack.ID] = end
		}
	}

	// keep the usual playback pacing: it starts when the leading rendition
	// publishes its first anchor. This is done before the anchor is published
	// so that other renditions cannot start delivering samples concurrently.
	if p.isLeading && !p.playbackStarted {
		p.playbackStarted = true

		marker := &clientTimeConvFMP4{
			leadingTimeScale: p.leadingTrackRate,
			leadingBaseTime:  int64(leadingPartTrack.BaseTime),
		}
		marker.initialize()
		marker.setNTP(plan.anchor.q, 0, int(p.leadingTrackRate))
		marker.setLeadingNTPReceived()

		p.parent.setTimeConv(marker)
	}

	// all samples are valid: publish the anchor and hand the samples over
	err = p.epochManager.commitSegment(errGroupCtx, p.epochStream, plan)
	if err != nil {
		return err
	}

	p.segmentWaitGroup = &sync.WaitGroup{}

	for trackID, entry := range entries {
		trackProc := p.trackProcessors[trackID]

		p.segmentWaitGroup.Add(1)

		err = trackProc.push(errGroupCtx, entry)
		if err != nil {
			p.segmentWaitGroup.Done()
			return err
		}

		p.lastDeliveredEnd[trackID] = endTimes[trackID]
	}

	p.segmentWaitGroup.Wait()

	return nil
}

func (p *clientStreamProcessorFMP4) prepareEpochSamples(
	anchor *clientFMP4EpochAnchor,
	partTrack *fmp4.PartTrack,
	trackProc *clientTrackProcessorFMP4,
	clockRate int64,
	startDTS int64,
) ([]procEntryFMP4Sample, int64, error) {
	samples := make([]procEntryFMP4Sample, 0, len(partTrack.Samples))

	dts := startDTS
	raw := int64(partTrack.BaseTime)

	for _, sample := range partTrack.Samples {
		data, err := trackProc.decodePayload(sample)
		if err != nil {
			return nil, 0, err
		}

		ntp := anchor.absoluteTime(raw, clockRate)

		samples = append(samples, procEntryFMP4Sample{
			pts:  dts + int64(sample.PTSOffset),
			dts:  dts,
			ntp:  &ntp,
			data: data,
		})

		dts += int64(sample.Duration)
		raw += int64(sample.Duration)
	}

	return samples, dts, nil
}

func (p *clientStreamProcessorFMP4) initializeTrackProcessors(errGroupCtx context.Context) error {
	p.trackProcessors = make(map[int]*clientTrackProcessorFMP4)

	for i, track := range p.clientStreamTracks {
		trackProc := &clientTrackProcessorFMP4{
			track:           track,
			streamProcessor: p,
		}
		err := trackProc.initialize()
		if err != nil {
			return err
		}

		p.errGroup.Go(func() error {
			return trackProc.run(errGroupCtx)
		})

		p.trackProcessors[p.init.Tracks[i].ID] = trackProc
	}

	return nil
}

func (p *clientStreamProcessorFMP4) initializeTimeConv(
	errGroupCtx context.Context,
	leadingPartTrack *fmp4.PartTrack,
) error {
	if p.isLeading {
		timeScale := findTimeScaleOfLeadingTrack(p.init.Tracks, p.leadingTrackID)

		p.timeConv = &clientTimeConvFMP4{
			leadingTimeScale: int64(timeScale),
			leadingBaseTime:  int64(leadingPartTrack.BaseTime),
		}
		p.timeConv.initialize()

		p.parent.setTimeConv(p.timeConv)
	} else {
		tmp, ok := p.parent.waitTimeConv(errGroupCtx)
		if !ok {
			return fmt.Errorf("terminated")
		}

		p.timeConv, ok = tmp.(*clientTimeConvFMP4)
		if !ok {
			return fmt.Errorf("stream playlists are mixed MPEG-TS/fMP4")
		}
	}

	return nil
}
