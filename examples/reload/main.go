// Command reload demonstrates low latency HLS reload coordination using the
// EXT-X-RENDITION-REPORT tag. It parses two media playlists, plans a blocking
// reload request from the video rendition towards the audio rendition, and
// commits the (locally simulated) response.
package main

import (
	"fmt"
	"os"
	"strings"

	m3u8 "github.com/grafov/m3u8"
)

const videoPlaylist = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:2
#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES
#EXT-X-MEDIA-SEQUENCE:100
#EXT-X-PART:DURATION=0.50000,URI="v100.p0.m4s"
#EXT-X-PART:DURATION=0.50000,URI="v100.p1.m4s"
#EXTINF:1.000,
v100.ts
#EXT-X-RENDITION-REPORT:URI="audio.m3u8",LAST-MSN=700,LAST-PART=0
`

const audioPlaylist = `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:2
#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES
#EXT-X-MEDIA-SEQUENCE:700
`

const (
	sourceURI = "https://example.invalid/video.m3u8"
	targetURI = "https://example.invalid/audio.m3u8"
)

func main() {
	video, err := parseMedia(videoPlaylist)
	if err != nil {
		fmt.Fprintf(os.Stderr, "parse video playlist: %v\n", err)
		os.Exit(1)
	}
	audio, err := parseMedia(audioPlaylist)
	if err != nil {
		fmt.Fprintf(os.Stderr, "parse audio playlist: %v\n", err)
		os.Exit(1)
	}

	sync := m3u8.NewReloadSync()
	if err := sync.Update(sourceURI, video); err != nil {
		fmt.Fprintf(os.Stderr, "update source: %v\n", err)
		os.Exit(1)
	}
	if err := sync.Update(targetURI, audio); err != nil {
		fmt.Fprintf(os.Stderr, "update target: %v\n", err)
		os.Exit(1)
	}

	req, err := sync.Plan(sourceURI, targetURI)
	if err != nil {
		fmt.Fprintf(os.Stderr, "plan reload: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("planned request: %s\n", req.URL)

	// In a real client this URL would be fetched with a blocking GET. Here we
	// simulate the response playlist that publishes the requested part.
	response, err := parseMedia(`#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:2
#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES
#EXT-X-MEDIA-SEQUENCE:700
#EXT-X-PART:DURATION=0.25000,URI="a700.p0.m4s"
#EXT-X-PART:DURATION=0.25000,URI="a700.p1.m4s"
`)
	if err != nil {
		fmt.Fprintf(os.Stderr, "parse response playlist: %v\n", err)
		os.Exit(1)
	}
	if err := sync.Commit(req.Token, response); err != nil {
		fmt.Fprintf(os.Stderr, "commit response: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("committed response for MSN %d part %d\n", req.MSN, *req.Part)
}

func parseMedia(text string) (*m3u8.MediaPlaylist, error) {
	p, listType, err := m3u8.DecodeFrom(strings.NewReader(text), true)
	if err != nil {
		return nil, err
	}
	if listType != m3u8.MEDIA {
		return nil, fmt.Errorf("expected a media playlist, got list type %v", listType)
	}
	return p.(*m3u8.MediaPlaylist), nil
}
