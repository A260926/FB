// Copyright 2020-2021 The ebml-go authors.
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
	"io"

	"github.com/at-wat/ebml-go"
)

type blockReader struct {
	f          chan *frame
	closed     chan struct{}
	trackEntry TrackEntry

	// terminalErr is set by the reading goroutine before f is closed when
	// content decoding of a block fails. Reading the closed channel orders the
	// write before the receiving Read call.
	terminalErr error
	errReturned bool
}

func (r *blockReader) Read() (b []byte, keyframe bool, timestamp int64, err error) {
	frame, ok := <-r.f
	if !ok {
		if r.terminalErr != nil && !r.errReturned {
			r.errReturned = true
			return nil, false, 0, r.terminalErr
		}
		return nil, false, 0, io.EOF
	}
	return frame.b, frame.keyframe, frame.timestamp, nil
}

func (r *blockReader) Close() error {
	close(r.closed)
	return nil
}

func (r *blockReader) TrackEntry() TrackEntry {
	return cloneTrackEntry(r.trackEntry)
}

type trackInfo struct {
	entry   TrackEntry
	decoder *contentDecoder
}

// NewSimpleBlockReader creates BlockReadCloserWithTrackEntry for each track specified as tracks argument.
// It reads SimpleBlock-s and BlockGroup.Block-s. Any optional data in BlockGroup are dropped.
// If you need full data, consider implementing a custom reader using ebml.Unmarshal.
//
// Note that, keyframe flag from BlockGroup.Block may be incorrect.
// If you have knowledge about this, please consider fixing it.
//
// By default, the frames and TrackEntry().CodecPrivate are returned exactly as
// stored in the container. With WithContentDecoding, the Matroska
// ContentEncodings declared by the tracks are undone before returning the data.
func NewSimpleBlockReader(r io.Reader, opts ...BlockReaderOption) ([]BlockReadCloserWithTrackEntry, error) {
	options := &BlockReaderOptions{
		BlockReadWriterOptions: BlockReadWriterOptions{
			onFatal: func(err error) {
				panic(err)
			},
		},
	}
	for _, o := range opts {
		if err := o.ApplyToBlockReaderOptions(options); err != nil {
			return nil, err
		}
	}

	var trackInfos []trackInfo
	if options.contentDecoding {
		var header struct {
			Segment struct {
				Tracks struct {
					TrackEntry []map[string]interface{}
				} `ebml:"Tracks,stop"`
			}
		}
		switch err := ebml.Unmarshal(r, &header, options.unmarshalOpts...); err {
		case ebml.ErrReadStopped:
		default:
			return nil, err
		}

		decodingLimits := &contentDecodingOptions{
			maxFrameBytes:   options.maxFrameBytes,
			maxPrivateBytes: options.maxPrivateBytes,
		}
		trackInfos = make([]trackInfo, 0, len(header.Segment.Tracks.TrackEntry))
		for _, m := range header.Segment.Tracks.TrackEntry {
			parsed := parseTrackEntryMap(m)
			decoder, err := parsed.buildDecoder(decodingLimits)
			if err != nil {
				return nil, err
			}
			trackInfos = append(trackInfos, trackInfo{
				entry:   parsed.entry,
				decoder: decoder,
			})
		}
	} else {
		var header struct {
			Segment struct {
				Tracks struct {
					TrackEntry []TrackEntry
				} `ebml:"Tracks,stop"`
			}
		}
		switch err := ebml.Unmarshal(r, &header, options.unmarshalOpts...); err {
		case ebml.ErrReadStopped:
		default:
			return nil, err
		}

		trackInfos = make([]trackInfo, 0, len(header.Segment.Tracks.TrackEntry))
		for _, t := range header.Segment.Tracks.TrackEntry {
			trackInfos = append(trackInfos, trackInfo{entry: t})
		}
	}

	var ws []BlockReadCloserWithTrackEntry
	br := make(map[uint64]*blockReader)
	decoders := make(map[uint64]*contentDecoder, len(trackInfos))

	for _, t := range trackInfos {
		r := &blockReader{
			f:          make(chan *frame),
			closed:     make(chan struct{}),
			trackEntry: t.entry,
		}
		ws = append(ws, r)
		br[t.entry.TrackNumber] = r
		decoders[t.entry.TrackNumber] = t.decoder
	}

	type blockGroup struct {
		Block             ebml.Block
		ReferencePriority uint64
	}
	type clusterReader struct {
		Timecode    chan uint64
		SimpleBlock chan ebml.Block
		BlockGroup  chan blockGroup
	}
	timecodeCh := make(chan uint64)
	blockCh := make(chan ebml.Block)
	blockGroupCh := make(chan blockGroup)
	c := struct {
		Cluster clusterReader
	}{
		Cluster: clusterReader{
			Timecode:    timecodeCh,
			SimpleBlock: blockCh,
			BlockGroup:  blockGroupCh,
		},
	}
	go func() {
		timecodeCh := timecodeCh
		blockCh := blockCh
		blockGroupCh := blockGroupCh

		var finalized bool
		finalize := func(err error) {
			if finalized {
				return
			}
			finalized = true
			for k := range br {
				br[k].terminalErr = err
				close(br[k].f)
			}
		}

		// Timecode is the first child of its Cluster, so it arrives before the
		// blocks it applies to.
		var timecode uint64
		for timecodeCh != nil || blockCh != nil || blockGroupCh != nil {
			var b *ebml.Block
			select {
			case tc, ok := <-timecodeCh:
				if !ok {
					timecodeCh = nil
					continue
				}
				timecode = tc
				continue
			case block, ok := <-blockCh:
				if !ok {
					blockCh = nil
					continue
				}
				b = &block
			case bg, ok := <-blockGroupCh:
				if !ok {
					blockGroupCh = nil
					continue
				}
				b = &bg.Block
				// FIXME: This may be wrong.
				//        ReferencePriority == 0 means that the frame is not referenced.
				b.Keyframe = bg.ReferencePriority != 0
			}

			if finalized {
				// Content decoding already failed; keep draining so that the
				// unmarshalling goroutine is not blocked on a channel send.
				continue
			}

			r := br[b.TrackNumber]
			if r == nil {
				continue
			}

			data := b.Data
			if decoder := decoders[b.TrackNumber]; decoder != nil {
				decoded, err := decoder.decodeLaces(data, options.maxFrameBytes)
				if err != nil {
					// Drop the whole block so that a block never yields a
					// subset of its laces.
					finalize(err)
					continue
				}
				data = decoded
			}

			for _, d := range data {
				frame := &frame{
					trackNumber: b.TrackNumber,
					keyframe:    b.Keyframe,
					timestamp:   int64(timecode) + int64(b.Timecode),
					b:           d,
				}
				select {
				case r.f <- frame:
				case <-r.closed:
				}
			}
		}
		finalize(nil)
	}()
	go func() {
		defer func() {
			close(timecodeCh)
			close(blockCh)
			close(blockGroupCh)
		}()
		if err := ebml.Unmarshal(r, &c, options.unmarshalOpts...); err != nil {
			if options.onFatal != nil {
				options.onFatal(err)
			}
		}
	}()

	return ws, nil
}
