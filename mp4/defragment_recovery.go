package mp4

import "fmt"

// recoveryGroupKinds lists the recovery sample grouping types that
// Defragment carries from the fragmented input into the progressive stbl.
// Each type has its own description space, so both may occur in one track
// and one traf.
var recoveryGroupKinds = []string{"roll", "prol"}

func isRecoveryGroupingType(kind string) bool {
	for _, supported := range recoveryGroupKinds {
		if kind == supported {
			return true
		}
	}
	return false
}

// recoveryGroup is one output sample's membership in a recovery sample group.
// hasGroup is false for sbgp group_description_index 0 (no group), which is
// distinct from a real description carrying recovery distance 0.
type recoveryGroup struct {
	kind     string
	hasGroup bool
	distance int16
}

// recoveryScope says where validated sample-group boxes were found, so that
// every error names the track and either its stbl or the fragment position.
type recoveryScope struct {
	trackID uint32
	inTraf  bool
	moofPos uint64 // valid when inTraf
}

func (s recoveryScope) location() string {
	if s.inTraf {
		return fmt.Sprintf("traf of track %d in moof at %d", s.trackID, s.moofPos)
	}
	return fmt.Sprintf("stbl of track %d", s.trackID)
}

// collectProgressiveRecovery validates the recovery sgpd/sbgp boxes of the
// moov sample tables and expands them over the progressive prefix samples.
// The track-level descriptions are kept for fragment sbgp boxes that refer to
// them. Dropped tracks are neither validated nor converted.
func (d *defragmenter) collectProgressiveRecovery() error {
	for _, track := range d.tracks {
		if !track.keep {
			continue
		}
		stbl := trackStbl(track.trak)
		if stbl == nil {
			continue
		}
		scope := recoveryScope{trackID: track.trak.Tkhd.TrackID}
		descriptions, err := recoverySgpds(stbl.Children, scope)
		if err != nil {
			return err
		}
		track.globalRecovery = descriptions
		nrSamples := 0
		if stbl.Stsz != nil {
			nrSamples = int(stbl.Stsz.GetNrSamples())
		}
		groups, err := recoveryGroupsOfScope(stbl.Children, descriptions, nrSamples, scope)
		if err != nil {
			return err
		}
		track.appendRecoveryGroups(groups, nrSamples)
	}
	return nil
}

// collectTrafRecovery validates the recovery sgpd/sbgp boxes of one traf and
// expands them over its samples, counting sbgp samples continuously across
// all truns of the traf in decode order. Track-level descriptions resolve
// indices 1..65535 while the traf's own sgpd resolves indices 65537 and above.
func (d *defragmenter) collectTrafRecovery(moof *MoofBox, traf *TrafBox, track *defragTrack, nrSamples int) error {
	scope := recoveryScope{trackID: track.trak.Tkhd.TrackID, inTraf: true, moofPos: moof.StartPos}
	groups, err := recoveryGroupsOfScope(traf.Children, track.globalRecovery, nrSamples, scope)
	if err != nil {
		return err
	}
	track.appendRecoveryGroups(groups, nrSamples)
	return nil
}

// appendRecoveryGroups adds one recovery entry per sample of a scope for each
// recovery grouping type. A type without an sbgp in this scope contributes
// no-group entries, so every type always covers every collected sample and the
// synthesized sbgp run counts add up to the output sample count.
func (t *defragTrack) appendRecoveryGroups(groups map[string][]recoveryGroup, nrSamples int) {
	for _, kind := range recoveryGroupKinds {
		scoped, present := groups[kind]
		if !present {
			scoped = make([]recoveryGroup, nrSamples)
			for i := range scoped {
				scoped[i] = recoveryGroup{kind: kind}
			}
		}
		t.recovery = append(t.recovery, scoped...)
	}
}

// recoverySgpds validates every recovery sgpd box in children and returns the
// signed 16-bit descriptions per grouping type. Two sgpd boxes of the same
// grouping type in one scope are an error.
func recoverySgpds(children []Box, scope recoveryScope) (map[string][]int16, error) {
	descriptions := make(map[string][]int16)
	for _, child := range children {
		box, ok := child.(*SgpdBox)
		if !ok || !isRecoveryGroupingType(box.GroupingType) {
			continue
		}
		if _, exists := descriptions[box.GroupingType]; exists {
			return nil, fmt.Errorf("%s: duplicate sgpd box with grouping_type %q",
				scope.location(), box.GroupingType)
		}
		descs, err := validateRecoverySgpd(box, scope)
		if err != nil {
			return nil, err
		}
		descriptions[box.GroupingType] = descs
	}
	return descriptions, nil
}

// validateRecoverySgpd checks a recovery sgpd box and returns its descriptions
// in 1-based order. Version 1 and version 2 with
// default_group_description_index 0 are accepted; every description must be a
// signed big-endian 16-bit recovery distance.
func validateRecoverySgpd(box *SgpdBox, scope recoveryScope) ([]int16, error) {
	location := scope.location()
	switch box.Version {
	case 1:
	case 2:
		if box.DefaultGroupDescriptionIndex != 0 {
			return nil, fmt.Errorf("%s: sgpd box with grouping_type %q has default_group_description_index %d, only 0 is supported",
				location, box.GroupingType, box.DefaultGroupDescriptionIndex)
		}
	default:
		return nil, fmt.Errorf("%s: sgpd box with grouping_type %q has unsupported version %d, only 1 and 2 are supported",
			location, box.GroupingType, box.Version)
	}
	if box.DefaultLength != 2 {
		return nil, fmt.Errorf("%s: sgpd box with grouping_type %q has default_length %d, recovery descriptions must be 2 bytes",
			location, box.GroupingType, box.DefaultLength)
	}
	descs := make([]int16, 0, len(box.SampleGroupEntries))
	for i, entry := range box.SampleGroupEntries {
		distance, ok := recoveryDistance(entry)
		if !ok {
			return nil, fmt.Errorf("%s: description %d of sgpd box with grouping_type %q is not a 2-byte recovery distance",
				location, i+1, box.GroupingType)
		}
		descs = append(descs, distance)
	}
	return descs, nil
}

// recoveryDistance reads a signed big-endian 16-bit recovery distance from a
// sample group entry. The typed roll entry and a two-byte unknown entry, as
// used for prol, both carry exactly one signed 16-bit value.
func recoveryDistance(entry SampleGroupEntry) (int16, bool) {
	switch e := entry.(type) {
	case *RollSampleGroupEntry:
		return e.RollDistance, true
	case *UnknownSampleGroupEntry:
		if len(e.Data) != 2 {
			return 0, false
		}
		return int16(uint16(e.Data[0])<<8 | uint16(e.Data[1])), true
	default:
		return 0, false
	}
}

// recoveryGroupsOfScope validates the recovery boxes of one scope (a stbl or a
// traf) and returns the per-sample groups. globalDescriptions are the stbl
// sgpd descriptions; a traf additionally uses its own sgpd boxes. A missing or
// short sbgp leaves the uncovered samples without a group.
func recoveryGroupsOfScope(children []Box, globalDescriptions map[string][]int16,
	nrSamples int, scope recoveryScope) (map[string][]recoveryGroup, error) {
	localDescriptions, err := recoverySgpds(children, scope)
	if err != nil {
		return nil, err
	}
	result := make(map[string][]recoveryGroup)
	seen := make(map[string]bool)
	for _, child := range children {
		box, ok := child.(*SbgpBox)
		if !ok || !isRecoveryGroupingType(box.GroupingType) {
			continue
		}
		if seen[box.GroupingType] {
			return nil, fmt.Errorf("%s: duplicate sbgp box with grouping_type %q",
				scope.location(), box.GroupingType)
		}
		seen[box.GroupingType] = true
		groups, err := validateRecoverySbgp(box, globalDescriptions[box.GroupingType],
			localDescriptions[box.GroupingType], nrSamples, scope)
		if err != nil {
			return nil, err
		}
		result[box.GroupingType] = groups
	}
	return result, nil
}

// validateRecoverySbgp checks a recovery sbgp box and expands its runs into one
// recoveryGroup per sample of the scope. Indices 1..65535 resolve against
// globalDescriptions and 65537+ against localDescriptions (traf only); index 0
// means no group and 65536 is reserved.
func validateRecoverySbgp(box *SbgpBox, globalDescriptions, localDescriptions []int16,
	nrSamples int, scope recoveryScope) ([]recoveryGroup, error) {
	location := scope.location()
	switch box.Version {
	case 0:
	case 1:
		if box.GroupingTypeParameter != 0 {
			return nil, fmt.Errorf("%s: sbgp box with grouping_type %q has grouping_type_parameter %d, only 0 is supported",
				location, box.GroupingType, box.GroupingTypeParameter)
		}
	default:
		return nil, fmt.Errorf("%s: sbgp box with grouping_type %q has unsupported version %d, only 0 and 1 are supported",
			location, box.GroupingType, box.Version)
	}
	if len(box.SampleCounts) != len(box.GroupDescriptionIndices) {
		return nil, fmt.Errorf("%s: sbgp box with grouping_type %q has %d runs but %d group description indices",
			location, box.GroupingType, len(box.SampleCounts), len(box.GroupDescriptionIndices))
	}
	groups := make([]recoveryGroup, 0, nrSamples)
	covered := 0
	for run, count := range box.SampleCounts {
		if count == 0 {
			return nil, fmt.Errorf("%s: run %d of sbgp box with grouping_type %q has zero sample count",
				location, run+1, box.GroupingType)
		}
		if covered+int(count) > nrSamples {
			return nil, fmt.Errorf("%s: sbgp box with grouping_type %q counts %d samples but its scope has %d",
				location, box.GroupingType, covered+int(count), nrSamples)
		}
		group := recoveryGroup{kind: box.GroupingType}
		index := box.GroupDescriptionIndices[run]
		switch {
		case index == 0:
			// No group; stays the zero-value recoveryGroup.
		case index == sbgpInsideOffset:
			return nil, fmt.Errorf("%s: sbgp box with grouping_type %q uses reserved group_description_index 65536",
				location, box.GroupingType)
		case index < sbgpInsideOffset:
			if int(index) > len(globalDescriptions) {
				return nil, fmt.Errorf("%s: sbgp box with grouping_type %q refers to missing description %d, the stbl sgpd has %d",
					location, box.GroupingType, index, len(globalDescriptions))
			}
			group.hasGroup = true
			group.distance = globalDescriptions[index-1]
		default:
			if !scope.inTraf {
				return nil, fmt.Errorf("%s: sbgp box with grouping_type %q uses fragment-local index %d in a stbl, where only 1..65535 are valid",
					location, box.GroupingType, index)
			}
			localIndex := index - sbgpInsideOffset - 1
			if int(localIndex) >= len(localDescriptions) {
				return nil, fmt.Errorf("%s: sbgp box with grouping_type %q refers to missing fragment description %d, the traf sgpd has %d",
					location, box.GroupingType, index, len(localDescriptions))
			}
			group.hasGroup = true
			group.distance = localDescriptions[localIndex]
		}
		for range count {
			groups = append(groups, group)
		}
		covered += int(count)
	}
	// Samples not covered by any run have no group.
	for len(groups) < nrSamples {
		groups = append(groups, recoveryGroup{kind: box.GroupingType})
	}
	return groups, nil
}

// buildRecoveryGroupBoxes synthesizes the stbl sgpd/sbgp pair of every
// recovery grouping type with at least one actual member. Descriptions are
// numbered from 1 in first-reference order of the final decode sequence; equal
// distances, whatever their source scope, share one description. The sbgp
// runs merge consecutive equal indices, including runs without a group, and
// the run counts add up to the number of output samples. A type without any
// member produces no boxes and no unreferenced descriptions.
func buildRecoveryGroupBoxes(groups []recoveryGroup) []Box {
	var boxes []Box
	for _, kind := range recoveryGroupKinds {
		var dictionary []int16
		indexOf := make(map[int16]uint32)
		indices := make([]uint32, 0, len(groups))
		members := 0
		for _, group := range groups {
			if group.kind != kind {
				continue
			}
			if !group.hasGroup {
				indices = append(indices, 0)
				continue
			}
			members++
			index, ok := indexOf[group.distance]
			if !ok {
				index = uint32(len(dictionary) + 1)
				indexOf[group.distance] = index
				dictionary = append(dictionary, group.distance)
			}
			indices = append(indices, index)
		}
		if members == 0 {
			continue // no box pair, and no unreferenced descriptions
		}
		entries := make([]SampleGroupEntry, 0, len(dictionary))
		for _, distance := range dictionary {
			entries = append(entries, newRecoveryEntry(kind, distance))
		}
		sgpd := &SgpdBox{
			Version:            1,
			GroupingType:       kind,
			DefaultLength:      2,
			SampleGroupEntries: entries,
		}
		var counts, runIndices []uint32
		for _, index := range indices {
			if n := len(counts); n > 0 && runIndices[n-1] == index {
				counts[n-1]++
				continue
			}
			counts = append(counts, 1)
			runIndices = append(runIndices, index)
		}
		sbgp := &SbgpBox{
			Version:                 0,
			GroupingType:            kind,
			SampleCounts:            counts,
			GroupDescriptionIndices: runIndices,
		}
		boxes = append(boxes, sgpd, sbgp)
	}
	return boxes
}

// newRecoveryEntry creates the two-byte sample group entry of a recovery
// grouping type, using the typed roll entry for roll and an opaque two-byte
// unknown entry, the on-wire form of prol, otherwise.
func newRecoveryEntry(kind string, distance int16) SampleGroupEntry {
	if kind == "roll" {
		return &RollSampleGroupEntry{RollDistance: distance}
	}
	return &UnknownSampleGroupEntry{
		Name: kind,
		Data: []byte{byte(uint16(distance) >> 8), byte(distance)},
	}
}
