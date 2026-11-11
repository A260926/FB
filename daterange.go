package m3u8

/*
 Part of M3U8 parser & generator library.
 This file defines functions related to EXT-X-DATERANGE tags.

 Copyright 2013-2019 The Project Developers.
 See the AUTHORS and LICENSE files at the top-level directory of this distribution
 and at https://github.com/grafov/m3u8/

 ॐ तारे तुत्तारे तुरे स्व
*/

import (
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"
	"time"
)

// DateRangeError declares an error of a date range update or parsing.
// It carries the ID of the date range and the name of the attribute
// that caused the failure.
type DateRangeError struct {
	ID        string
	Attribute string
	Reason    string
}

func (e *DateRangeError) Error() string {
	return fmt.Sprintf("daterange %q: attribute %s: %s", e.ID, e.Attribute, e.Reason)
}

// parseDateRange parses the attribute list of an EXT-X-DATERANGE tag
// (the part after "#EXT-X-DATERANGE:") into a DateRange structure.
func parseDateRange(line string) (*DateRange, error) {
	dr := new(DateRange)
	for _, part := range splitAttributeList(line) {
		kv := strings.SplitN(part, "=", 2)
		if len(kv) != 2 {
			return nil, errors.New("daterange: bad attribute pair: " + part)
		}
		name := strings.TrimSpace(kv[0])
		value, quoted := unquoteValue(strings.TrimSpace(kv[1]))
		switch name {
		case "ID":
			dr.ID = value
		case "CLASS":
			dr.Class = value
		case "START-DATE":
			t, err := TimeParse(value)
			if err != nil {
				return nil, &DateRangeError{dr.ID, "START-DATE", err.Error()}
			}
			dr.StartDate = t
		case "END-DATE":
			t, err := TimeParse(value)
			if err != nil {
				return nil, &DateRangeError{dr.ID, "END-DATE", err.Error()}
			}
			dr.EndDate = t
			dr.EndDateSet = true
		case "DURATION":
			f, err := strconv.ParseFloat(value, 64)
			if err != nil {
				return nil, &DateRangeError{dr.ID, "DURATION", err.Error()}
			}
			dr.Duration = f
			dr.DurationSet = true
		case "PLANNED-DURATION":
			f, err := strconv.ParseFloat(value, 64)
			if err != nil {
				return nil, &DateRangeError{dr.ID, "PLANNED-DURATION", err.Error()}
			}
			dr.PlannedDuration = f
			dr.PlannedDurationSet = true
		case "END-ON-NEXT":
			switch strings.ToUpper(value) {
			case "YES":
				dr.EndOnNext = true
			case "NO":
				dr.EndOnNext = false
			default:
				return nil, &DateRangeError{dr.ID, "END-ON-NEXT", "value must be YES or NO"}
			}
		default:
			if strings.HasPrefix(name, "X-") {
				attr := DateRangeAttribute{Name: name, Value: value, Type: DateRangeAttributeNumber}
				switch {
				case quoted:
					attr.Type = DateRangeAttributeString
				case isHexSequence(value):
					attr.Type = DateRangeAttributeHex
				default:
					if _, err := strconv.ParseFloat(value, 64); err != nil {
						return nil, &DateRangeError{dr.ID, name, "client attribute must be a quoted string, a number or a hexadecimal sequence"}
					}
				}
				dr.X = append(dr.X, attr)
			}
			// unknown non client-defined attributes are ignored
		}
	}
	return dr, nil
}

// splitAttributeList splits an attribute list on commas that appear
// outside of quoted strings.
func splitAttributeList(line string) []string {
	var parts []string
	var cur strings.Builder
	inQuotes := false
	for _, r := range line {
		switch {
		case r == '"':
			inQuotes = !inQuotes
			cur.WriteRune(r)
		case r == ',' && !inQuotes:
			parts = append(parts, cur.String())
			cur.Reset()
		default:
			cur.WriteRune(r)
		}
	}
	parts = append(parts, cur.String())
	return parts
}

// unquoteValue strips surrounding quotes of an attribute value and
// reports whether the value was quoted.
func unquoteValue(value string) (string, bool) {
	if len(value) >= 2 && value[0] == '"' && value[len(value)-1] == '"' {
		return value[1 : len(value)-1], true
	}
	return value, false
}

// isHexSequence reports whether value looks like a hexadecimal-sequence
// as defined by the HLS specification (0x followed by hex digits).
func isHexSequence(value string) bool {
	if len(value) < 3 || value[0] != '0' || (value[1] != 'x' && value[1] != 'X') {
		return false
	}
	for _, r := range value[2:] {
		if !('0' <= r && r <= '9') && !('a' <= r && r <= 'f') && !('A' <= r && r <= 'F') {
			return false
		}
	}
	return true
}

// durationToTime converts seconds to time.Duration with nanosecond
// precision.
func durationToTime(seconds float64) time.Duration {
	return time.Duration(math.Round(seconds * float64(time.Second)))
}

// merge applies the attributes of the update to dr. Attributes that
// are already set must carry equal values (times are compared as
// instants, numbers numerically); attributes not set yet are added.
func (dr *DateRange) merge(u *DateRange) error {
	conflict := func(attr string) error {
		return &DateRangeError{dr.ID, attr, "conflicting value"}
	}
	if dr.ID == "" {
		dr.ID = u.ID
	}
	if u.Class != "" {
		if dr.Class != "" && dr.Class != u.Class {
			return conflict("CLASS")
		}
		dr.Class = u.Class
	}
	if !u.StartDate.IsZero() {
		if !dr.StartDate.IsZero() && !dr.StartDate.Equal(u.StartDate) {
			return conflict("START-DATE")
		}
		dr.StartDate = u.StartDate
	}
	if u.EndDateSet {
		if dr.EndDateSet && !dr.EndDate.Equal(u.EndDate) {
			return conflict("END-DATE")
		}
		dr.EndDate = u.EndDate
		dr.EndDateSet = true
	}
	if u.DurationSet {
		if dr.DurationSet && dr.Duration != u.Duration {
			return conflict("DURATION")
		}
		dr.Duration = u.Duration
		dr.DurationSet = true
	}
	if u.PlannedDurationSet {
		if dr.PlannedDurationSet && dr.PlannedDuration != u.PlannedDuration {
			return conflict("PLANNED-DURATION")
		}
		dr.PlannedDuration = u.PlannedDuration
		dr.PlannedDurationSet = true
	}
	if u.EndOnNext {
		dr.EndOnNext = true
	}
	for _, xa := range u.X {
		found := false
		for i := range dr.X {
			if dr.X[i].Name == xa.Name {
				found = true
				if !dateRangeAttributeEqual(dr.X[i], xa) {
					return conflict(xa.Name)
				}
			}
		}
		if !found {
			dr.X = append(dr.X, xa)
		}
	}
	return nil
}

// dateRangeAttributeEqual compares two client-defined attributes.
// Numbers are compared numerically, hexadecimal sequences case
// insensitively, strings literally.
func dateRangeAttributeEqual(a, b DateRangeAttribute) bool {
	if a.Type == DateRangeAttributeNumber && b.Type == DateRangeAttributeNumber {
		fa, ea := strconv.ParseFloat(a.Value, 64)
		fb, eb := strconv.ParseFloat(b.Value, 64)
		if ea == nil && eb == nil {
			return fa == fb
		}
	}
	if a.Type == DateRangeAttributeHex && b.Type == DateRangeAttributeHex {
		return strings.EqualFold(a.Value, b.Value)
	}
	return a.Type == b.Type && a.Value == b.Value
}

// validate checks the consistency rules of a merged date range.
func (dr *DateRange) validate() error {
	if dr.ID == "" {
		return &DateRangeError{dr.ID, "ID", "must not be empty"}
	}
	if dr.StartDate.IsZero() {
		return &DateRangeError{dr.ID, "START-DATE", "must be present on first occurrence of the ID"}
	}
	if dr.EndOnNext {
		if dr.Class == "" {
			return &DateRangeError{dr.ID, "END-ON-NEXT", "requires a non-empty CLASS"}
		}
		if dr.EndDateSet {
			return &DateRangeError{dr.ID, "END-ON-NEXT", "must not be combined with END-DATE"}
		}
		if dr.DurationSet {
			return &DateRangeError{dr.ID, "END-ON-NEXT", "must not be combined with DURATION"}
		}
	}
	if dr.DurationSet && dr.Duration < 0 {
		return &DateRangeError{dr.ID, "DURATION", "must not be negative"}
	}
	if dr.PlannedDurationSet && dr.PlannedDuration < 0 {
		return &DateRangeError{dr.ID, "PLANNED-DURATION", "must not be negative"}
	}
	if dr.EndDateSet && dr.EndDate.Before(dr.StartDate) {
		return &DateRangeError{dr.ID, "END-DATE", "must not be earlier than START-DATE"}
	}
	if dr.EndDateSet && dr.DurationSet &&
		!dr.StartDate.Add(durationToTime(dr.Duration)).Equal(dr.EndDate) {
		return &DateRangeError{dr.ID, "DURATION", "inconsistent with START-DATE and END-DATE"}
	}
	return nil
}

// SetDateRange adds or updates a date range of the media playlist.
// Date ranges are merged by ID: an update may add attributes not seen
// before, attributes already known must carry equal values. On a
// conflict an error (of type *DateRangeError) is returned and the
// stored range is left untouched. This operation resets the playlist
// cache, so a following Encode() reflects the update.
func (p *MediaPlaylist) SetDateRange(update *DateRange) error {
	if update == nil {
		return &DateRangeError{"", "ID", "update must not be nil"}
	}
	existing := p.dateRangeIdx[update.ID]
	merged := new(DateRange)
	if existing != nil {
		*merged = *existing
		merged.X = append([]DateRangeAttribute(nil), existing.X...)
	}
	if err := merged.merge(update); err != nil {
		return err
	}
	if err := merged.validate(); err != nil {
		return err
	}
	if existing == nil {
		if p.dateRangeIdx == nil {
			p.dateRangeIdx = make(map[string]*DateRange)
		}
		p.dateRanges = append(p.dateRanges, merged)
		p.dateRangeIdx[merged.ID] = merged
	} else {
		*existing = *merged
	}
	p.buf.Reset()
	return nil
}

// DateRange returns the effective attributes of the date range with
// the given ID. The second return value reports whether such a range
// exists. The returned structure is a copy: EndDate (with EndDateSet)
// reflects the known end of the range, derived from DURATION or from
// the next range of the same CLASS for END-ON-NEXT ranges, while the
// attributes stored in the playlist are left as they were read.
func (p *MediaPlaylist) DateRange(id string) (*DateRange, bool) {
	dr := p.dateRangeIdx[id]
	if dr == nil {
		return nil, false
	}
	return p.effectiveDateRange(dr), true
}

// DateRanges returns the effective attributes of all date ranges of
// the playlist, in the order their IDs first appeared.
func (p *MediaPlaylist) DateRanges() []*DateRange {
	out := make([]*DateRange, 0, len(p.dateRanges))
	for _, dr := range p.dateRanges {
		out = append(out, p.effectiveDateRange(dr))
	}
	return out
}

// effectiveDateRange returns a copy of the stored range with the known
// end of the range filled into EndDate. The stored (raw) attributes
// are never modified, so a derived END-DATE is not written back on
// encode.
func (p *MediaPlaylist) effectiveDateRange(dr *DateRange) *DateRange {
	out := new(DateRange)
	*out = *dr
	out.X = append([]DateRangeAttribute(nil), dr.X...)
	if end, ok := p.dateRangeEnd(dr); ok {
		out.EndDate = end
		out.EndDateSet = true
	}
	return out
}

// dateRangeEnd computes the known end of a date range.
func (p *MediaPlaylist) dateRangeEnd(dr *DateRange) (time.Time, bool) {
	switch {
	case dr.EndDateSet:
		return dr.EndDate, true
	case dr.DurationSet:
		return dr.StartDate.Add(durationToTime(dr.Duration)), true
	case dr.EndOnNext:
		return p.derivedDateRangeEnd(dr)
	}
	return time.Time{}, false
}

// derivedDateRangeEnd finds the end of an END-ON-NEXT range: the start
// of the nearest range of the same CLASS whose START-DATE is strictly
// later. Ranges of other classes and ranges with the same start do not
// truncate it. The end is unknown until such a successor appears.
func (p *MediaPlaylist) derivedDateRangeEnd(dr *DateRange) (time.Time, bool) {
	var best time.Time
	found := false
	for _, other := range p.dateRanges {
		if other == dr || other.Class != dr.Class {
			continue
		}
		if other.StartDate.After(dr.StartDate) {
			if !found || other.StartDate.Before(best) {
				best = other.StartDate
				found = true
			}
		}
	}
	return best, found
}

// encode serializes the stored (raw) attributes of the date range into
// an EXT-X-DATERANGE tag.
func (dr *DateRange) encode() string {
	var b strings.Builder
	b.WriteString("#EXT-X-DATERANGE:ID=\"")
	b.WriteString(dr.ID)
	b.WriteRune('"')
	if dr.Class != "" {
		b.WriteString(",CLASS=\"")
		b.WriteString(dr.Class)
		b.WriteRune('"')
	}
	if !dr.StartDate.IsZero() {
		b.WriteString(",START-DATE=\"")
		b.WriteString(dr.StartDate.Format(DATETIME))
		b.WriteRune('"')
	}
	if dr.EndDateSet {
		b.WriteString(",END-DATE=\"")
		b.WriteString(dr.EndDate.Format(DATETIME))
		b.WriteRune('"')
	}
	if dr.DurationSet {
		b.WriteString(",DURATION=")
		b.WriteString(strconv.FormatFloat(dr.Duration, 'f', -1, 64))
	}
	if dr.PlannedDurationSet {
		b.WriteString(",PLANNED-DURATION=")
		b.WriteString(strconv.FormatFloat(dr.PlannedDuration, 'f', -1, 64))
	}
	if dr.EndOnNext {
		b.WriteString(",END-ON-NEXT=YES")
	}
	for _, xa := range dr.X {
		b.WriteRune(',')
		b.WriteString(xa.Name)
		b.WriteRune('=')
		if xa.Type == DateRangeAttributeString {
			b.WriteRune('"')
			b.WriteString(xa.Value)
			b.WriteRune('"')
		} else {
			b.WriteString(xa.Value)
		}
	}
	return b.String()
}
