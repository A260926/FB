package m3u8

/*
 Tests for low latency HLS reload coordination and the EXT-X-PART /
 EXT-X-RENDITION-REPORT / EXT-X-SERVER-CONTROL parsing and encoding.

 Copyright 2013-2019 The Project Developers.
 See the AUTHORS and LICENSE files at the top-level directory of this distribution
 and at https://github.com/grafov/m3u8/
*/

import (
	"math"
	"net/url"
	"strings"
	"testing"

	"github.com/grafov/m3u8/task_support/taskcheck"
)

const reloadPlaylistText = "#EXTM3U\n" +
	"#EXT-X-VERSION:9\n" +
	"#EXT-X-TARGETDURATION:4\n" +
	"#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES,PART-HOLD-BACK=1.5\n" +
	"#EXT-X-MEDIA-SEQUENCE:100\n" +
	"#EXT-X-PART:DURATION=0.50000,URI=\"v100.p0.m4s\"\n" +
	"#EXT-X-PART:DURATION=0.50000,URI=\"v100.p1.m4s\"\n" +
	"#EXTINF:1.000,\n" +
	"v100.ts\n" +
	"#EXTINF:1.000,\n" +
	"v101.ts\n" +
	"#EXT-X-PART:DURATION=0.25000,URI=\"v102.p0.m4s\"\n" +
	"#EXT-X-PART:DURATION=0.25000,URI=\"v102.p1.m4s\"\n" +
	"#EXT-X-RENDITION-REPORT:URI=\"audio700.m3u8\",LAST-MSN=700,LAST-PART=0\n" +
	"#EXT-X-RENDITION-REPORT:URI=\"audio800.m3u8\",LAST-MSN=800\n" +
	"#EXT-X-RENDITION-REPORT:URI=\"audio900.m3u8\"\n"

func decodeReloadMedia(t *testing.T, text string, strict bool) (*MediaPlaylist, error) {
	t.Helper()
	pl, _ := NewMediaPlaylist(0, 16)
	err := pl.DecodeFrom(strings.NewReader(text), strict)
	return pl, err
}

func TestReloadTags(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		p, err := decodeReloadMedia(t, reloadPlaylistText, true)
		c.Check(err == nil, "strict decode failed: %v", err)
		if err != nil {
			return
		}

		c.Check(p.Count() == 2, "complete segment count should exclude parts, got %d", p.Count())
		c.Check(p.SeqNo == 100, "media sequence should stay 100, got %d", p.SeqNo)
		c.Check(p.CanBlockReload, "CAN-BLOCK-RELOAD=YES should enable blocking")

		segs := p.GetAllSegments()
		c.Check(len(segs) == 2, "expected 2 complete segments, got %d", len(segs))
		if len(segs) == 2 {
			c.Check(len(segs[0].Parts) == 2, "first segment should own 2 parts, got %d", len(segs[0].Parts))
			if len(segs[0].Parts) == 2 {
				c.Check(segs[0].Parts[0].URI == "v100.p0.m4s", "unexpected part URI %q", segs[0].Parts[0].URI)
				c.Check(segs[0].Parts[1].Duration == 0.5, "unexpected part duration %v", segs[0].Parts[1].Duration)
			}
			c.Check(len(segs[1].Parts) == 0, "second segment should own no parts, got %d", len(segs[1].Parts))
			c.Check(segs[0].SeqId == 100 && segs[1].SeqId == 101, "segment sequence ids should be 100,101")
		}

		c.Check(len(p.TrailingParts) == 2, "expected 2 trailing parts, got %d", len(p.TrailingParts))
		if len(p.TrailingParts) == 2 {
			c.Check(p.TrailingParts[0].URI == "v102.p0.m4s", "unexpected trailing part URI %q", p.TrailingParts[0].URI)
		}

		c.Check(len(p.RenditionReports) == 3, "expected 3 rendition reports, got %d", len(p.RenditionReports))
		if len(p.RenditionReports) == 3 {
			r := p.RenditionReports[0]
			c.Check(r.URI == "audio700.m3u8", "unexpected report URI %q", r.URI)
			c.Check(r.LastMSN != nil && *r.LastMSN == 700, "explicit LAST-MSN should parse as 700")
			c.Check(r.LastPart != nil && *r.LastPart == 0, "explicit LAST-PART=0 must be preserved as a pointer to 0")
			r = p.RenditionReports[1]
			c.Check(r.LastMSN != nil && *r.LastMSN == 800, "explicit LAST-MSN should parse as 800")
			c.Check(r.LastPart == nil, "omitted LAST-PART must stay nil")
			r = p.RenditionReports[2]
			c.Check(r.LastMSN == nil, "omitted LAST-MSN must stay nil")
			c.Check(r.LastPart == nil, "omitted LAST-PART must stay nil")
		}

		// Encoding preserves omitted versus explicit zero.
		encoded := p.Encode().String()
		c.Check(strings.Contains(encoded, `#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES`), "server control not encoded:\n%s", encoded)
		c.Check(strings.Contains(encoded, `#EXT-X-RENDITION-REPORT:URI="audio900.m3u8"`), "omitted report attributes should stay omitted:\n%s", encoded)
		c.Check(strings.Contains(encoded, `#EXT-X-RENDITION-REPORT:URI="audio700.m3u8",LAST-MSN=700,LAST-PART=0`), "explicit zero report not encoded:\n%s", encoded)

		// Re-read after Encode must agree with the original parse.
		rp, err := decodeReloadMedia(t, encoded, true)
		c.Check(err == nil, "re-decode of encoded playlist failed: %v", err)
		if err == nil {
			c.Check(rp.Count() == p.Count(), "roundtrip count mismatch %d != %d", rp.Count(), p.Count())
			c.Check(len(rp.TrailingParts) == len(p.TrailingParts), "roundtrip trailing parts mismatch")
			c.Check(len(rp.RenditionReports) == len(p.RenditionReports), "roundtrip reports mismatch")
			c.Check(rp.CanBlockReload, "roundtrip lost CAN-BLOCK-RELOAD")
			rpSegs := rp.GetAllSegments()
			if len(rpSegs) == len(segs) && len(rpSegs) > 0 {
				c.Check(len(rpSegs[0].Parts) == len(segs[0].Parts), "roundtrip lost segment parts")
			}
			if len(rp.RenditionReports) == 3 {
				c.Check(rp.RenditionReports[1].LastPart == nil, "roundtrip must keep omitted LAST-PART nil")
				c.Check(rp.RenditionReports[0].LastPart != nil && *rp.RenditionReports[0].LastPart == 0, "roundtrip must keep explicit zero")
			}
		}

		// The generic DecodeFrom entry point must behave identically.
		global, listType, err := DecodeFrom(strings.NewReader(reloadPlaylistText), true)
		c.Check(err == nil, "global DecodeFrom failed: %v", err)
		c.Check(listType == MEDIA, "global DecodeFrom detected unexpected list type %v", listType)
		if err == nil && listType == MEDIA {
			gm := global.(*MediaPlaylist)
			c.Check(gm.Count() == p.Count(), "global decode count mismatch")
			c.Check(len(gm.TrailingParts) == len(p.TrailingParts), "global decode trailing parts mismatch")
			c.Check(len(gm.RenditionReports) == len(p.RenditionReports), "global decode reports mismatch")
			c.Check(gm.CanBlockReload, "global decode lost CAN-BLOCK-RELOAD")
		}

		// Strict validation of PART attributes.
		badCases := map[string]string{
			"empty part URI":     "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-PART:DURATION=1.0,URI=\"\"\n",
			"zero part duration": "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-PART:DURATION=0,URI=\"a.m4s\"\n",
			"negative duration":  "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-PART:DURATION=-1.0,URI=\"a.m4s\"\n",
			"infinite duration":  "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-PART:DURATION=Inf,URI=\"a.m4s\"\n",
			"missing duration":   "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-PART:URI=\"a.m4s\"\n",
			"empty report URI":   "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-RENDITION-REPORT:URI=\"\",LAST-MSN=7\n",
			"duplicate report":   "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-RENDITION-REPORT:URI=\"a.m3u8\",LAST-MSN=7\n#EXT-X-RENDITION-REPORT:URI=\"a.m3u8\",LAST-MSN=8\n",
			"negative last msn":  "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-RENDITION-REPORT:URI=\"a.m3u8\",LAST-MSN=-1\n",
			"negative last part": "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-RENDITION-REPORT:URI=\"a.m3u8\",LAST-MSN=1,LAST-PART=-2\n",
			"hex last msn":       "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-RENDITION-REPORT:URI=\"a.m3u8\",LAST-MSN=0x10\n",
		}
		for name, text := range badCases {
			_, err := decodeReloadMedia(t, text, true)
			c.Check(err != nil, "strict decode should reject %s", name)
		}

		// Non-strict parsing keeps going on the same malformed inputs.
		if _, err := decodeReloadMedia(t, badCases["negative last part"], false); err != nil {
			c.Check(err == nil, "non-strict decode should not fail: %v", err)
		}
	})
}

func newReloadMedia(t *testing.T, seq uint64) *MediaPlaylist {
	t.Helper()
	p, err := NewMediaPlaylist(0, 8)
	if err != nil {
		t.Fatalf("NewMediaPlaylist: %v", err)
	}
	p.SeqNo = seq
	return p
}

func appendSegs(t *testing.T, p *MediaPlaylist, seq uint64, uris ...string) {
	t.Helper()
	for _, u := range uris {
		if err := p.Append(u, 1.0, ""); err != nil {
			t.Fatalf("Append(%q): %v", u, err)
		}
	}
}

func mustU(winsize, capacity uint) (*MediaPlaylist, error) {
	return NewMediaPlaylist(winsize, capacity)
}

func u64ptr(v uint64) *uint64 { return &v }

func mustURLQuery(t *testing.T, raw string) url.Values {
	t.Helper()
	u, err := url.Parse(raw)
	if err != nil {
		t.Fatalf("parse %q: %v", raw, err)
	}
	return u.Query()
}

func TestReloadPlan(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const sourceURI = "https://example.invalid/video.m3u8"
		const targetURI = "https://example.invalid/audio.m3u8?lang=zh&_HLS_msn=1&_HLS_part=9&keep=yes"

		// Video (MSN 100) and audio (MSN 700) have independent numbering.
		source := newReloadMedia(t, 100)
		appendSegs(t, source, 100, "v100.ts", "v101.ts")
		source.Segments[0].Parts = []Part{{URI: "v100.p0.m4s", Duration: 0.5}, {URI: "v100.p1.m4s", Duration: 0.5}}
		source.RenditionReports = []RenditionReport{{URI: "audio.m3u8?lang=zh&_HLS_msn=1&_HLS_part=9&keep=yes", LastMSN: u64ptr(700), LastPart: u64ptr(0)}}

		target := newReloadMedia(t, 700)
		target.CanBlockReload = true

		sync := NewReloadSync()
		c.Check(sync.Update(sourceURI, source) == nil, "Update source failed")
		c.Check(sync.Update(targetURI, target) == nil, "Update target failed")

		// Mutating the caller's objects after Update must not affect snapshots.
		source.RenditionReports = nil
		source.Segments[0].Parts = nil
		_ = source.Encode()                    // fill encoding cache
		source.Encode().WriteString("garbage") // mutate the cached buffer as well
		target.CanBlockReload = false
		target.SeqNo = 9999

		req, err := sync.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan failed: %v", err)
		if err != nil {
			return
		}
		c.Check(req.Token != 0, "token must be non-zero")
		c.Check(req.MSN == 700, "request MSN should come from the report (700), got %d", req.MSN)
		c.Check(req.Part != nil && *req.Part == 1, "explicit LAST-PART=0 should advance to part 1")

		q := mustURLQuery(t, req.URL)
		c.Check(q.Get("lang") == "zh", "target query lang should be kept, got %q", q.Get("lang"))
		c.Check(q.Get("keep") == "yes", "target query keep should be kept, got %q", q.Get("keep"))
		c.Check(q.Get("_HLS_msn") == "700", "URL should carry _HLS_msn=700, got %q", q.Get("_HLS_msn"))
		c.Check(q.Get("_HLS_part") == "1", "URL should carry _HLS_part=1, got %q", q.Get("_HLS_part"))
		c.Check(q.Get("_HLS_skip") == "", "Plan must not add _HLS_skip")

		// A second Plan for the same target is rejected while one is pending.
		_, err = sync.Plan(sourceURI, targetURI)
		c.Check(err != nil, "duplicate Plan for the same target should fail")

		// Abort releases the target so planning can be repeated.
		c.Check(sync.Abort(req.Token) == nil, "Abort of active token should succeed")
		c.Check(sync.Abort(req.Token) != nil, "Aborting an already aborted token should fail")
		req2, err := sync.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan after Abort failed: %v", err)
		c.Check(req2.Token != req.Token, "tokens must not be reused")
		if err == nil {
			c.Check(sync.Abort(req2.Token) == nil, "cleanup abort failed")
		}

		// Omitted LAST-PART derives the last part of that MSN, then advances one.
		source2 := newReloadMedia(t, 100)
		appendSegs(t, source2, 100, "v100.ts", "v101.ts")
		source2.Segments[1].Parts = []Part{{URI: "v101.p0.m4s"}, {URI: "v101.p1.m4s"}, {URI: "v101.p2.m4s"}}
		source2.RenditionReports = []RenditionReport{{URI: "audio.m3u8?lang=zh&_HLS_msn=1&_HLS_part=9&keep=yes", LastMSN: u64ptr(101)}}
		c.Check(sync.Update(sourceURI, source2) == nil, "Update source2 failed")
		syncTarget := newReloadMedia(t, 101)
		syncTarget.CanBlockReload = true
		c.Check(sync.Update(targetURI, syncTarget) == nil, "Update aligned target failed")
		req3, err := sync.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan with omitted LAST-PART failed: %v", err)
		if err == nil {
			c.Check(req3.MSN == 101, "derived request should stay on MSN 101, got %d", req3.MSN)
			c.Check(req3.Part != nil && *req3.Part == 3, "omitted LAST-PART should advance past last part index 2, got %v", formatPart(req3.Part))
			c.Check(sync.Abort(req3.Token) == nil, "cleanup abort failed")
		}

		// No LAST-PART, explicit LAST-MSN outside the source window: part stays nil,
		// so the request advances to the next MSN without _HLS_part.
		source3 := newReloadMedia(t, 100)
		appendSegs(t, source3, 100, "v100.ts")
		source3.RenditionReports = []RenditionReport{{URI: "audio.m3u8?lang=zh&_HLS_msn=1&_HLS_part=9&keep=yes", LastMSN: u64ptr(3700)}}
		c.Check(sync.Update(sourceURI, source3) == nil, "Update source3 failed")
		req4, err := sync.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan with out-of-window explicit MSN failed: %v", err)
		if err == nil {
			c.Check(req4.MSN == 3701, "should advance from explicit MSN, got %d", req4.MSN)
			c.Check(req4.Part == nil, "out-of-window MSN without LAST-PART must keep part nil")
			q := mustURLQuery(t, req4.URL)
			c.Check(q.Get("_HLS_msn") == "3701", "URL should carry advanced MSN, got %q", q.Get("_HLS_msn"))
			c.Check(q.Get("_HLS_part") == "", "stale _HLS_part must be removed when part is nil, got %q", q.Get("_HLS_part"))
			c.Check(q.Get("_HLS_skip") == "", "Plan must not add _HLS_skip")
			c.Check(sync.Abort(req4.Token) == nil, "cleanup abort failed")
		}

		// Target local position wins when it is further along the same track:
		// target has trailing parts -> (SeqNo+Count, len(TrailingParts)).
		source3b := newReloadMedia(t, 100)
		appendSegs(t, source3b, 100, "v100.ts")
		source3b.RenditionReports = []RenditionReport{{URI: "audio.m3u8?lang=zh&_HLS_msn=1&_HLS_part=9&keep=yes", LastMSN: u64ptr(700), LastPart: u64ptr(0)}}
		c.Check(sync.Update(sourceURI, source3b) == nil, "Update source3b failed")
		target2 := newReloadMedia(t, 700)
		target2.CanBlockReload = true
		target2.TrailingParts = []Part{{URI: "a0.m4s"}, {URI: "a1.m4s"}, {URI: "a2.m4s"}, {URI: "a3.m4s"}, {URI: "a4.m4s"}}
		c.Check(sync.Update(targetURI, target2) == nil, "Update target2 failed")
		req5, err := sync.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan with local trailing parts failed: %v", err)
		if err == nil {
			c.Check(req5.MSN == 700, "same-MSN merge should keep MSN 700, got %d", req5.MSN)
			c.Check(req5.Part != nil && *req5.Part == 5, "target local trailing parts (5) should beat report part 1, got %v", formatPart(req5.Part))
			c.Check(sync.Abort(req5.Token) == nil, "cleanup abort failed")
		}

		// Target local position without trailing parts is (SeqNo+Count, nil), which
		// beats any part of a lower MSN because the MSN is larger.
		target3 := newReloadMedia(t, 700)
		target3.CanBlockReload = true
		appendSegs(t, target3, 700, "a700.ts", "a701.ts")
		c.Check(sync.Update(targetURI, target3) == nil, "Update target3 failed")
		req5b, err := sync.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan with local complete segments failed: %v", err)
		if err == nil {
			c.Check(req5b.MSN == 702, "target local (SeqNo+Count) should win, got %d", req5b.MSN)
			c.Check(req5b.Part == nil, "target local position without trailing parts has nil part")
			c.Check(sync.Abort(req5b.Token) == nil, "cleanup abort failed")
		}

		// Within one MSN a nil part sorts before any concrete part.
		c.Check(comparePositions(10, nil, 10, u64ptr(0)) < 0, "nil part should sort before part 0")
		c.Check(comparePositions(10, u64ptr(4), 10, nil) > 0, "part 4 should sort after nil")
		c.Check(comparePositions(9, u64ptr(50), 10, nil) < 0, "lower MSN should sort first")

		// Ring window wrap: segment sequence order must follow the ring head,
		// not the physical slice order.
		ring, err := mustU(3, 5)
		c.Check(err == nil, "NewMediaPlaylist(3,5) failed: %v", err)
		if err == nil {
			ring.SeqNo = 100
			appendSegs(t, ring, 100, "s100.ts", "s101.ts", "s102.ts", "s103.ts", "s104.ts")
			ring.Slide("s105.ts", 1.0, "") // forces head to wrap around
			ring.Slide("s106.ts", 1.0, "")
			logical := ring.GetAllSegments()
			c.Check(len(logical) == 5, "ring should hold 5 segments, got %d", len(logical))
			if len(logical) == 5 {
				c.Check(logical[0].SeqId == 102 && logical[4].SeqId == 106, "GetAllSegments should follow ring order 102..106, got %d..%d", logical[0].SeqId, logical[4].SeqId)
			}
			// Put parts on the logically last segment only. Its physical slot is
			// head+4 (== tail-1), which is not the highest physical index, so a
			// derivation that follows slice order instead of the ring head would
			// pick the wrong segment.
			ring.Segments[(ring.head+4)%ring.capacity].Parts = []Part{{URI: "last.p0.m4s"}, {URI: "last.p1.m4s"}}
			ring.Segments[(ring.head+1)%ring.capacity].Parts = []Part{{URI: "early.p0.m4s"}}

			ring.RenditionReports = []RenditionReport{{URI: "wrap-audio.m3u8"}}

			wrapSource := "https://example.invalid/wrap.m3u8"
			wrapTarget := "https://example.invalid/wrap-audio.m3u8"
			wrapExt := newReloadMedia(t, 100)
			wrapExt.CanBlockReload = true
			c.Check(sync.Update(wrapSource, ring) == nil, "Update ring source failed")
			c.Check(sync.Update(wrapTarget, wrapExt) == nil, "Update ring target failed")
			reqR, err := sync.Plan(wrapSource, wrapTarget)
			c.Check(err == nil, "Plan for wrapped ring failed: %v", err)
			if err == nil {
				c.Check(reqR.MSN == 106, "derivation must pick the last logical segment 106, got %d", reqR.MSN)
				c.Check(reqR.Part != nil && *reqR.Part == 2, "derivation should advance past last part index 1, got %v", formatPart(reqR.Part))
				c.Check(sync.Abort(reqR.Token) == nil, "cleanup abort failed")
			}
		}

		// Omitted LAST-MSN derives the last MSN that has parts.
		sourceDerive := newReloadMedia(t, 100)
		appendSegs(t, sourceDerive, 100, "v100.ts", "v101.ts", "v102.ts")
		sourceDerive.Segments[0].Parts = []Part{{URI: "v100.p0.m4s"}}
		sourceDerive.Segments[1].Parts = []Part{{URI: "v101.p0.m4s"}, {URI: "v101.p1.m4s"}}
		sourceDerive.RenditionReports = []RenditionReport{{URI: "audio.m3u8?lang=zh&_HLS_msn=1&_HLS_part=9&keep=yes"}}
		deriveTarget := newReloadMedia(t, 100)
		deriveTarget.CanBlockReload = true
		c.Check(sync.Update(sourceURI, sourceDerive) == nil, "Update derive source failed")
		c.Check(sync.Update(targetURI, deriveTarget) == nil, "Update derive target failed")
		reqD, err := sync.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan with omitted LAST-MSN failed: %v", err)
		if err == nil {
			c.Check(reqD.MSN == 101, "omitted LAST-MSN should derive the last MSN with parts (101), got %d", reqD.MSN)
			c.Check(reqD.Part != nil && *reqD.Part == 2, "omitted LAST-PART should advance past last part of 101 (index 1) to 2, got %v", formatPart(reqD.Part))
			c.Check(sync.Abort(reqD.Token) == nil, "cleanup abort failed")
		}

		// Omitted LAST-MSN with trailing parts uses the trailing MSN, and
		// omitted LAST-PART uses the trailing part index.
		sourceTail := newReloadMedia(t, 100)
		appendSegs(t, sourceTail, 100, "v100.ts")
		sourceTail.TrailingParts = []Part{{URI: "t0.m4s"}, {URI: "t1.m4s"}, {URI: "t2.m4s"}}
		sourceTail.RenditionReports = []RenditionReport{{URI: "audio.m3u8?lang=zh&_HLS_msn=1&_HLS_part=9&keep=yes"}}
		tailTarget := newReloadMedia(t, 100)
		tailTarget.CanBlockReload = true
		c.Check(sync.Update(sourceURI, sourceTail) == nil, "Update tail source failed")
		c.Check(sync.Update(targetURI, tailTarget) == nil, "Update tail target failed")
		reqT, err := sync.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan with trailing-source derivation failed: %v", err)
		if err == nil {
			c.Check(reqT.MSN == 101, "trailing parts belong to MSN 101 (segment 100 + 1), got %d", reqT.MSN)
			c.Check(reqT.Part != nil && *reqT.Part == 3, "trailing part index should advance to 3, got %v", formatPart(reqT.Part))
			c.Check(sync.Abort(reqT.Token) == nil, "cleanup abort failed")
		}

		// A source with no media content cannot derive a position.
		emptySource := newReloadMedia(t, 100)
		emptySource.RenditionReports = []RenditionReport{{URI: "audio.m3u8?lang=zh&_HLS_msn=1&_HLS_part=9&keep=yes"}}
		emptyTarget := newReloadMedia(t, 100)
		emptyTarget.CanBlockReload = true
		c.Check(sync.Update(sourceURI, emptySource) == nil, "Update empty source failed")
		c.Check(sync.Update(targetURI, emptyTarget) == nil, "Update empty target failed")
		_, err = sync.Plan(sourceURI, targetURI)
		c.Check(err != nil, "Plan with no media content must fail")

		// Conflicts and missing reports otherwise.
		// Conflict: two relative spellings of the same target URL.
		source4 := newReloadMedia(t, 100)
		appendSegs(t, source4, 100, "v100.ts")
		baseTarget := "https://example.invalid/audio.m3u8"
		source4.RenditionReports = []RenditionReport{
			{URI: "audio.m3u8", LastMSN: u64ptr(700)},
			{URI: "./audio.m3u8", LastMSN: u64ptr(700)},
		}
		c.Check(sync.Update(sourceURI, source4) == nil, "Update conflict source failed")
		sync.Update(baseTarget, func() *MediaPlaylist {
			mm := newReloadMedia(t, 700)
			mm.CanBlockReload = true
			return mm
		}())
		_, err = sync.Plan(sourceURI, baseTarget)
		c.Check(err != nil, "two reports resolving to the same target must conflict")

		// Missing report, non-blocking target, missing snapshot.
		source5 := newReloadMedia(t, 100)
		appendSegs(t, source5, 100, "v100.ts")
		source5.RenditionReports = []RenditionReport{{URI: "other.m3u8"}}
		c.Check(sync.Update(sourceURI, source5) == nil, "Update source5 failed")
		_, err = sync.Plan(sourceURI, baseTarget)
		c.Check(err != nil, "Plan without a matching report must fail")
		_, err = sync.Plan("https://example.invalid/missing.m3u8", baseTarget)
		c.Check(err != nil, "Plan without a target snapshot must fail")

		nonBlockingTarget := "https://example.invalid/nonblock.m3u8"
		nonBlock := newReloadMedia(t, 700)
		nonBlock.CanBlockReload = false
		c.Check(sync.Update(nonBlockingTarget, nonBlock) == nil, "Update non-blocking target failed")
		source6 := newReloadMedia(t, 100)
		appendSegs(t, source6, 100, "v100.ts")
		source6.RenditionReports = []RenditionReport{{URI: "nonblock.m3u8", LastMSN: u64ptr(700)}}
		c.Check(sync.Update(sourceURI, source6) == nil, "Update source6 failed")
		_, err = sync.Plan(sourceURI, nonBlockingTarget)
		c.Check(err != nil, "Plan against a non-blocking target must fail")
		// Failed Plan leaves no pending request, so a later valid plan can proceed.
		nonBlock.CanBlockReload = true
		c.Check(sync.Update(nonBlockingTarget, nonBlock) == nil, "Update non-blocking target failed")
		_, err = sync.Plan(sourceURI, nonBlockingTarget)
		c.Check(err == nil, "Plan after enabling blocking failed: %v", err)

		// URI validation: relative and fragment-bearing URIs are rejected.
		c.Check(sync.Update("relative.m3u8", source6) != nil, "Update must reject a relative URI")
		c.Check(sync.Update("ftp://example.invalid/a.m3u8", source6) != nil, "Update must reject a non-http(s) URI")
		c.Check(sync.Update("https://example.invalid/a.m3u8#frag", source6) != nil, "Update must reject a fragment")
		_, err = sync.Plan("relative.m3u8", nonBlockingTarget)
		c.Check(err != nil, "Plan must reject a relative source URI")
		_, err = sync.Plan(sourceURI, "https://example.invalid/a.m3u8#frag")
		c.Check(err != nil, "Plan must reject a fragment target URI")
		c.Check(sync.Update(sourceURI, nil) != nil, "Update must reject a nil playlist")

		// Overflow in MSN advancement is reported and leaves no pending request.
		overflowSource := newReloadMedia(t, 100)
		appendSegs(t, overflowSource, 100, "v100.ts")
		overflowSource.RenditionReports = []RenditionReport{{URI: "overflow.m3u8", LastMSN: u64ptr(math.MaxUint64)}}
		overflowTarget := "https://example.invalid/overflow.m3u8"
		ot := newReloadMedia(t, 700)
		ot.CanBlockReload = true
		c.Check(sync.Update(sourceURI, overflowSource) == nil, "Update overflow source failed")
		c.Check(sync.Update(overflowTarget, ot) == nil, "Update overflow target failed")
		_, err = sync.Plan(sourceURI, overflowTarget)
		c.Check(err != nil, "Plan must reject media sequence overflow")
	})
}

func TestReloadCommit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const sourceURI = "https://example.invalid/video.m3u8"
		const targetURI = "https://example.invalid/audio.m3u8"

		makeSource := func(lastMSN *uint64, lastPart *uint64) *MediaPlaylist {
			s := newReloadMedia(t, 100)
			appendSegs(t, s, 100, "v100.ts", "v101.ts")
			s.Segments[0].Parts = []Part{{URI: "v100.p0.m4s"}, {URI: "v100.p1.m4s"}}
			s.RenditionReports = []RenditionReport{{URI: "audio.m3u8", LastMSN: lastMSN, LastPart: lastPart}}
			return s
		}
		makeTarget := func() *MediaPlaylist {
			tp := newReloadMedia(t, 700)
			tp.CanBlockReload = true
			return tp
		}

		setup := func(c *taskcheck.C) *ReloadSync {
			sync := NewReloadSync()
			c.Check(sync.Update(sourceURI, makeSource(u64ptr(700), u64ptr(0))) == nil, "Update source failed")
			c.Check(sync.Update(targetURI, makeTarget()) == nil, "Update target failed")
			return sync
		}

		// Success with a part: response publishes the requested trailing part.
		sync := setup(c)
		req, err := sync.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan failed: %v", err)
		if err != nil {
			return
		}
		c.Check(req.MSN == 700 && req.Part != nil && *req.Part == 1, "unexpected request position: msn=%d part=%v", req.MSN, formatPart(req.Part))
		partial := newReloadMedia(t, 700)
		partial.CanBlockReload = true
		partial.TrailingParts = []Part{{URI: "a0.m4s"}, {URI: "a1.m4s"}}
		c.Check(sync.Commit(req.Token, partial) == nil, "Commit with published trailing part failed: %v", err)
		c.Check(sync.Abort(req.Token) != nil, "committed token should no longer be active")
		c.Check(sync.Commit(req.Token, partial) != nil, "committing twice must fail")

		// After commit the target snapshot is the response; mutate it and ensure
		// the coordinator kept an independent copy.
		partial.TrailingParts = append(partial.TrailingParts, Part{URI: "a2.m4s"}, Part{URI: "a3.m4s"})
		partial.CanBlockReload = false
		_ = partial.Encode()
		reqNext, err := sync.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan after commit failed: %v", err)
		if err == nil {
			// Snapshot had 2 trailing parts -> local (700, 2); report next is also
			// (700,2), so the merge is a tie and both agree.
			c.Check(reqNext.MSN == 700 && reqNext.Part != nil && *reqNext.Part == 2, "commit snapshot must be deep-copied, got msn=%d part=%v", reqNext.MSN, formatPart(reqNext.Part))
			c.Check(sync.Abort(reqNext.Token) == nil, "cleanup abort failed")
		}

		// Success when the segment became complete, even for a part request.
		sync2 := setup(c)
		req2, err := sync2.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan failed: %v", err)
		if err == nil {
			done := newReloadMedia(t, 700)
			done.CanBlockReload = true
			appendSegs(t, done, 700, "a700.ts")
			c.Check(sync2.Commit(req2.Token, done) == nil, "Commit should accept a complete segment")
		}

		// Trailing part promoted to a complete segment: the same requested part 0
		// is now covered by the completed segment.
		sync2b := NewReloadSync()
		sync2bTarget := newReloadMedia(t, 700)
		sync2bTarget.CanBlockReload = true
		sync2bSource := newReloadMedia(t, 100)
		appendSegs(t, sync2bSource, 100, "v100.ts")
		// Report has an explicit LAST-MSN with no LAST-PART and source has no
		// part at 700, so the request advances to MSN 701 with no part. The
		// response publishes MSN 701 as a complete segment.
		sync2bSource.RenditionReports = []RenditionReport{{URI: "audio.m3u8", LastMSN: u64ptr(700)}}
		c.Check(sync2b.Update(sourceURI, sync2bSource) == nil, "Update src2b failed")
		c.Check(sync2b.Update(targetURI, sync2bTarget) == nil, "Update target2b failed")
		req2b, err := sync2b.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan 2b failed: %v", err)
		if err == nil {
			c.Check(req2b.MSN == 701 && req2b.Part == nil, "2b should request MSN 701 without a part")
			promoted := newReloadMedia(t, 701)
			promoted.CanBlockReload = true
			appendSegs(t, promoted, 701, "a701.ts")
			c.Check(sync2b.Commit(req2b.Token, promoted) == nil, "Commit should accept the promoted complete segment")
		}

		// Reported trailing parts survive encode/reparse, and the parsed copy can
		// continue planning.
		sync2c := NewReloadSync()
		src2c := newReloadMedia(t, 100)
		appendSegs(t, src2c, 100, "v100.ts")
		src2c.TrailingParts = []Part{{URI: "v101.p0.m4s", Duration: 0.5}, {URI: "v101.p1.m4s", Duration: 0.5}}
		src2c.RenditionReports = []RenditionReport{{URI: "audio.m3u8"}}
		encoded2c := src2c.Encode().String()
		c.Check(strings.Contains(encoded2c, "#EXT-X-PART:"), "encoded source should contain parts:\n%s", encoded2c)
		reparsed2c, err := decodeReloadMedia(t, encoded2c, true)
		c.Check(err == nil, "reparse for planning failed: %v", err)
		if err == nil {
			c.Check(len(reparsed2c.TrailingParts) == 2, "reparsed trailing parts mismatch: %d", len(reparsed2c.TrailingParts))
			c.Check(len(reparsed2c.RenditionReports) == 1, "reparsed reports mismatch: %d", len(reparsed2c.RenditionReports))
			tgt2c := newReloadMedia(t, 100)
			tgt2c.CanBlockReload = true
			c.Check(sync2c.Update(sourceURI, reparsed2c) == nil, "Update reparsed source failed")
			c.Check(sync2c.Update(targetURI, tgt2c) == nil, "Update 2c target failed")
			req2c, err := sync2c.Plan(sourceURI, targetURI)
			c.Check(err == nil, "Plan from reparsed source failed: %v", err)
			if err == nil {
				c.Check(req2c.MSN == 101 && req2c.Part != nil && *req2c.Part == 2, "reparsed plan should request MSN 101 part 2, got msn=%d part=%v", req2c.MSN, formatPart(req2c.Part))
				c.Check(sync2c.Abort(req2c.Token) == nil, "cleanup abort failed")
			}
		}

		// A target whose local parts already exceed the reported position is
		// requested at max(report next, local next).
		sync2d := NewReloadSync()
		src2d := newReloadMedia(t, 100)
		appendSegs(t, src2d, 100, "v100.ts")
		src2d.RenditionReports = []RenditionReport{{URI: "audio.m3u8", LastMSN: u64ptr(700), LastPart: u64ptr(0)}}
		tgt2d := newReloadMedia(t, 700)
		tgt2d.CanBlockReload = true
		tgt2d.TrailingParts = []Part{{URI: "a0.m4s"}}
		c.Check(sync2d.Update(sourceURI, src2d) == nil, "Update src2d failed")
		c.Check(sync2d.Update(targetURI, tgt2d) == nil, "Update target2d failed")
		req2d, err := sync2d.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan 2d failed: %v", err)
		if err == nil {
			c.Check(req2d.MSN == 700 && req2d.Part != nil && *req2d.Part == 1, "spec example: source part 0 -> next 1, got msn=%d part=%v", req2d.MSN, formatPart(req2d.Part))
			c.Check(sync2d.Abort(req2d.Token) == nil, "cleanup abort failed")
		}

		// Error response / missing part: snapshot and pending stay untouched, and
		// the same token can later succeed.
		sync3 := setup(c)
		req3, err := sync3.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan failed: %v", err)
		if err == nil {
			short := newReloadMedia(t, 700)
			short.CanBlockReload = true
			short.TrailingParts = []Part{{URI: "a0.m4s"}} // only part 0 published, part 1 requested
			c.Check(sync3.Commit(req3.Token, short) != nil, "Commit should reject an unpublished part")
			// Still pending: planning again must fail.
			_, err = sync3.Plan(sourceURI, targetURI)
			c.Check(err != nil, "failed commit must keep the request pending")
			// Same token succeeds once the position appears.
			short.TrailingParts = append(short.TrailingParts, Part{URI: "a1.m4s"})
			c.Check(sync3.Commit(req3.Token, short) == nil, "retry with the same token should succeed: %v", err)
		}

		// Window slid past the requested position without proving it arrived.
		sync4 := setup(c)
		req4, err := sync4.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan failed: %v", err)
		if err == nil {
			slidPast := newReloadMedia(t, 800)
			slidPast.CanBlockReload = true
			appendSegs(t, slidPast, 800, "a800.ts")
			c.Check(sync4.Commit(req4.Token, slidPast) != nil, "Commit should reject a window that merely slid past")
			c.Check(sync4.Abort(req4.Token) == nil, "abort after rejection failed")
		}

		// No-part request: LAST-MSN=700 has no parts in the source window, so the
		// report stays part-less and advances to 701 without a part.
		sync5 := NewReloadSync()
		c.Check(sync5.Update(sourceURI, makeSource(u64ptr(700), nil)) == nil, "Update source failed")
		c.Check(sync5.Update(targetURI, makeTarget()) == nil, "Update target failed")
		req5, err := sync5.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan failed: %v", err)
		if err == nil {
			c.Check(req5.MSN == 701 && req5.Part == nil, "no-part request should be msn=701,nil, got msn=%d part=%v", req5.MSN, formatPart(req5.Part))
			withPartOnly := newReloadMedia(t, 701)
			withPartOnly.CanBlockReload = true
			withPartOnly.TrailingParts = []Part{{URI: "b0.m4s"}}
			c.Check(sync5.Commit(req5.Token, withPartOnly) == nil, "Commit should accept an MSN with a published part")
		}

		// Staleness: a source Update invalidates the old token; Abort of the stale
		// token is allowed, and the old token cannot cancel the new request.
		sync6 := setup(c)
		req6, err := sync6.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan failed: %v", err)
		if err != nil {
			return
		}
		c.Check(sync6.Update(sourceURI, makeSource(u64ptr(700), u64ptr(0))) == nil, "Update source again failed")
		staleResp := newReloadMedia(t, 700)
		staleResp.CanBlockReload = true
		staleResp.TrailingParts = []Part{{URI: "a0.m4s"}, {URI: "a1.m4s"}}
		c.Check(sync6.Commit(req6.Token, staleResp) != nil, "Commit with a stale token must fail")
		c.Check(sync6.Abort(req6.Token) == nil, "stale token should be abortable")
		req7, err := sync6.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan after aborting stale token failed: %v", err)
		if err == nil {
			c.Check(req7.Token != req6.Token, "retry must use a new token")
			c.Check(sync6.Abort(req6.Token) != nil, "old token must not cancel the new request")
			c.Check(sync6.Abort(req7.Token) == nil, "aborting the new request failed")
		}

		// A target Update also makes outstanding tokens stale.
		sync7 := setup(c)
		req8, err := sync7.Plan(sourceURI, targetURI)
		c.Check(err == nil, "Plan failed: %v", err)
		if err == nil {
			c.Check(sync7.Update(targetURI, makeTarget()) == nil, "Update target again failed")
			resp := newReloadMedia(t, 700)
			resp.CanBlockReload = true
			resp.TrailingParts = []Part{{URI: "a0.m4s"}, {URI: "a1.m4s"}}
			c.Check(sync7.Commit(req8.Token, resp) != nil, "Commit after a target Update must be stale")
		}

		// Unknown token handling.
		sync8 := NewReloadSync()
		c.Check(sync8.Commit(42, newReloadMedia(t, 1)) != nil, "Commit with an unknown token must fail")
		c.Check(sync8.Abort(42) != nil, "Abort with an unknown token must fail")
	})
}
