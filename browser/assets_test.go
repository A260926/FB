package browser

import (
	"bytes"
	"github.com/headzoo/surf/internal/testassert"
	"net/url"
	"testing"
)

func TestDownload(t *testing.T) {

	out := &bytes.Buffer{}
	u, _ := url.Parse("http://i.imgur.com/HW4bJtY.jpg")
	asset := NewImageAsset(u, "", "", "")
	l, err := DownloadAsset(asset, out)
	testassert.Nil(t, err)
	testassert.Greater(t, 0, int(l))
	testassert.Equal(t, int(l), out.Len())
}

func TestDownloadAsync(t *testing.T) {

	ch := make(AsyncDownloadChannel, 1)
	u1, _ := url.Parse("http://i.imgur.com/HW4bJtY.jpg")
	u2, _ := url.Parse("http://i.imgur.com/HkPOzEH.jpg")
	asset1 := NewImageAsset(u1, "", "", "")
	asset2 := NewImageAsset(u2, "", "", "")
	out1 := &bytes.Buffer{}
	out2 := &bytes.Buffer{}

	queue := 2
	DownloadAssetAsync(asset1, out1, ch)
	DownloadAssetAsync(asset2, out2, ch)

	for {
		select {
		case result := <-ch:
			testassert.Greater(t, 0, int(result.Size))
			if result.Asset == asset1 {
				testassert.Equal(t, int(result.Size), out1.Len())
			} else if result.Asset == asset2 {
				testassert.Equal(t, int(result.Size), out2.Len())
			} else {
				t.Failed()
			}
			queue--
			if queue == 0 {
				goto COMPLETE
			}
		}
	}

COMPLETE:
	close(ch)
	testassert.Equal(t, 0, queue)
}
