// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package prog

import (
	"bytes"
	"encoding/hex"
	"sort"
	"testing"

	"github.com/google/syzkaller/pkg/image"
	"github.com/stretchr/testify/require"
)

// This observes the supplied starting point, including its stop behaviour.
// It is not the regression suite for the requested change.
func TestUserFBBaselineHints(t *testing.T) {
	target := initTargetTest(t, "test", "64")
	p, err := target.Deserialize([]byte(`test$hint_data(&AUTO="0809101112131415")`), Strict)
	require.NoError(t, err)
	before := bytes.Clone(p.Serialize())
	comps := make(CompMap)
	comps.Add(0x101, 0x12111009, 0x42, true)
	comps.Add(0x102, 0x13121110, 0x43, true)
	var outputs []string
	p.MutateWithHints(0, comps, func(candidate *Prog) bool {
		outputs = append(outputs, hex.EncodeToString(candidate.Calls[0].Args[0].(*PointerArg).Res.(*DataArg).Data()))
		return true
	})
	sort.Strings(outputs)
	require.Equal(t, []string{"0809430000001415", "0842000000131415"}, outputs)
	require.Equal(t, before, p.Serialize())
	t.Logf("NORMAL raw_candidates=%d source_unchanged=true outputs=%v", len(outputs), outputs)

	stopped := 0
	p.MutateWithHints(0, comps, func(candidate *Prog) bool {
		stopped++
		return false
	})
	require.Equal(t, 2, stopped, "starting-point witness changed")
	require.Equal(t, before, p.Serialize())
	t.Logf("WITNESS raw_callbacks_after_false=%d", stopped)

	p, err = target.Deserialize([]byte(`ioctl$1(0x0, 0x111, 0x0)`), Strict)
	require.NoError(t, err)
	comps = make(CompMap)
	comps.Add(0x103, 0x111, 0x666, true)
	var scalar []string
	p.MutateWithHints(0, comps, func(candidate *Prog) bool {
		scalar = append(scalar, string(candidate.Serialize()))
		return true
	})
	require.Len(t, scalar, 1)
	require.Contains(t, scalar[0], "0x666")
	t.Log("NORMAL scalar_candidates=1")

	typ := target.SyscallMap["serialize3"].Args[0].Type.(*PtrType).Elem.(*BufferType)
	require.Equal(t, BufferCompressed, typ.Kind)
	original := []byte{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}
	arg := MakeDataArg(typ, DirIn, image.Compress(original))
	comps = make(CompMap)
	comps.Add(0x104, 0x33221100, 0xaabbccdd, true)
	comps.Add(0x105, 0x77665544, 0xaabbccdd, true)
	images0, memory0 := image.StatImages.Load(), image.StatMemory.Load()
	var imageOutputs []string
	generateHints(comps, arg, nil, func() bool {
		require.Equal(t, images0, image.StatImages.Load(), "mapping held during callback")
		data, dtor := image.MustDecompress(arg.Data())
		imageOutputs = append(imageOutputs, hex.EncodeToString(data))
		dtor()
		return true
	})
	sort.Strings(imageOutputs)
	require.Equal(t, []string{"00112233ddccbbaa", "ddccbbaa44556677"}, imageOutputs)
	stopped = 0
	generateHints(comps, arg, nil, func() bool { stopped++; return false })
	require.Equal(t, 1, stopped)
	raw, dtor := image.MustDecompress(arg.Data())
	require.Equal(t, original, raw)
	dtor()
	require.Equal(t, images0, image.StatImages.Load())
	require.Equal(t, memory0, image.StatMemory.Load())
	t.Log("NORMAL compressed_candidates=2 stop_callbacks=1 mappings_released=true")
}
