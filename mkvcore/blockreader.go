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
	"sync"

	"github.com/at-wat/ebml-go"
)

type blockReader struct {
	f          chan *frame
	closed     chan struct{}
	trackEntry TrackEntry
	decoder    *contentDecoder // nil keeps the raw container-byte path

	termMu  sync.Mutex
	termErr error
}

func (r *blockReader) Read() (b []byte, keyframe bool, timestamp int64, err error) {
	frame, ok := <-r.f
	if !ok {
		r.termMu.Lock()
		// The terminal error is delivered exactly once; every later Read
		// returns io.EOF.
		err = r.termErr
		r.termErr = nil
		r.termMu.Unlock()
		if err != nil {
			return nil, false, 0, err
		}
		return nil, false, 0, io.EOF
	}
	return frame.b, frame.keyframe, frame.timestamp, nil
}

func (r *blockReader) Close() error {
	close(r.closed)
	return nil
}

// TrackEntry returns an independent copy of the track metadata. When content
// decoding is enabled, CodecPrivate is the restored codec configuration while
// ContentEncodings keeps the original source declaration. Mutating returned
// slices never affects later Read or later TrackEntry calls.
func (r *blockReader) TrackEntry() TrackEntry {
	return cloneTrackEntry(r.trackEntry)
}

func (r *blockReader) setTermErr(err error) {
	r.termMu.Lock()
	if r.termErr == nil {
		r.termErr = err
	}
	r.termMu.Unlock()
}

// NewSimpleBlockReader creates BlockReadCloserWithTrackEntry for each track specified as tracks argument.
// It reads SimpleBlock-s and BlockGroup.Block-s. Any optional data in BlockGroup are dropped.
// If you need full data, consider implementing a custom reader using ebml.Unmarshal.
//
// Note that, keyframe flag from BlockGroup.Block may be incorrect.
// If you have knowledge about this, please consider fixing it.
//
// By default the readers return the exact bytes stored in the container.
// WithContentDecoding opts in to transparent Matroska ContentEncoding
// decoding instead.
func NewSimpleBlockReader(r io.Reader, opts ...BlockReaderOption) ([]BlockReadCloserWithTrackEntry, error) {
	options := &BlockReaderOptions{}
	for _, o := range opts {
		if err := o.ApplyToBlockReaderOptions(options); err != nil {
			return nil, err
		}
	}
	// fatalCh carries structural unmarshal failures to the processing
	// goroutine when content decoding is enabled, so they terminate the
	// readers with an error instead of killing the process with a panic.
	fatalCh := make(chan error, 1)
	userFatal := options.onFatal
	switch {
	case options.contentDecoding:
		options.onFatal = func(err error) {
			select {
			case fatalCh <- err:
			default:
			}
			if userFatal != nil {
				userFatal(err)
			}
		}
	case options.onFatal == nil:
		options.onFatal = func(err error) {
			panic(err)
		}
	}

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

	var ws []BlockReadCloserWithTrackEntry
	br := make(map[uint64]*blockReader)
	decoders := make(map[uint64]*contentDecoder)

	// Validate every track declaration and CodecPrivate up front, before any
	// background reading starts. Tracks without ContentEncoding get no
	// decoder and keep the raw read path.
	if options.contentDecoding {
		for _, t := range header.Segment.Tracks.TrackEntry {
			d, err := newContentDecoder(t, options.maxFrameBytes, options.maxPrivateBytes)
			if err != nil {
				return nil, err
			}
			decoders[t.TrackNumber] = d
		}
	}

	for _, t := range header.Segment.Tracks.TrackEntry {
		public := t
		if d := decoders[t.TrackNumber]; d != nil {
			// A missing CodecPrivate stays absent; prefixes never synthesize
			// one. restoredPrivate is nil when the source had none.
			public.CodecPrivate = d.restoredPrivate
		}
		rdr := &blockReader{
			f:          make(chan *frame),
			closed:     make(chan struct{}),
			trackEntry: public,
			decoder:    decoders[t.TrackNumber],
		}
		ws = append(ws, rdr)
		br[t.TrackNumber] = rdr
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
		// Timecode is the first child of its Cluster, so it arrives before the
		// blocks it applies to.
		var timecode uint64
		var fatal error
	L_READ:
		for {
			var b *ebml.Block
			select {
			case err := <-fatalCh:
				fatal = err
				break L_READ
			case tc, ok := <-timecodeCh:
				if !ok {
					timecodeCh = nil
					if blockCh == nil && blockGroupCh == nil {
						break L_READ
					}
					continue
				}
				timecode = tc
				continue
			case block, ok := <-blockCh:
				if !ok {
					blockCh = nil
					if timecodeCh == nil && blockGroupCh == nil {
						break L_READ
					}
					continue
				}
				b = &block
			case bg, ok := <-blockGroupCh:
				if !ok {
					blockGroupCh = nil
					if timecodeCh == nil && blockCh == nil {
						break L_READ
					}
					continue
				}
				b = &bg.Block
				// FIXME: This may be wrong.
				//        ReferencePriority == 0 means that the frame is not referenced.
				b.Keyframe = bg.ReferencePriority != 0
			}
			rdr := br[b.TrackNumber]
			if rdr == nil {
				continue
			}

			// First split the block into its laced frames, then apply the
			// track's own encoding chain to each frame separately. The whole
			// block is decoded before any frame is delivered so a failure can
			// never expose half of a laced group.
			frames := make([]*frame, 0, len(b.Data))
			for l := range b.Data {
				payload := b.Data[l]
				if rdr.decoder != nil {
					decoded, err := rdr.decoder.decodeFrame(payload)
					if err != nil {
						fatal = err
						break L_READ
					}
					payload = decoded
				}
				frames = append(frames, &frame{
					trackNumber: b.TrackNumber,
					keyframe:    b.Keyframe,
					timestamp:   int64(timecode) + int64(b.Timecode),
					b:           payload,
				})
			}
			for _, f := range frames {
				select {
				case rdr.f <- f:
				case <-rdr.closed:
				}
			}
		}

		// A structural failure both buffers an error in fatalCh and closes the
		// element channels; the select above may have exited via the closures
		// without consuming the buffered error. Pick it up here so a parse
		// failure terminates readers with an error rather than plain EOF.
		if fatal == nil {
			select {
			case fatal = <-fatalCh:
			default:
			}
		}

		if fatal != nil {
			// Every still-open reader gets the same terminal error; closing
			// the frame channels makes the next Read return it, and reads
			// after that return io.EOF. Already-closed readers must not block
			// this shutdown, and closing a channel never blocks.
			for _, rdr := range br {
				rdr.setTermErr(fatal)
			}
			// Drain elements the unmarshal goroutine may still send so it can
			// reach its deferred channel close instead of blocking forever.
			for blockCh != nil || blockGroupCh != nil || timecodeCh != nil {
				select {
				case _, ok := <-timecodeCh:
					if !ok {
						timecodeCh = nil
					}
				case _, ok := <-blockCh:
					if !ok {
						blockCh = nil
					}
				case _, ok := <-blockGroupCh:
					if !ok {
						blockGroupCh = nil
					}
				}
			}
		}
		for _, rdr := range br {
			close(rdr.f)
		}
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
