package srt

import (
	"testing"

	"github.com/stretchr/testify/require"
)

func TestAbsoluteBandwidthConfig(t *testing.T) {
	t.Run("Validate", func(t *testing.T) {
		// Values below -1 are rejected
		config := DefaultConfig()
		config.MaxBW = -2
		require.Error(t, config.Validate())

		// -1 (default), 0, and positive values are accepted
		for _, maxBW := range []int64{-1, 0, 1, 1000} {
			config := DefaultConfig()
			config.MaxBW = maxBW
			require.NoError(t, config.Validate(), "MaxBW=%d must be accepted", maxBW)
			require.Equal(t, maxBW, config.MaxBW)
		}
	})

	t.Run("MarshalUnmarshalURL", func(t *testing.T) {
		// A positive MaxBW enables the absolute bandwidth mode and
		// must survive a roundtrip through the URL representation.
		config := DefaultConfig()
		config.MaxBW = 1000

		srturl := config.MarshalURL("localhost:6000")
		require.Contains(t, srturl, "maxbw=1000")

		parsed := DefaultConfig()
		_, err := parsed.UnmarshalURL(srturl)
		require.NoError(t, err)
		require.Equal(t, int64(1000), parsed.MaxBW)
		require.NoError(t, parsed.Validate())

		// Zero is a valid, explicit value
		config = DefaultConfig()
		config.MaxBW = 0

		srturl = config.MarshalURL("localhost:6000")
		require.Contains(t, srturl, "maxbw=0")

		parsed = DefaultConfig()
		_, err = parsed.UnmarshalURL(srturl)
		require.NoError(t, err)
		require.Equal(t, int64(0), parsed.MaxBW)
		require.NoError(t, parsed.Validate())

		// The default keeps the previous behavior and is not marshalled
		config = DefaultConfig()
		require.Equal(t, int64(-1), config.MaxBW)

		srturl = config.MarshalURL("localhost:6000")
		require.NotContains(t, srturl, "maxbw")

		parsed = DefaultConfig()
		_, err = parsed.UnmarshalURL(srturl)
		require.NoError(t, err)
		require.Equal(t, int64(-1), parsed.MaxBW)
		require.NoError(t, parsed.Validate())

		// Values below -1 are rejected after parsing
		parsed = DefaultConfig()
		_, err = parsed.UnmarshalURL("srt://localhost:6000?maxbw=-2")
		require.NoError(t, err)
		require.Equal(t, int64(-2), parsed.MaxBW)
		require.Error(t, parsed.Validate())
	})
}
