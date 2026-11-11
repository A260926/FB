package h264

import (
	"bytes"
	"testing"

	"github.com/stretchr/testify/require"
)

// bitWriter is a minimal bit writer used to build parameter sets and slice
// headers for the tests.
type bitWriter struct {
	buf []byte
	pos int
}

func (w *bitWriter) writeBit(v uint64) {
	if w.pos%8 == 0 {
		w.buf = append(w.buf, 0)
	}
	if (v & 0x01) != 0 {
		w.buf[w.pos/8] |= 1 << (7 - uint(w.pos%8))
	}
	w.pos++
}

func (w *bitWriter) writeBits(v uint64, n int) {
	for i := n - 1; i >= 0; i-- {
		w.writeBit((v >> uint(i)) & 0x01)
	}
}

func (w *bitWriter) writeGolombUnsigned(v uint32) {
	n := uint64(v) + 1
	leadingZeros := 0
	for tmp := n; tmp > 1; tmp >>= 1 {
		leadingZeros++
	}

	w.writeBits(0, leadingZeros)
	w.writeBits(n, leadingZeros+1)
}

func (w *bitWriter) writeGolombSigned(v int32) {
	var code uint32
	if v > 0 {
		code = uint32(v)*2 - 1
	} else {
		code = uint32(-v) * 2
	}
	w.writeGolombUnsigned(code)
}

func (w *bitWriter) writeTrailing() {
	w.writeBit(1)
	for w.pos%8 != 0 {
		w.writeBit(0)
	}
}

// escapeRBSP adds emulation prevention bytes.
func escapeRBSP(rbsp []byte) []byte {
	out := make([]byte, 0, len(rbsp))
	zeros := 0

	for _, b := range rbsp {
		if zeros == 2 && b <= 3 {
			out = append(out, 3)
			zeros = 0
		}

		out = append(out, b)

		if b == 0 {
			zeros++
		} else {
			zeros = 0
		}
	}

	return out
}

type parameterSetConfig struct {
	spsID                 uint32
	ppsID                 uint32
	log2MaxFrameNumMinus4 uint32
	log2MaxPOCLsbMinus4   uint32
	pocType               uint32
	separateColourPlane   bool
}

func buildSPS(cfg parameterSetConfig) []byte {
	w := &bitWriter{}

	if cfg.separateColourPlane {
		w.writeBits(100, 8) // profile_idc
	} else {
		w.writeBits(66, 8) // profile_idc (baseline)
	}
	w.writeBits(0, 8)  // constraint flags + reserved bits
	w.writeBits(12, 8) // level_idc
	w.writeGolombUnsigned(cfg.spsID)

	if cfg.separateColourPlane {
		w.writeGolombUnsigned(3) // chroma_format_idc
		w.writeBit(1)            // separate_colour_plane_flag
		w.writeGolombUnsigned(0) // bit_depth_luma_minus8
		w.writeGolombUnsigned(0) // bit_depth_chroma_minus8
		w.writeBit(0)            // qpprime_y_zero_transform_bypass_flag
		w.writeBit(0)            // seq_scaling_matrix_present_flag
	}

	w.writeGolombUnsigned(cfg.log2MaxFrameNumMinus4)
	w.writeGolombUnsigned(cfg.pocType)

	switch cfg.pocType {
	case 0:
		w.writeGolombUnsigned(cfg.log2MaxPOCLsbMinus4)
	case 1:
		w.writeBit(1)            // delta_pic_order_always_zero_flag
		w.writeGolombSigned(0)   // offset_for_non_ref_pic
		w.writeGolombSigned(0)   // offset_for_top_to_bottom_field
		w.writeGolombUnsigned(0) // num_ref_frames_in_pic_order_cnt_cycle
	}

	w.writeGolombUnsigned(1)  // max_num_ref_frames
	w.writeBit(0)             // gaps_in_frame_num_value_allowed_flag
	w.writeGolombUnsigned(19) // pic_width_in_mbs_minus1
	w.writeGolombUnsigned(11) // pic_height_in_map_units_minus1
	w.writeBit(1)             // frame_mbs_only_flag
	w.writeBit(1)             // direct_8x8_inference_flag
	w.writeBit(0)             // frame_cropping_flag
	w.writeBit(0)             // vui_parameters_present_flag
	w.writeTrailing()

	return append([]byte{0x67}, escapeRBSP(w.buf)...)
}

func buildPPS(ppsID uint32, spsID uint32) []byte {
	w := &bitWriter{}
	w.writeGolombUnsigned(ppsID)
	w.writeGolombUnsigned(spsID)
	w.writeBit(0)            // entropy_coding_mode_flag
	w.writeBit(0)            // bottom_field_pic_order_in_frame_present_flag
	w.writeGolombUnsigned(0) // num_slice_groups_minus1
	w.writeTrailing()

	return append([]byte{0x68}, escapeRBSP(w.buf)...)
}

type sliceParams struct {
	isIDR     bool
	ppsID     uint32
	firstMB   uint32
	sliceType uint32
	frameNum  uint32
	idrPicID  uint32
	pocLsb    uint32
}

func buildSlice(params sliceParams, cfg parameterSetConfig) []byte {
	w := &bitWriter{}
	w.writeGolombUnsigned(params.firstMB)
	w.writeGolombUnsigned(params.sliceType)
	w.writeGolombUnsigned(params.ppsID)
	w.writeBits(uint64(params.frameNum), int(cfg.log2MaxFrameNumMinus4+4))

	if params.isIDR {
		w.writeGolombUnsigned(params.idrPicID)
	}

	if cfg.pocType == 0 {
		w.writeBits(uint64(params.pocLsb), int(cfg.log2MaxPOCLsbMinus4+4))
	}

	w.writeTrailing()

	header := byte(0x41)
	if params.isIDR {
		header = 0x65
	}

	return append([]byte{header}, escapeRBSP(w.buf)...)
}

type parameterSetSample struct {
	au  [][]byte
	pts int64
}

func runDTSSequence(t *testing.T, sequence []parameterSetSample) []int64 {
	t.Helper()

	ex := NewDTSExtractor()
	out := make([]int64, len(sequence))

	for i, sample := range sequence {
		var err error
		out[i], err = ex.Extract(sample.au, sample.pts)
		require.NoError(t, err, "sample %d", i)
	}

	return out
}

func requireDTSEqual(t *testing.T, expected []int64, actual []int64) {
	t.Helper()

	require.Len(t, actual, len(expected))
	for i := range expected {
		require.Equal(t, expected[i], actual[i], "sample %d", i)
	}
}

func TestDTSParameterSets(t *testing.T) {
	cfgA := parameterSetConfig{
		spsID:                 0,
		ppsID:                 0,
		log2MaxFrameNumMinus4: 0,
		log2MaxPOCLsbMinus4:   0,
	}
	cfgB := parameterSetConfig{
		spsID:                 1,
		ppsID:                 1,
		log2MaxFrameNumMinus4: 4,
		log2MaxPOCLsbMinus4:   12,
	}

	spsA := buildSPS(cfgA)
	ppsA := buildPPS(cfgA.ppsID, cfgA.spsID)
	spsB := buildSPS(cfgB)
	ppsB := buildPPS(cfgB.ppsID, cfgB.spsID)

	picture := func(params sliceParams, cfg parameterSetConfig) []byte {
		return buildSlice(params, cfg)
	}

	// base GOP with B-frames on configuration A
	base := []parameterSetSample{
		{
			au: [][]byte{
				spsA,
				ppsA,
				picture(sliceParams{isIDR: true, ppsID: 0, frameNum: 0, pocLsb: 0}, cfgA),
			},
			pts: 0,
		},
		{
			au: [][]byte{
				picture(sliceParams{ppsID: 0, frameNum: 1, pocLsb: 4}, cfgA),
			},
			pts: 6000,
		},
		{
			au: [][]byte{
				picture(sliceParams{ppsID: 0, frameNum: 2, pocLsb: 2, sliceType: 1}, cfgA),
			},
			pts: 3000,
		},
		{
			au: [][]byte{
				picture(sliceParams{ppsID: 0, frameNum: 2, pocLsb: 1, sliceType: 1}, cfgA),
			},
			pts: 1500,
		},
		{
			au: [][]byte{
				picture(sliceParams{ppsID: 0, frameNum: 3, pocLsb: 8}, cfgA),
			},
			pts: 9000,
		},
		{
			au: [][]byte{
				picture(sliceParams{ppsID: 0, frameNum: 4, pocLsb: 12}, cfgA),
			},
			pts: 12000,
		},
	}

	baseDTS := runDTSSequence(t, base)

	t.Run("inactive declarations", func(t *testing.T) {
		// The same sequence with unused SPS/PPS declarations and repeated
		// declarations interleaved. DTS must not change.
		full := make([]parameterSetSample, len(base))
		for i, sample := range base {
			au := append([][]byte{}, sample.au...)

			switch i {
			case 0:
				au = [][]byte{spsA, ppsA, spsB, ppsB, sample.au[2]}
			case 1:
				au = append([][]byte{spsA, ppsA, spsB, ppsB}, au...)
			case 2:
				au = append([][]byte{spsB, ppsB}, au...)
			}

			full[i] = parameterSetSample{au: au, pts: sample.pts}
		}

		requireDTSEqual(t, baseDTS, runDTSSequence(t, full))
	})

	t.Run("PPS id change with the same SPS", func(t *testing.T) {
		ppsC := buildPPS(2, 0)

		seq := []parameterSetSample{
			base[0],
			{
				au: [][]byte{
					ppsC,
					picture(sliceParams{ppsID: 2, frameNum: 1, pocLsb: 4}, cfgA),
				},
				pts: base[1].pts,
			},
			{
				au:  [][]byte{picture(sliceParams{ppsID: 2, frameNum: 2, pocLsb: 2, sliceType: 1}, cfgA)},
				pts: base[2].pts,
			},
			{
				au:  [][]byte{picture(sliceParams{ppsID: 2, frameNum: 2, pocLsb: 1, sliceType: 1}, cfgA)},
				pts: base[3].pts,
			},
			{
				au:  [][]byte{picture(sliceParams{ppsID: 2, frameNum: 3, pocLsb: 8}, cfgA)},
				pts: base[4].pts,
			},
			{
				au:  [][]byte{picture(sliceParams{ppsID: 2, frameNum: 4, pocLsb: 12}, cfgA)},
				pts: base[5].pts,
			},
		}

		requireDTSEqual(t, baseDTS, runDTSSequence(t, seq))
	})

	t.Run("IDR switch to a different SPS", func(t *testing.T) {
		switched := append([]parameterSetSample{}, base...)
		switched = append(switched,
			parameterSetSample{
				au: [][]byte{
					spsB,
					ppsB,
					picture(sliceParams{isIDR: true, ppsID: 1, frameNum: 0, pocLsb: 0}, cfgB),
				},
				pts: 15000,
			},
			parameterSetSample{
				au:  [][]byte{picture(sliceParams{ppsID: 1, frameNum: 1, pocLsb: 4}, cfgB)},
				pts: 21000,
			},
			parameterSetSample{
				au:  [][]byte{picture(sliceParams{ppsID: 1, frameNum: 2, pocLsb: 2, sliceType: 1}, cfgB)},
				pts: 18000,
			},
			parameterSetSample{
				au:  [][]byte{picture(sliceParams{ppsID: 1, frameNum: 2, pocLsb: 1, sliceType: 1}, cfgB)},
				pts: 16500,
			},
			parameterSetSample{
				au:  [][]byte{picture(sliceParams{ppsID: 1, frameNum: 3, pocLsb: 8}, cfgB)},
				pts: 24000,
			},
		)

		got := runDTSSequence(t, switched)

		// Outputs after the switch must match those of a fresh extractor,
		// apart from the retained DTS lower bound, which does not alter the
		// result of IDR pictures.
		reference := runDTSSequence(t, switched[len(base):])
		requireDTSEqual(t, reference, got[len(base):])

		require.GreaterOrEqual(t, got[len(base)], got[len(base)-1])
		require.Equal(t, switched[len(base)].pts, got[len(base)])
	})

	t.Run("switching SPS without IDR", func(t *testing.T) {
		ex := NewDTSExtractor()

		_, err := ex.Extract(base[0].au, base[0].pts)
		require.NoError(t, err)

		_, err = ex.Extract([][]byte{
			spsB,
			ppsB,
			picture(sliceParams{ppsID: 1, frameNum: 1, pocLsb: 4}, cfgB),
		}, base[1].pts)

		require.Error(t, err)
		require.Contains(t, err.Error(), "IDR")
		require.Contains(t, err.Error(), "SPS")
	})

	t.Run("multiple slices of the same picture", func(t *testing.T) {
		single := []parameterSetSample{
			base[0],
			base[1],
			base[2],
		}

		multi := []parameterSetSample{
			{
				au: [][]byte{
					spsA,
					ppsA,
					picture(sliceParams{isIDR: true, ppsID: 0, firstMB: 0, frameNum: 0, pocLsb: 0}, cfgA),
					picture(sliceParams{isIDR: true, ppsID: 0, firstMB: 20, frameNum: 0, pocLsb: 0}, cfgA),
				},
				pts: base[0].pts,
			},
			{
				au: [][]byte{
					picture(sliceParams{ppsID: 0, firstMB: 0, frameNum: 1, pocLsb: 4}, cfgA),
					picture(sliceParams{ppsID: 0, firstMB: 20, frameNum: 1, pocLsb: 4}, cfgA),
				},
				pts: base[1].pts,
			},
			{
				au: [][]byte{
					picture(sliceParams{ppsID: 0, firstMB: 0, frameNum: 2, pocLsb: 2, sliceType: 1}, cfgA),
					picture(sliceParams{ppsID: 0, firstMB: 40, frameNum: 2, pocLsb: 2, sliceType: 1}, cfgA),
				},
				pts: base[2].pts,
			},
		}

		requireDTSEqual(t, runDTSSequence(t, single), runDTSSequence(t, multi))
	})

	t.Run("conflicting slices", func(t *testing.T) {
		firstIDR := picture(sliceParams{isIDR: true, ppsID: 0, frameNum: 0, pocLsb: 0}, cfgA)

		conflicts := []struct {
			name         string
			slice        []byte
			declarations [][]byte
			contains     string
		}{
			{
				name:  "different PPS",
				slice: picture(sliceParams{isIDR: true, ppsID: 1, frameNum: 0, pocLsb: 0}, cfgB),
				declarations: [][]byte{
					spsB,
					ppsB,
				},
				contains: "PPS",
			},
			{
				name:     "different frame_num",
				slice:    picture(sliceParams{isIDR: true, ppsID: 0, frameNum: 1, pocLsb: 0}, cfgA),
				contains: "frame_num",
			},
			{
				name:     "different POC",
				slice:    picture(sliceParams{isIDR: true, ppsID: 0, frameNum: 0, pocLsb: 1}, cfgA),
				contains: "pic_order_cnt_lsb",
			},
			{
				name:     "different IDR identity",
				slice:    picture(sliceParams{ppsID: 0, frameNum: 0, pocLsb: 0}, cfgA),
				contains: "IDR",
			},
		}

		for _, ca := range conflicts {
			t.Run(ca.name, func(t *testing.T) {
				au := append([][]byte{spsA, ppsA}, ca.declarations...)
				au = append(au, firstIDR, ca.slice)

				ex := NewDTSExtractor()
				_, err := ex.Extract(au, 0)
				require.Error(t, err)
				require.Contains(t, err.Error(), ca.contains)
			})
		}
	})

	t.Run("missing references", func(t *testing.T) {
		t.Run("unknown PPS", func(t *testing.T) {
			ex := NewDTSExtractor()
			_, err := ex.Extract([][]byte{
				spsA,
				ppsA,
				picture(sliceParams{isIDR: true, ppsID: 3, frameNum: 0, pocLsb: 0}, cfgA),
			}, 0)
			require.Error(t, err)
			require.Contains(t, err.Error(), "PPS 3 was not received yet")
		})

		t.Run("missing SPS referenced by PPS", func(t *testing.T) {
			ex := NewDTSExtractor()
			_, err := ex.Extract([][]byte{
				spsA,
				buildPPS(2, 9),
				picture(sliceParams{isIDR: true, ppsID: 2, frameNum: 0, pocLsb: 0}, cfgA),
			}, 0)
			require.Error(t, err)
			require.Contains(t, err.Error(), "SPS 9 referenced by PPS 2 was not received yet")
		})

		t.Run("declaration after the slice", func(t *testing.T) {
			// SPS 9 is declared after the IDR that references it: the
			// declaration must not be applied backwards.
			ex := NewDTSExtractor()
			_, err := ex.Extract([][]byte{
				spsA,
				buildPPS(2, 9),
				picture(sliceParams{isIDR: true, ppsID: 2, frameNum: 0, pocLsb: 0}, cfgA),
				buildSPS(parameterSetConfig{
					spsID:                 9,
					log2MaxFrameNumMinus4: 0,
					log2MaxPOCLsbMinus4:   0,
				}),
			}, 0)
			require.Error(t, err)
			require.Contains(t, err.Error(), "SPS 9 referenced by PPS 2 was not received yet")
		})

		t.Run("PPS declared after the slice", func(t *testing.T) {
			ex := NewDTSExtractor()
			_, err := ex.Extract([][]byte{
				spsA,
				buildPPS(9, 0),
				picture(sliceParams{isIDR: true, ppsID: 0, frameNum: 0, pocLsb: 0}, cfgA),
				ppsA,
			}, 0)
			require.Error(t, err)
			require.Contains(t, err.Error(), "PPS 0 was not received yet")
		})
	})

	t.Run("unsupported coding mode", func(t *testing.T) {
		cfgType1 := parameterSetConfig{
			spsID:                 0,
			ppsID:                 0,
			log2MaxFrameNumMinus4: 0,
			pocType:               1,
		}

		ex := NewDTSExtractor()
		_, err := ex.Extract([][]byte{
			buildSPS(cfgType1),
			buildPPS(0, 0),
			picture(sliceParams{isIDR: true, ppsID: 0, frameNum: 0}, cfgType1),
		}, 0)
		require.Error(t, err)
		require.Contains(t, err.Error(), "pic_order_cnt_type = 1 is not supported yet")

		cfgSeparatePlane := parameterSetConfig{
			spsID:                 0,
			ppsID:                 0,
			log2MaxFrameNumMinus4: 0,
			log2MaxPOCLsbMinus4:   0,
			separateColourPlane:   true,
		}

		ex = NewDTSExtractor()
		_, err = ex.Extract([][]byte{
			buildSPS(cfgSeparatePlane),
			buildPPS(0, 0),
			picture(sliceParams{isIDR: true, ppsID: 0, frameNum: 0, pocLsb: 0}, cfgSeparatePlane),
		}, 0)
		require.Error(t, err)
		require.Contains(t, err.Error(), "separate colour planes are not supported")
	})

	t.Run("POC type 2 slices", func(t *testing.T) {
		cfgType2 := parameterSetConfig{
			spsID:                 0,
			ppsID:                 0,
			log2MaxFrameNumMinus4: 0,
			pocType:               2,
		}

		ex := NewDTSExtractor()
		dts, err := ex.Extract([][]byte{
			buildSPS(cfgType2),
			buildPPS(0, 0),
			picture(sliceParams{isIDR: true, ppsID: 0, firstMB: 0, frameNum: 0}, cfgType2),
			picture(sliceParams{isIDR: true, ppsID: 0, firstMB: 20, frameNum: 0}, cfgType2),
		}, 0)
		require.NoError(t, err)
		require.Equal(t, int64(0), dts)

		ex = NewDTSExtractor()
		_, err = ex.Extract([][]byte{
			buildSPS(cfgType2),
			buildPPS(0, 0),
			picture(sliceParams{isIDR: true, ppsID: 0, frameNum: 0}, cfgType2),
			picture(sliceParams{isIDR: true, ppsID: 0, frameNum: 1}, cfgType2),
		}, 0)
		require.Error(t, err)
		require.Contains(t, err.Error(), "frame_num")
	})

	t.Run("buffer reuse", func(t *testing.T) {
		sps := buildSPS(cfgA)
		pps := buildPPS(0, 0)
		idr := picture(sliceParams{isIDR: true, ppsID: 0, frameNum: 0, pocLsb: 0}, cfgA)

		ex := NewDTSExtractor()
		_, err := ex.Extract([][]byte{sps, pps, idr}, 0)
		require.NoError(t, err)

		control := NewDTSExtractor()
		_, err = control.Extract([][]byte{
			buildSPS(cfgA),
			buildPPS(0, 0),
			picture(sliceParams{isIDR: true, ppsID: 0, frameNum: 0, pocLsb: 0}, cfgA),
		}, 0)
		require.NoError(t, err)

		// the caller reuses the buffers that were passed to Extract
		for i := range sps {
			sps[i] = 0xFF
		}
		for i := range pps {
			pps[i] = 0xFF
		}

		exDTS, err := ex.Extract([][]byte{
			picture(sliceParams{ppsID: 0, frameNum: 1, pocLsb: 4}, cfgA),
		}, 6000)
		require.NoError(t, err)

		controlDTS, err := control.Extract([][]byte{
			picture(sliceParams{ppsID: 0, frameNum: 1, pocLsb: 4}, cfgA),
		}, 6000)
		require.NoError(t, err)
		require.Equal(t, controlDTS, exDTS)
	})
}

func TestPPSUnmarshal(t *testing.T) {
	t.Run("valid", func(t *testing.T) {
		var pps PPS
		err := pps.Unmarshal(buildPPS(2, 3))
		require.NoError(t, err)
		require.Equal(t, uint32(2), pps.ID)
		require.Equal(t, uint32(3), pps.SPSID)
	})

	t.Run("truncated", func(t *testing.T) {
		var pps PPS
		require.EqualError(t, pps.Unmarshal([]byte{0x68}), "not enough bits")
	})

	t.Run("invalid PPS id", func(t *testing.T) {
		w := &bitWriter{}
		w.writeGolombUnsigned(300)
		w.writeGolombUnsigned(0)
		w.writeTrailing()

		var pps PPS
		err := pps.Unmarshal(append([]byte{0x68}, escapeRBSP(w.buf)...))
		require.Error(t, err)
		require.Contains(t, err.Error(), "invalid pic_parameter_set_id")
	})

	t.Run("invalid SPS id", func(t *testing.T) {
		w := &bitWriter{}
		w.writeGolombUnsigned(0)
		w.writeGolombUnsigned(40)
		w.writeTrailing()

		var pps PPS
		err := pps.Unmarshal(append([]byte{0x68}, escapeRBSP(w.buf)...))
		require.Error(t, err)
		require.Contains(t, err.Error(), "invalid seq_parameter_set_id")
	})
}

func TestPPSUnmarshalEmulationPrevention(t *testing.T) {
	// 00 00 03 sequences must be removed before reading the Exp-Golomb
	// fields.
	w := &bitWriter{}
	w.writeGolombUnsigned(0)
	w.writeGolombUnsigned(0)
	w.writeBits(0, 6)  // align to a byte boundary
	w.writeBits(0, 16) // 00 00
	w.writeBits(3, 8)  // 03
	w.writeTrailing()

	escaped := escapeRBSP(w.buf)
	require.True(t, bytes.Contains(escaped, []byte{0x00, 0x00, 0x03, 0x03}))

	var pps PPS
	require.NoError(t, pps.Unmarshal(append([]byte{0x68}, escaped...)))
	require.Equal(t, uint32(0), pps.ID)
	require.Equal(t, uint32(0), pps.SPSID)
}
