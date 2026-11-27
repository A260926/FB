// Copyright 2019 The ebml-go authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package mkvcore

import (
	"github.com/at-wat/ebml-go"
)

type simpleBlockGroup struct {
	Block             []ebml.Block `ebml:"Block"`
	ReferencePriority uint64       `ebml:"ReferencePriority"`
}

type simpleBlockCluster struct {
	Timecode    uint64             `ebml:"Timecode"`
	PrevSize    uint64             `ebml:"PrevSize,omitempty"`
	SimpleBlock []ebml.Block       `ebml:"SimpleBlock"`
	BlockGroup  []simpleBlockGroup `ebml:"BlockGroup,omitempty"`
}

type cueTrackPosition struct {
	CueTrack           uint64 `ebml:"CueTrack"`
	CueClusterPosition uint64 `ebml:"CueClusterPosition"`
}

type cuePoint struct {
	CueTime           uint64             `ebml:"CueTime"`
	CueTrackPositions []cueTrackPosition `ebml:"CueTrackPositions"`
}

type cues struct {
	CuePoint []cuePoint `ebml:"CuePoint"`
}

type seekFixed struct {
	SeekID       []byte  `ebml:"SeekID"`
	SeekPosition *uint64 `ebml:"SeekPosition,size=8"`
}

type seekHeadFixed struct {
	Seek []seekFixed `ebml:"Seek"`
}

type flexTracks struct {
	TrackEntry []interface{} `ebml:"TrackEntry"`
}

type flexSegment struct {
	SeekHead *seekHeadFixed       `ebml:"SeekHead,omitempty"`
	Info     interface{}          `ebml:"Info"`
	Tracks   flexTracks           `ebml:"Tracks"`
	Cluster  []simpleBlockCluster `ebml:"Cluster,size=unknown"`
}

type flexHeader struct {
	Header  interface{} `ebml:"EBML"`
	Segment flexSegment `ebml:"Segment,size=unknown"`
}

// TrackEntry is a TrackEntry struct with all mandatory elements and commonly used elements.
type TrackEntry struct {
	TrackNumber        uint64
	TrackUID           uint64
	TrackType          uint8
	FlagEnabled        uint8
	FlagDefault        uint8
	FlagForced         uint8
	FlagLacing         uint8
	MinCache           uint64
	DefaultDuration    uint64
	MaxBlockAdditionID uint64
	Name               string
	Language           string
	LanguageIETF       string
	CodecID            string
	CodecDecodeAll     uint8
	SeekPreRoll        uint64
	// CodecPrivate is the raw CodecPrivate element as stored in the container.
	// When content decoding is enabled (see WithContentDecoding), the
	// TrackEntry returned by BlockReadCloserWithTrackEntry.TrackEntry carries
	// the restored CodecPrivate instead, while ContentEncodings keeps the
	// original source declaration. The restored data must not be passed
	// through the declared encoding chain another time.
	CodecPrivate []byte `ebml:"CodecPrivate,omitempty"`
	// ContentEncodings is the source ContentEncodings declaration of the
	// track. It is kept unchanged even when content decoding is enabled.
	ContentEncodings *ContentEncodings `ebml:"ContentEncodings,omitempty"`
}

// ContentEncodings represents the Matroska ContentEncodings element.
type ContentEncodings struct {
	ContentEncoding []ContentEncoding `ebml:"ContentEncoding"`
}

// ContentEncoding represents one Matroska ContentEncoding element.
// Pointer scalar fields distinguish an absent element (nil) from an explicit
// zero value, matching the Matroska defaults: ContentEncodingOrder defaults
// to 0, ContentEncodingScope to 1, and ContentEncodingType to 0.
type ContentEncoding struct {
	// Order is ContentEncodingOrder. nil means the default 0.
	Order *uint64 `ebml:"ContentEncodingOrder"`
	// Scope is ContentEncodingScope. nil means the default 1 (frame only).
	Scope *uint64 `ebml:"ContentEncodingScope"`
	// Type is ContentEncodingType. nil means the default 0 (compression).
	Type *uint64 `ebml:"ContentEncodingType"`
	// Compression is the ContentCompression child.
	Compression *ContentCompression `ebml:"ContentCompression,omitempty"`
	// Encryption is the ContentEncryption child, which is not supported.
	Encryption *ContentEncryption `ebml:"ContentEncryption,omitempty"`
}

// ContentCompression represents the Matroska ContentCompression element.
type ContentCompression struct {
	// Algo is ContentCompAlgo. nil means the default 0 (zlib).
	// 3 means header stripping; 1 (bzlib) and 2 (lzo) are not supported.
	Algo *uint64 `ebml:"ContentCompAlgo"`
	// Settings is ContentCompSettings, the common prefix prepended back by
	// the header-stripping (algo 3) decoding step.
	Settings []byte `ebml:"ContentCompSettings,omitempty"`
}

// ContentEncryption represents the Matroska ContentEncryption element.
// Content encryption is never decoded by this package; an encoding carrying
// it is rejected when content decoding is enabled.
type ContentEncryption struct {
	Algo *uint64 `ebml:"ContentEncAlgo"`
}
