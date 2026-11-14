package astisub

import (
	"errors"
	"fmt"
	"math/big"
	"sort"
	"time"
)

// EditClip describes a segment of a source subtitle track to use in an
// assembled subtitle track. In and Out are boundaries in the source's own
// timeline (a half-open interval [In, Out)); Duration is the length the
// segment occupies in the assembled track.
type EditClip struct {
	Source   *Subtitles
	In       time.Duration
	Out      time.Duration
	Duration time.Duration
}

const maxAssembledDuration = time.Duration(1<<63 - 1)

// Assemble assembles the clips, in the order they are given, into a new
// subtitle track starting at zero. Clips are never reordered, so the same
// source may be referenced several times, source ranges may overlap, and the
// ranges may be selected in reverse order. The input subtitles and clip table
// are never modified.
func Assemble(clips []EditClip) (*Subtitles, error) {
	// Nothing to assemble: still return a usable subtitle track.
	if len(clips) == 0 {
		return NewSubtitles(), nil
	}

	// Validate the clip table and compute the output length before doing any
	// work, so that an overflowing timeline never yields a partial result.
	var total time.Duration
	var sources []*Subtitles
	seen := make(map[*Subtitles]bool, len(clips))
	for _, clip := range clips {
		if clip.Source == nil {
			return nil, errors.New("astisub: assembling clips: nil source")
		}
		if clip.In < 0 {
			return nil, fmt.Errorf("astisub: assembling clips: negative in %s", clip.In)
		}
		if clip.Out <= clip.In {
			return nil, fmt.Errorf("astisub: assembling clips: out %s is not greater than in %s", clip.Out, clip.In)
		}
		if clip.Duration <= 0 {
			return nil, fmt.Errorf("astisub: assembling clips: non-positive duration %s", clip.Duration)
		}
		if clip.Duration > maxAssembledDuration-total {
			return nil, errors.New("astisub: assembling clips: total duration overflow")
		}
		total += clip.Duration
		if !seen[clip.Source] {
			seen[clip.Source] = true
			sources = append(sources, clip.Source)
		}
	}

	// Validate every event of every source that participates in the assembly.
	// This also rejects invalid word markers before anything is produced.
	for _, source := range sources {
		for _, item := range source.Items {
			if item == nil {
				continue
			}
			if item.EndAt < item.StartAt {
				return nil, fmt.Errorf("astisub: assembling clips: event ends at %s before it starts at %s", item.EndAt, item.StartAt)
			}
			if err := validateAssembledWordStarts(item); err != nil {
				return nil, err
			}
		}
	}

	// Give each distinct source its own namespace so that identically named
	// styles and regions (for instance "default") do not overwrite each other.
	out := NewSubtitles()
	namespaces := make(map[*Subtitles]*assemblyNamespace, len(sources))
	for idx, source := range sources {
		namespaces[source] = newAssemblyNamespace(fmt.Sprintf("s%d_", idx+1), source, out)
	}

	// Assemble the clips nose to tail.
	var clipOffset time.Duration
	for _, clip := range clips {
		ns := namespaces[clip.Source]
		sourceSpan := clip.Out - clip.In
		for _, item := range clip.Source.Items {
			if item == nil {
				continue
			}

			// Intersect the event with the clip's source range.
			intersectionStart := item.StartAt
			if intersectionStart < clip.In {
				intersectionStart = clip.In
			}
			intersectionEnd := item.EndAt
			if intersectionEnd > clip.Out {
				intersectionEnd = clip.Out
			}
			if intersectionStart >= intersectionEnd {
				continue
			}

			// Map both original endpoints independently: multiplying the source
			// offset by Duration and dividing by Out-In, flooring at the
			// nanosecond. Never accumulate previously rounded lengths.
			outStart := clipOffset + scaleAssembledTime(intersectionStart-clip.In, sourceSpan, clip.Duration)
			outEnd := clipOffset + scaleAssembledTime(intersectionEnd-clip.In, sourceSpan, clip.Duration)
			if outEnd <= outStart {
				continue
			}

			// Only an empty intersection or a zero-length quantized event is
			// dropped; events are otherwise kept, including overlapping ones.
			out.Items = append(out.Items, ns.cloneItem(item, clip, intersectionStart, intersectionEnd, clipOffset, outStart, outEnd))
		}
		clipOffset += clip.Duration
	}

	// Overlapping subtitles are kept. Stable ordering preserves clip order and,
	// inside a clip, the source's original item order for identical starts.
	sort.SliceStable(out.Items, func(i, j int) bool {
		return out.Items[i].StartAt < out.Items[j].StartAt
	})
	for idx, item := range out.Items {
		item.Index = idx + 1
	}
	return out, nil
}

// scaleAssembledTime maps an offset inside a clip's source range onto the
// clip's output length, flooring at the nanosecond.
func scaleAssembledTime(offset, sourceSpan, duration time.Duration) time.Duration {
	if offset <= 0 {
		return 0
	}
	if offset > sourceSpan {
		offset = sourceSpan
	}
	n := new(big.Int).Mul(big.NewInt(int64(offset)), big.NewInt(int64(duration)))
	n.Quo(n, big.NewInt(int64(sourceSpan)))
	return time.Duration(n.Int64())
}

// validateAssembledWordStarts verifies that non-zero word group starts stay
// inside their event and never move backwards. Word groups are per line, so
// each line is checked independently.
func validateAssembledWordStarts(item *Item) error {
	for _, line := range item.Lines {
		var previous time.Duration
		var hasPrevious bool
		for _, lineItem := range line.Items {
			if lineItem.StartAt == 0 {
				continue
			}
			if lineItem.StartAt < item.StartAt || lineItem.StartAt > item.EndAt {
				return fmt.Errorf("astisub: assembling clips: word start %s is outside event [%s,%s]", lineItem.StartAt, item.StartAt, item.EndAt)
			}
			if hasPrevious && lineItem.StartAt < previous {
				return fmt.Errorf("astisub: assembling clips: word start %s is before previous word start %s", lineItem.StartAt, previous)
			}
			previous = lineItem.StartAt
			hasPrevious = true
		}
	}
	return nil
}

// assemblyNamespace holds the renamed styles and regions of a single source.
// The same namespace is reused every time that source is clipped again.
type assemblyNamespace struct {
	prefix       string
	out          *Subtitles
	stylesByPtr  map[*Style]*Style
	stylesByID   map[string]*Style
	regionsByPtr map[*Region]*Region
	regionsByID  map[string]*Region
}

func newAssemblyNamespace(prefix string, source *Subtitles, out *Subtitles) *assemblyNamespace {
	ns := &assemblyNamespace{
		prefix:       prefix,
		out:          out,
		stylesByPtr:  make(map[*Style]*Style),
		stylesByID:   make(map[string]*Style),
		regionsByPtr: make(map[*Region]*Region),
		regionsByID:  make(map[string]*Region),
	}

	// Clone every named style up front so that the output keeps the source's
	// complete style table, including parents that are not directly used.
	styleIDs := make([]string, 0, len(source.Styles))
	for id := range source.Styles {
		styleIDs = append(styleIDs, id)
	}
	sort.Strings(styleIDs)
	for _, id := range styleIDs {
		style := source.Styles[id]
		if style == nil {
			continue
		}
		clone := ns.style(style)
		if id != "" {
			if _, ok := ns.stylesByID[id]; !ok {
				ns.stylesByID[id] = clone
			}
		}
	}

	// Clone regions. Their Style references are resolved through ns.style, so
	// region -> style and style -> parent chains keep pointing at the renamed
	// objects.
	regionIDs := make([]string, 0, len(source.Regions))
	for id := range source.Regions {
		regionIDs = append(regionIDs, id)
	}
	sort.Strings(regionIDs)
	for _, id := range regionIDs {
		region := source.Regions[id]
		if region == nil {
			continue
		}
		clone := ns.region(region)
		if id != "" {
			if _, ok := ns.regionsByID[id]; !ok {
				ns.regionsByID[id] = clone
			}
		}
	}
	return ns
}

// style returns the renamed clone of style, creating it (and its parent
// chain) on demand. Attribute objects are shared read-only.
func (ns *assemblyNamespace) style(style *Style) *Style {
	if style == nil {
		return nil
	}
	if clone, ok := ns.stylesByPtr[style]; ok {
		return clone
	}
	if style.ID != "" {
		if clone, ok := ns.stylesByID[style.ID]; ok {
			ns.stylesByPtr[style] = clone
			return clone
		}
	}

	id := style.ID
	if id != "" {
		id = ns.prefix + id
	}
	clone := &Style{
		ID:          id,
		InlineStyle: style.InlineStyle,
	}
	// Register before resolving the parent so that a cyclic chain cannot
	// recurse forever.
	ns.stylesByPtr[style] = clone
	if style.ID != "" {
		if _, ok := ns.stylesByID[style.ID]; !ok {
			ns.stylesByID[style.ID] = clone
		}
		if _, ok := ns.out.Styles[clone.ID]; !ok {
			ns.out.Styles[clone.ID] = clone
		}
	}
	clone.Style = ns.style(style.Style)
	return clone
}

// region returns the renamed clone of region, creating it on demand.
func (ns *assemblyNamespace) region(region *Region) *Region {
	if region == nil {
		return nil
	}
	if clone, ok := ns.regionsByPtr[region]; ok {
		return clone
	}
	if region.ID != "" {
		if clone, ok := ns.regionsByID[region.ID]; ok {
			ns.regionsByPtr[region] = clone
			return clone
		}
	}

	id := region.ID
	if id != "" {
		id = ns.prefix + id
	}
	inlineStyle := region.InlineStyle
	if inlineStyle == nil {
		inlineStyle = &StyleAttributes{}
	}
	clone := &Region{
		ID:          id,
		InlineStyle: inlineStyle,
	}
	ns.regionsByPtr[region] = clone
	if region.ID != "" {
		if _, ok := ns.regionsByID[region.ID]; !ok {
			ns.regionsByID[region.ID] = clone
		}
		if _, ok := ns.out.Regions[clone.ID]; !ok {
			ns.out.Regions[clone.ID] = clone
		}
	}
	clone.Style = ns.style(region.Style)
	return clone
}

// cloneItem builds an independent output event. Events, lines and word items
// never share their timing with the input or with other output events.
func (ns *assemblyNamespace) cloneItem(item *Item, clip EditClip, intersectionStart, intersectionEnd, clipOffset, outStart, outEnd time.Duration) *Item {
	clone := &Item{
		Comments:    append([]string(nil), item.Comments...),
		EndAt:       outEnd,
		InlineStyle: item.InlineStyle,
		Region:      ns.region(item.Region),
		StartAt:     outStart,
		Style:       ns.style(item.Style),
	}
	for _, line := range item.Lines {
		lineClone := Line{VoiceName: line.VoiceName}
		lineClone.Items = ns.cloneLineItems(line.Items, item, clip, intersectionStart, intersectionEnd, clipOffset, outStart)
		if len(lineClone.Items) > 0 {
			clone.Lines = append(clone.Lines, lineClone)
		}
	}
	return clone
}

// cloneLineItems splits a line into word groups and keeps the groups that
// intersect the clipped event. Group text, tags and order are untouched; only
// the start marker of the first item in a group is moved to the output
// timeline (or zeroed when it lands exactly on the new event start).
func (ns *assemblyNamespace) cloneLineItems(items []LineItem, item *Item, clip EditClip, intersectionStart, intersectionEnd, clipOffset, outStart time.Duration) []LineItem {
	groups := buildWordGroups(items, item.StartAt, item.EndAt)
	var out []LineItem
	for _, group := range groups {
		// Half-open intersection between the group and the clipped event.
		if group.start >= intersectionEnd || group.end <= intersectionStart {
			continue
		}

		start := group.start
		if start < intersectionStart {
			start = intersectionStart
		}
		mapped := clipOffset + scaleAssembledTime(start-clip.In, clip.Out-clip.In, clip.Duration)
		for idx := range group.items {
			lineItem := group.items[idx]
			lineItem.Style = ns.style(lineItem.Style)
			if idx == 0 {
				if mapped <= outStart {
					// A marker exactly on the new event start is written as a
					// zero-valued continuation marker.
					lineItem.StartAt = 0
				} else {
					lineItem.StartAt = mapped
				}
			} else {
				// Continuation fragments always stay zero.
				lineItem.StartAt = 0
			}
			out = append(out, lineItem)
		}
	}
	return out
}

// wordGroup is a run of line items sharing one word marker. A non-zero
// StartAt begins a new group; a zero StartAt continues the previous one.
type wordGroup struct {
	start time.Duration
	end   time.Duration
	items []LineItem
}

func buildWordGroups(items []LineItem, itemStart, itemEnd time.Duration) []wordGroup {
	var groups []wordGroup
	for idx, lineItem := range items {
		if idx == 0 || lineItem.StartAt != 0 {
			start := lineItem.StartAt
			if idx == 0 && start == 0 {
				start = itemStart
			}
			groups = append(groups, wordGroup{start: start})
		}
		groups[len(groups)-1].items = append(groups[len(groups)-1].items, lineItem)
	}
	for idx := range groups {
		if idx+1 < len(groups) {
			groups[idx].end = groups[idx+1].start
		} else {
			groups[idx].end = itemEnd
		}
	}
	return groups
}
