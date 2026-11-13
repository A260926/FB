// Copyright 2026 the LinuxBoot Authors. All rights reserved
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package compression

import (
	"github.com/linuxboot/fiano/pkg/guid"
)

// PureGoCompressor returns a Compressor implemented entirely in Go for the
// GUIDed section GUID, or nil if the GUID has no pure-Go compressor in this
// repository. Unlike CompressorFromGUID, it never shells out to xz, brotli or
// any other external program, so callers that must stay in-process (for
// example read-only layout analysis) get deterministic behavior.
func PureGoCompressor(g *guid.GUID) Compressor {
	switch *g {
	case LZMAGUID:
		return &LZMA{}
	case LZMAX86GUID:
		return &LZMAX86{&LZMA{}}
	default:
		return nil
	}
}
