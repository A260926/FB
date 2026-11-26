package gohlslib

import (
	"context"
	"fmt"
	"sync"
	"time"

	"github.com/bluenviron/gohlslib/v2/pkg/playlist"
)

// floorMulDiv returns floor(v*m/d), using integer arithmetic only.
// d must be positive.
func floorMulDiv(v, m, d int64) int64 {
	q := v / d
	r := v % d
	if r < 0 {
		q--
		r += d
	}
	return q*m + r*m/d
}

// floorDurationToTimestamp converts a duration into clock ticks, rounding towards -infinity.
func floorDurationToTimestamp(v time.Duration, clockRate int64) int64 {
	return floorMulDiv(int64(v), clockRate, int64(time.Second))
}

// floorTimestampToDuration converts clock ticks into a duration, rounding towards -infinity.
func floorTimestampToDuration(v int64, clockRate int64) time.Duration {
	return time.Duration(floorMulDiv(v, int64(time.Second), clockRate))
}

// discontinuityEpochOf returns the absolute discontinuity sequence of the
// segment at position pos: EXT-X-DISCONTINUITY-SEQUENCE (0 when absent) plus
// the number of EXT-X-DISCONTINUITY tags from the beginning of the playlist
// through the segment itself.
func discontinuityEpochOf(pl *playlist.Media, pos int) int64 {
	var ret int64

	if pl.DiscontinuitySequence != nil {
		ret = int64(*pl.DiscontinuitySequence)
	}

	for i := 0; i <= pos && i < len(pl.Segments); i++ {
		if pl.Segments[i].Discontinuity {
			ret++
		}
	}

	return ret
}

// clientFMP4EpochAnchor contains the media/time anchors of a rendition for a
// single discontinuity generation. Anchors are immutable: a new generation
// gets a new anchor, so a rendition that is still in the previous generation
// is never affected by a leading rendition that already entered the next one.
type clientFMP4EpochAnchor struct {
	q                time.Time
	t0               time.Time
	leadingBase      int64
	leadingClockRate int64
}

func (a *clientFMP4EpochAnchor) leadingBaseAt(clockRate int64) int64 {
	return floorMulDiv(a.leadingBase, clockRate, a.leadingClockRate)
}

func (a *clientFMP4EpochAnchor) firstDTS(rawBase int64, clockRate int64) int64 {
	return floorDurationToTimestamp(a.q.Sub(a.t0), clockRate) +
		rawBase - a.leadingBaseAt(clockRate)
}

func (a *clientFMP4EpochAnchor) absoluteTime(rawBase int64, clockRate int64) time.Time {
	return a.q.Add(floorTimestampToDuration(rawBase-a.leadingBaseAt(clockRate), clockRate))
}

// clientFMP4EpochStream is the identity of a rendition inside the shared
// epoch manager. It travels from the downloader to the fMP4 processor.
type clientFMP4EpochStream struct {
	isLeading bool

	hasEpoch  bool
	lastEpoch int64
	anchors   map[int64]*clientFMP4EpochAnchor
	eos       bool
}

// clientFMP4EpochManager coordinates discontinuity generations between
// renditions of the same client.
//
// It keeps at most two adjacent generations. A leading rendition that wants to
// enter a third generation waits until every non-EOS rendition has left the
// oldest one. All waits are released by context cancellation, by an error, or
// by the end of a rendition.
type clientFMP4EpochManager struct {
	mutex sync.Mutex

	notify chan struct{}
	err    error

	// set when the leading rendition is not an fMP4 one and cannot provide a
	// time origin; renditions waiting for it must fail like the legacy code
	// does for mixed MPEG-TS/fMP4 streams.
	unsupported bool

	t0        time.Time
	t0Set     bool
	leaderEOS bool

	initialEpoch int64
	initialSet   bool

	generations map[int64]struct{}
	streams     map[*clientFMP4EpochStream]struct{}

	// signalled when a rendition has to wait for the two-generation limit to
	// be released; consumers may observe it to know a wait is in progress.
	chGenerationWait chan struct{}
}

func newClientFMP4EpochManager() *clientFMP4EpochManager {
	return &clientFMP4EpochManager{
		notify:           make(chan struct{}),
		chGenerationWait: make(chan struct{}, 1),
		generations:      make(map[int64]struct{}),
		streams:          make(map[*clientFMP4EpochStream]struct{}),
	}
}

func (m *clientFMP4EpochManager) newStream(isLeading bool) *clientFMP4EpochStream {
	s := &clientFMP4EpochStream{
		isLeading: isLeading,
		anchors:   make(map[int64]*clientFMP4EpochAnchor),
	}

	m.mutex.Lock()
	m.streams[s] = struct{}{}
	m.mutex.Unlock()

	return s
}

func (m *clientFMP4EpochManager) broadcastLocked() {
	close(m.notify)
	m.notify = make(chan struct{})
}

func (m *clientFMP4EpochManager) fail(err error) {
	m.mutex.Lock()
	defer m.mutex.Unlock()

	if m.err == nil {
		m.err = err
	}
	m.broadcastLocked()
}

// setUnsupported marks the epoch manager as unable to provide a time origin.
func (m *clientFMP4EpochManager) setUnsupported() {
	m.mutex.Lock()
	defer m.mutex.Unlock()

	m.unsupported = true
	m.broadcastLocked()
}

func (m *clientFMP4EpochManager) markEOS(s *clientFMP4EpochStream) {
	m.mutex.Lock()
	defer m.mutex.Unlock()

	if s.eos {
		return
	}

	s.eos = true

	if s.isLeading && !m.t0Set {
		m.leaderEOS = true
	}
	m.broadcastLocked()
}

func (m *clientFMP4EpochManager) waitNotify(ctx context.Context, notify chan struct{}) error {
	select {
	case <-notify:
		return nil
	case <-ctx.Done():
		return fmt.Errorf("terminated")
	}
}

func (m *clientFMP4EpochManager) minGenerationLocked() int64 {
	var ret int64
	first := true

	for gen := range m.generations {
		if first || gen < ret {
			ret = gen
			first = false
		}
	}

	return ret
}

func (m *clientFMP4EpochManager) maxGenerationLocked() int64 {
	var ret int64
	first := true

	for gen := range m.generations {
		if first || gen > ret {
			ret = gen
			first = false
		}
	}

	return ret
}

func (m *clientFMP4EpochManager) generationInUseLocked(
	gen int64,
	except *clientFMP4EpochStream,
) bool {
	for s := range m.streams {
		if s == except || s.eos {
			continue
		}

		if !s.hasEpoch {
			// a rendition that has not started yet may still begin in the
			// initial generation, so it keeps that generation alive
			if m.initialSet && m.initialEpoch <= gen {
				return true
			}
			continue
		}

		if s.lastEpoch <= gen {
			return true
		}
	}

	return false
}

type clientFMP4EpochSegmentPlan struct {
	anchor      *clientFMP4EpochAnchor
	epoch       int64
	epochChange bool
	t0Candidate *time.Time
}

// resolveSegment returns the anchor that must be used to convert a segment.
// It does not update any shared state; callers can therefore fully validate a
// segment and only commit its anchor afterwards.
func (m *clientFMP4EpochManager) resolveSegment(
	ctx context.Context,
	s *clientFMP4EpochStream,
	epoch int64,
	dateTime time.Time,
	hasDateTime bool,
	leadingClockRate int64,
	leadingBase int64,
) (*clientFMP4EpochSegmentPlan, error) {
	for {
		m.mutex.Lock()

		if m.err != nil {
			err := m.err
			m.mutex.Unlock()
			return nil, err
		}

		if m.unsupported {
			m.mutex.Unlock()
			return nil, fmt.Errorf("stream playlists are mixed MPEG-TS/fMP4")
		}

		if s.hasEpoch {
			if epoch < s.lastEpoch {
				m.mutex.Unlock()
				return nil, fmt.Errorf("discontinuity sequence went backwards")
			}
			if epoch > s.lastEpoch+1 {
				m.mutex.Unlock()
				return nil, fmt.Errorf("discontinuity sequence jumped")
			}
		}

		if anchor, ok := s.anchors[epoch]; ok {
			m.mutex.Unlock()
			return &clientFMP4EpochSegmentPlan{
				anchor: anchor,
				epoch:  epoch,
			}, nil
		}

		// the first segment of a stream and the first segment of every
		// generation must carry a PROGRAM-DATE-TIME
		epochChange := !s.hasEpoch || epoch != s.lastEpoch
		if epochChange && !hasDateTime {
			m.mutex.Unlock()
			return nil, fmt.Errorf("missing PROGRAM-DATE-TIME")
		}

		var t0 time.Time
		var t0Candidate *time.Time

		if m.t0Set {
			t0 = m.t0
		} else {
			if !s.isLeading {
				if m.leaderEOS {
					m.mutex.Unlock()
					return nil, fmt.Errorf("leading stream ended before providing a time anchor")
				}

				notify := m.notify
				m.mutex.Unlock()

				if err := m.waitNotify(ctx, notify); err != nil {
					return nil, err
				}
				continue
			}

			t0 = dateTime
			t0Candidate = new(t0)
		}

		if !s.hasEpoch && m.initialSet && epoch != m.initialEpoch {
			err := fmt.Errorf("streams started in different discontinuity sequences")
			m.mutex.Unlock()
			return nil, err
		}

		anchor := &clientFMP4EpochAnchor{
			q:                dateTime,
			t0:               t0,
			leadingBase:      leadingBase,
			leadingClockRate: leadingClockRate,
		}

		m.mutex.Unlock()

		return &clientFMP4EpochSegmentPlan{
			anchor:      anchor,
			epoch:       epoch,
			epochChange: epochChange,
			t0Candidate: t0Candidate,
		}, nil
	}
}

// commitSegment publishes a validated anchor. It may wait when the two
// generation limit is reached; in that case no state is changed until the
// wait ends.
func (m *clientFMP4EpochManager) commitSegment(
	ctx context.Context,
	s *clientFMP4EpochStream,
	plan *clientFMP4EpochSegmentPlan,
) error {
	for {
		m.mutex.Lock()

		if m.err != nil {
			err := m.err
			m.mutex.Unlock()
			return err
		}

		if m.unsupported {
			m.mutex.Unlock()
			return fmt.Errorf("stream playlists are mixed MPEG-TS/fMP4")
		}

		if !s.hasEpoch {
			if m.initialSet && plan.epoch != m.initialEpoch {
				m.mutex.Unlock()
				return fmt.Errorf("streams started in different discontinuity sequences")
			}
		} else {
			if plan.epoch < s.lastEpoch {
				m.mutex.Unlock()
				return fmt.Errorf("discontinuity sequence went backwards")
			}
			if plan.epoch > s.lastEpoch+1 {
				m.mutex.Unlock()
				return fmt.Errorf("discontinuity sequence jumped")
			}
		}

		_, generationExists := m.generations[plan.epoch]

		if !generationExists && len(m.generations) >= 2 {
			oldest := m.minGenerationLocked()
			newest := m.maxGenerationLocked()

			if plan.epoch != newest+1 {
				m.mutex.Unlock()
				return fmt.Errorf("discontinuity sequence is not adjacent to the current generation")
			}

			if m.generationInUseLocked(oldest, s) {
				notify := m.notify
				if m.chGenerationWait != nil {
					select {
					case m.chGenerationWait <- struct{}{}:
					default:
					}
				}
				m.mutex.Unlock()

				if err := m.waitNotify(ctx, notify); err != nil {
					return err
				}
				continue
			}

			delete(m.generations, oldest)
		}

		if !generationExists {
			m.generations[plan.epoch] = struct{}{}
		}

		if !m.t0Set {
			if plan.t0Candidate == nil {
				m.mutex.Unlock()
				return fmt.Errorf("time anchor was not resolved")
			}

			m.t0 = *plan.t0Candidate
			m.t0Set = true
		}

		if !m.initialSet {
			m.initialSet = true
			m.initialEpoch = plan.epoch
		}

		if plan.epochChange {
			s.anchors[plan.epoch] = plan.anchor

			for gen := range s.anchors {
				if gen < plan.epoch-1 {
					delete(s.anchors, gen)
				}
			}

			s.hasEpoch = true
			s.lastEpoch = plan.epoch
		}

		m.broadcastLocked()
		m.mutex.Unlock()
		return nil
	}
}
