package astisub

import (
	"fmt"
	"math/bits"
	"sort"
	"time"
)

// EditClip represents one segment of an edit list. In and Out reference the
// source's own half-open interval [In, Out) and Duration is the length this
// segment occupies in the assembled timeline.
type EditClip struct {
	Source   *Subtitles
	In       time.Duration
	Out      time.Duration
	Duration time.Duration
}

// editMapper maps source times of one clip into the assembled timeline
type editMapper struct {
	in        time.Duration
	out       time.Duration
	duration  time.Duration
	offset    time.Duration
	clipStart time.Duration
	clipEnd   time.Duration
	newStart  time.Duration
}

// mapTime maps a source time into the assembled timeline. The offset within
// the segment is scaled by Duration/(Out-In) and rounded down to the
// nanosecond, then shifted by the sum of all previous segments' durations.
// Each endpoint is computed from its original value, never from previously
// rounded results.
func (m editMapper) mapTime(t time.Duration) time.Duration {
	return m.offset + scaleEditDuration(t-m.in, m.duration, m.out-m.in)
}

// scaleEditDuration computes d*mul/div rounded down to the nanosecond. The
// 128-bit intermediate keeps the multiplication of two nanosecond durations
// from overflowing; the result always fits since d never exceeds div.
func scaleEditDuration(d, mul, div time.Duration) time.Duration {
	hi, lo := bits.Mul64(uint64(d), uint64(mul))
	q, _ := bits.Div64(hi, lo, uint64(div))
	return time.Duration(q)
}

// editNamespace holds the renamed styles and regions of one source
type editNamespace struct {
	styles  map[string]*Style
	regions map[string]*Region
}

// style returns the renamed counterpart of a source style
func (n *editNamespace) style(s *Style) *Style {
	if s == nil {
		return nil
	}
	if r, ok := n.styles[s.ID]; ok {
		return r
	}
	return s
}

// region returns the renamed counterpart of a source region
func (n *editNamespace) region(r *Region) *Region {
	if r == nil {
		return nil
	}
	if v, ok := n.regions[r.ID]; ok {
		return v
	}
	return r
}

// Assemble assembles an edit list of clips into new subtitles on a new
// timeline starting at zero. Clips are concatenated in the given order: each
// clip's events are intersected with its own [In, Out) interval and linearly
// mapped into an output segment of length Duration. The same source may
// appear several times and intervals may overlap or be out of order. Styles
// and regions of each source are namespaced with s1_, s2_, ... prefixes in
// order of first appearance so same-named definitions don't collide. Neither
// the input subtitles nor the edit list are modified. The output starts on a
// fresh timeline and doesn't inherit any source metadata.
func Assemble(clips []EditClip) (*Subtitles, error) {
	// An empty edit list yields usable empty subtitles
	o := NewSubtitles()
	if len(clips) == 0 {
		return o, nil
	}

	// Validate clips, attribute namespaces and compute segment offsets
	const maxDuration = time.Duration(1<<63 - 1)
	var offset time.Duration
	var offsets = make([]time.Duration, len(clips))
	var namespaces = make(map[*Subtitles]string)
	var sources []*Subtitles
	for i, c := range clips {
		if c.Source == nil {
			return nil, fmt.Errorf("astisub: edit clip %d: source is nil", i)
		}
		if c.In < 0 {
			return nil, fmt.Errorf("astisub: edit clip %d: in %s is negative", i, c.In)
		}
		if c.Out <= c.In {
			return nil, fmt.Errorf("astisub: edit clip %d: out %s is not greater than in %s", i, c.Out, c.In)
		}
		if c.Duration <= 0 {
			return nil, fmt.Errorf("astisub: edit clip %d: duration %s is not positive", i, c.Duration)
		}
		if c.Duration > maxDuration-offset {
			return nil, fmt.Errorf("astisub: edit clip %d: cumulative duration overflows", i)
		}
		offsets[i] = offset
		offset += c.Duration
		if _, ok := namespaces[c.Source]; !ok {
			namespaces[c.Source] = fmt.Sprintf("s%d_", len(sources)+1)
			sources = append(sources, c.Source)
		}
	}

	// Validate source events
	for _, source := range sources {
		if err := validateEditSource(source); err != nil {
			return nil, err
		}
	}

	// Build namespaced styles and regions
	var builds = make(map[*Subtitles]*editNamespace)
	for _, source := range sources {
		builds[source] = buildEditNamespace(source, namespaces[source], o)
	}

	// Process clips in edit list order
	for i, c := range clips {
		var ns = builds[c.Source]
		var m = editMapper{in: c.In, out: c.Out, duration: c.Duration, offset: offsets[i]}
		for _, item := range c.Source.Items {
			// Intersect the event with the clip's [In, Out) interval
			var start, end = item.StartAt, item.EndAt
			if c.In > start {
				start = c.In
			}
			if c.Out < end {
				end = c.Out
			}
			if start >= end {
				continue
			}

			// Map both endpoints from their original values
			m.clipStart = start
			m.clipEnd = end
			m.newStart = m.mapTime(start)
			var newEnd = m.mapTime(end)

			// Drop events quantized to zero length
			if newEnd <= m.newStart {
				continue
			}

			// Copy the item, sharing attribute objects read-only
			var newItem = &Item{
				Comments:    append([]string(nil), item.Comments...),
				EndAt:       newEnd,
				InlineStyle: item.InlineStyle,
				Region:      ns.region(item.Region),
				StartAt:     m.newStart,
				Style:       ns.style(item.Style),
			}
			for _, l := range item.Lines {
				if nl := assembleEditLine(l, item.StartAt, item.EndAt, m, ns); len(nl.Items) > 0 {
					newItem.Lines = append(newItem.Lines, nl)
				}
			}
			o.Items = append(o.Items, newItem)
		}
	}

	// Order by start time, keeping edit list then source order on ties
	sort.SliceStable(o.Items, func(i, j int) bool {
		return o.Items[i].StartAt < o.Items[j].StartAt
	})

	// Renumber consecutively
	for i, item := range o.Items {
		item.Index = i + 1
	}
	return o, nil
}

// validateEditSource validates the events of a source used in an edit list
func validateEditSource(s *Subtitles) (err error) {
	for _, item := range s.Items {
		if item.EndAt < item.StartAt {
			return fmt.Errorf("astisub: source event ends at %s before it starts at %s", item.EndAt, item.StartAt)
		}
		for _, l := range item.Lines {
			var previous time.Duration
			var hasPrevious bool
			for _, li := range l.Items {
				if li.StartAt == 0 {
					continue
				}
				if li.StartAt < item.StartAt || li.StartAt > item.EndAt {
					return fmt.Errorf("astisub: word start %s is outside event [%s, %s]", li.StartAt, item.StartAt, item.EndAt)
				}
				if hasPrevious && li.StartAt < previous {
					return fmt.Errorf("astisub: word start %s decreases after %s", li.StartAt, previous)
				}
				previous, hasPrevious = li.StartAt, true
			}
		}
	}
	return
}

// buildEditNamespace renames a source's styles and regions with the given
// prefix and adds them to the output, keeping parent style chains intact.
// Attribute objects are shared read-only.
func buildEditNamespace(s *Subtitles, prefix string, o *Subtitles) *editNamespace {
	var n = &editNamespace{
		regions: make(map[string]*Region),
		styles:  make(map[string]*Style),
	}

	// Rename styles
	var ids []string
	for id := range s.Styles {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for _, id := range ids {
		var st = s.Styles[id]
		n.styles[st.ID] = &Style{ID: prefix + st.ID, InlineStyle: st.InlineStyle}
	}
	for _, id := range ids {
		if st := s.Styles[id]; st.Style != nil {
			if p, ok := n.styles[st.Style.ID]; ok {
				n.styles[st.ID].Style = p
			} else {
				n.styles[st.ID].Style = st.Style
			}
		}
	}

	// Rename regions
	ids = ids[:0]
	for id := range s.Regions {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for _, id := range ids {
		var r = s.Regions[id]
		n.regions[r.ID] = &Region{ID: prefix + r.ID, InlineStyle: r.InlineStyle}
	}
	for _, id := range ids {
		if r := s.Regions[id]; r.Style != nil {
			if st, ok := n.styles[r.Style.ID]; ok {
				n.regions[r.ID].Style = st
			} else {
				n.regions[r.ID].Style = r.Style
			}
		}
	}

	// Add to output
	for _, st := range n.styles {
		o.Styles[st.ID] = st
	}
	for _, r := range n.regions {
		o.Regions[r.ID] = r
	}
	return n
}

// assembleEditLine cuts a source line's word groups to the clipped event
// interval and maps the kept groups into the assembled timeline. A non-zero
// LineItem.StartAt starts a new word group at that absolute source time,
// zero-value fragments continue the previous group and the first group
// defaults to the event start. A group lasts until the next non-zero start
// or the event end. Only groups intersecting the clipped event interval are
// kept, with their text, tags, voice and order unchanged. Kept group starts
// are clamped to the intersection start before mapping; markers landing on
// the new event start and continuation fragments are written as zero.
func assembleEditLine(l Line, itemStartAt, itemEndAt time.Duration, m editMapper, n *editNamespace) (o Line) {
	o.VoiceName = l.VoiceName

	// Build word groups
	type wordGroup struct {
		start time.Duration
		items []LineItem
	}
	var groups []wordGroup
	for _, li := range l.Items {
		if li.StartAt != 0 || len(groups) == 0 {
			var start = li.StartAt
			if start == 0 {
				start = itemStartAt
			}
			groups = append(groups, wordGroup{start: start})
		}
		groups[len(groups)-1].items = append(groups[len(groups)-1].items, li)
	}

	// Keep groups intersecting the clipped event interval
	for i, g := range groups {
		var end = itemEndAt
		if i+1 < len(groups) {
			end = groups[i+1].start
		}
		var start = g.start
		if m.clipStart > start {
			start = m.clipStart
		}
		if m.clipEnd < end {
			end = m.clipEnd
		}
		if start >= end {
			continue
		}

		// Map the clamped group start; a marker landing on the new event
		// start is written as zero, and so are continuation fragments
		var mapped = m.mapTime(start)
		for j, li := range g.items {
			var nli = LineItem{
				InlineStyle: li.InlineStyle,
				Style:       n.style(li.Style),
				Text:        li.Text,
			}
			if j == 0 && mapped != m.newStart {
				nli.StartAt = mapped
			}
			o.Items = append(o.Items, nli)
		}
	}
	return
}
