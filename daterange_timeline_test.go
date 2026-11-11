package m3u8

import (
	"strings"
	"testing"
	"time"
)

func drTime(sec int) time.Time {
	return time.Date(2026, 1, 1, 0, 0, sec, 0, time.UTC)
}

func decodeMedia(t *testing.T, text string) *MediaPlaylist {
	t.Helper()
	raw, kind, err := DecodeFrom(strings.NewReader(text), true)
	if err != nil {
		t.Fatalf("decode failed: %v", err)
	}
	if kind != MEDIA {
		t.Fatalf("expected media playlist, got %v", kind)
	}
	return raw.(*MediaPlaylist)
}

func mustEnd(t *testing.T, p *MediaPlaylist, id string, want time.Time) {
	t.Helper()
	dr, ok := p.DateRange(id)
	if !ok {
		t.Fatalf("daterange %q not found", id)
	}
	if !dr.EndDateSet {
		t.Fatalf("daterange %q: end unknown, want %s", id, want)
	}
	if !dr.EndDate.Equal(want) {
		t.Fatalf("daterange %q: end = %s, want %s", id, dr.EndDate, want)
	}
}

func mustEndUnknown(t *testing.T, p *MediaPlaylist, id string) {
	t.Helper()
	dr, ok := p.DateRange(id)
	if !ok {
		t.Fatalf("daterange %q not found", id)
	}
	if dr.EndDateSet {
		t.Fatalf("daterange %q: end = %s, want unknown", id, dr.EndDate)
	}
}

func dateRangeLine(t *testing.T, out, id string) string {
	t.Helper()
	needle := "#EXT-X-DATERANGE:ID=\"" + id + "\""
	for _, line := range strings.Split(out, "\n") {
		if strings.HasPrefix(line, needle) {
			return line
		}
	}
	t.Fatalf("no daterange line for %q in output:\n%s", id, out)
	return ""
}

func TestDateRangeTimeline(t *testing.T) {
	t.Run("EndOnNextTimeline", func(t *testing.T) {
		// Input order: b (00:00:20Z), a (00:00:00Z, END-ON-NEXT=YES),
		// with an unrelated range of another class at 00:00:05Z in between.
		text := "#EXTM3U\n" +
			"#EXT-X-VERSION:3\n" +
			"#EXT-X-TARGETDURATION:6\n" +
			"#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00Z\n" +
			"#EXT-X-DATERANGE:ID=\"b\",CLASS=\"break\",START-DATE=\"2026-01-01T00:00:20Z\"\n" +
			"#EXT-X-DATERANGE:ID=\"a\",CLASS=\"break\",START-DATE=\"2026-01-01T00:00:00Z\",END-ON-NEXT=YES\n" +
			"#EXT-X-DATERANGE:ID=\"x\",CLASS=\"other\",START-DATE=\"2026-01-01T00:00:05Z\"\n" +
			"#EXTINF:6,\nseg0.ts\n#EXT-X-ENDLIST\n"
		p := decodeMedia(t, text)

		// The other-class range at 5s must not truncate a; the nearest
		// later range of the same class is b at 20s.
		mustEnd(t, p, "a", drTime(20))
		mustEndUnknown(t, p, "b")
		mustEndUnknown(t, p, "x")

		// The derived END-DATE must not leak into the raw attributes.
		out := p.Encode().String()
		if line := dateRangeLine(t, out, "a"); strings.Contains(line, "END-DATE") {
			t.Fatalf("derived END-DATE written to raw attributes: %s", line)
		}
		if line := dateRangeLine(t, out, "a"); !strings.Contains(line, "END-ON-NEXT=YES") {
			t.Fatalf("END-ON-NEXT missing in output: %s", line)
		}

		// A successor appearing later moves the effective end of a.
		err := p.SetDateRange(&DateRange{ID: "c", Class: "break", StartDate: drTime(10)})
		if err != nil {
			t.Fatalf("SetDateRange c: %v", err)
		}
		mustEnd(t, p, "a", drTime(10))
		mustEndUnknown(t, p, "b")

		// Encode was already called above; the update must refresh the output.
		out = p.Encode().String()
		dateRangeLine(t, out, "c")
		// Stable order of first appearance: b, a, x, c.
		ib := strings.Index(out, "ID=\"b\"")
		ia := strings.Index(out, "ID=\"a\"")
		ix := strings.Index(out, "ID=\"x\"")
		ic := strings.Index(out, "ID=\"c\"")
		if !(ib < ia && ia < ix && ix < ic) {
			t.Fatalf("dateranges not in first-seen order: %d %d %d %d\n%s", ib, ia, ix, ic, out)
		}

		// A rule-violating update (DURATION on an END-ON-NEXT range)
		// fails and leaves no partial attributes behind.
		err = p.SetDateRange(&DateRange{ID: "a", DurationSet: true, Duration: 5})
		if err == nil {
			t.Fatal("expected error for DURATION on END-ON-NEXT range")
		}
		drErr, ok := err.(*DateRangeError)
		if !ok {
			t.Fatalf("error type = %T, want *DateRangeError", err)
		}
		if drErr.ID != "a" || drErr.Attribute != "END-ON-NEXT" {
			t.Fatalf("error carries %q/%q, want a/END-ON-NEXT", drErr.ID, drErr.Attribute)
		}
		// Query again after the failed update: state must be unchanged.
		mustEnd(t, p, "a", drTime(10))
		dr, _ := p.DateRange("a")
		if dr.DurationSet {
			t.Fatal("failed update left DURATION behind")
		}

		// A conflicting START-DATE for a known ID is rejected too.
		err = p.SetDateRange(&DateRange{ID: "b", Class: "break", StartDate: drTime(21)})
		if err == nil {
			t.Fatal("expected conflict error for START-DATE")
		}
		if drErr, ok := err.(*DateRangeError); !ok || drErr.ID != "b" || drErr.Attribute != "START-DATE" {
			t.Fatalf("unexpected conflict error: %v", err)
		}
		mustEndUnknown(t, p, "b")
		dr, _ = p.DateRange("b")
		if !dr.StartDate.Equal(drTime(20)) {
			t.Fatalf("b start moved to %s after failed update", dr.StartDate)
		}
	})

	t.Run("MergeSameID", func(t *testing.T) {
		text := "#EXTM3U\n" +
			"#EXT-X-VERSION:3\n" +
			"#EXT-X-TARGETDURATION:6\n" +
			"#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00Z\n" +
			"#EXT-X-DATERANGE:ID=\"ad\",CLASS=\"break\",START-DATE=\"2026-01-01T00:00:00Z\",END-DATE=\"2026-01-01T00:00:30Z\"\n" +
			"#EXT-X-DATERANGE:ID=\"ad\",X-COMMENT=\"hello, world\",X-INDEX=2,X-VER=\"2\",X-RAW=0xABCD\n" +
			"#EXTINF:6,\nseg0.ts\n#EXT-X-ENDLIST\n"
		p := decodeMedia(t, text)

		dr, ok := p.DateRange("ad")
		if !ok {
			t.Fatal("daterange ad not found")
		}
		if !dr.EndDateSet || !dr.EndDate.Equal(drTime(30)) {
			t.Fatalf("ad end = %v %v, want 30s", dr.EndDate, dr.EndDateSet)
		}
		if len(dr.X) != 4 {
			t.Fatalf("ad has %d X- attributes, want 4: %+v", len(dr.X), dr.X)
		}
		x := make(map[string]DateRangeAttribute)
		for _, xa := range dr.X {
			x[xa.Name] = xa
		}
		if xa := x["X-COMMENT"]; xa.Value != "hello, world" || xa.Type != DateRangeAttributeString {
			t.Fatalf("X-COMMENT = %+v, want quoted string \"hello, world\"", xa)
		}
		if xa := x["X-INDEX"]; xa.Value != "2" || xa.Type != DateRangeAttributeNumber {
			t.Fatalf("X-INDEX = %+v, want number 2", xa)
		}
		if xa := x["X-VER"]; xa.Value != "2" || xa.Type != DateRangeAttributeString {
			t.Fatalf("X-VER = %+v, want string \"2\"", xa)
		}
		if xa := x["X-RAW"]; xa.Value != "0xABCD" || xa.Type != DateRangeAttributeHex {
			t.Fatalf("X-RAW = %+v, want hex 0xABCD", xa)
		}

		// Client attributes are written back with their original types.
		out := p.Encode().String()
		line := dateRangeLine(t, out, "ad")
		for _, want := range []string{`X-COMMENT="hello, world"`, "X-INDEX=2", `X-VER="2"`, "X-RAW=0xABCD"} {
			if !strings.Contains(line, want) {
				t.Fatalf("encoded line misses %s: %s", want, line)
			}
		}

		// Write-read twice: both declarations merge into a single range
		// and no attribute is lost or duplicated.
		p2 := decodeMedia(t, out)
		out2 := p2.Encode().String()
		if n := strings.Count(out2, "#EXT-X-DATERANGE:ID=\"ad\""); n != 1 {
			t.Fatalf("daterange ad encoded %d times:\n%s", n, out2)
		}
		dr2, _ := p2.DateRange("ad")
		if !dr2.EndDateSet || !dr2.EndDate.Equal(drTime(30)) || len(dr2.X) != 4 {
			t.Fatalf("roundtrip lost attributes: %+v", dr2)
		}
		if out != out2 {
			t.Fatalf("repeated encode-decode not stable:\n%s\n---\n%s", out, out2)
		}

		// A conflicting DURATION against the known END-DATE is rejected
		// and the stored range stays intact.
		err := p.SetDateRange(&DateRange{ID: "ad", DurationSet: true, Duration: 45})
		if err == nil {
			t.Fatal("expected error for conflicting DURATION")
		}
		if drErr, ok := err.(*DateRangeError); !ok || drErr.ID != "ad" || drErr.Attribute != "DURATION" {
			t.Fatalf("unexpected error: %v", err)
		}
		dr, _ = p.DateRange("ad")
		if dr.DurationSet {
			t.Fatal("failed update left DURATION behind")
		}
		if !dr.EndDateSet || !dr.EndDate.Equal(drTime(30)) {
			t.Fatalf("ad end changed after failed update: %v", dr.EndDate)
		}
	})

	t.Run("ZeroAndPlannedDuration", func(t *testing.T) {
		p, err := NewMediaPlaylist(1, 8)
		if err != nil {
			t.Fatal(err)
		}
		// An explicit zero DURATION is a known end equal to the start.
		if err = p.SetDateRange(&DateRange{ID: "z", StartDate: drTime(0), DurationSet: true, Duration: 0}); err != nil {
			t.Fatalf("SetDateRange z: %v", err)
		}
		mustEnd(t, p, "z", drTime(0))
		// PLANNED-DURATION alone leaves the end unknown.
		if err = p.SetDateRange(&DateRange{ID: "p", StartDate: drTime(0), PlannedDurationSet: true, PlannedDuration: 30}); err != nil {
			t.Fatalf("SetDateRange p: %v", err)
		}
		mustEndUnknown(t, p, "p")
		// No duration at all differs from an explicit zero.
		if err = p.SetDateRange(&DateRange{ID: "n", StartDate: drTime(0)}); err != nil {
			t.Fatalf("SetDateRange n: %v", err)
		}
		dr, _ := p.DateRange("n")
		if dr.DurationSet {
			t.Fatal("unset DURATION reported as set")
		}
		mustEndUnknown(t, p, "n")
		// The explicit zero survives encoding.
		if line := dateRangeLine(t, p.Encode().String(), "z"); !strings.Contains(line, "DURATION=0") {
			t.Fatalf("explicit zero DURATION not encoded: %s", line)
		}
	})

	t.Run("Validation", func(t *testing.T) {
		// A strictly parsed playlist with dateranges needs at least one
		// EXT-X-PROGRAM-DATE-TIME.
		noPDT := "#EXTM3U\n#EXT-X-TARGETDURATION:6\n" +
			"#EXT-X-DATERANGE:ID=\"a\",START-DATE=\"2026-01-01T00:00:00Z\"\n" +
			"#EXTINF:6,\nseg0.ts\n"
		if _, _, err := DecodeFrom(strings.NewReader(noPDT), true); err == nil {
			t.Fatal("expected error for daterange without PROGRAM-DATE-TIME")
		}
		// The first occurrence of an ID must carry START-DATE.
		noStart := "#EXTM3U\n#EXT-X-TARGETDURATION:6\n" +
			"#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00Z\n" +
			"#EXT-X-DATERANGE:ID=\"a\",CLASS=\"break\"\n" +
			"#EXTINF:6,\nseg0.ts\n"
		if _, _, err := DecodeFrom(strings.NewReader(noStart), true); err == nil {
			t.Fatal("expected error for first occurrence without START-DATE")
		}

		p, err := NewMediaPlaylist(1, 8)
		if err != nil {
			t.Fatal(err)
		}
		cases := []struct {
			name string
			dr   *DateRange
			attr string
		}{
			{"EndOnNextWithoutClass", &DateRange{ID: "e1", StartDate: drTime(0), EndOnNext: true}, "END-ON-NEXT"},
			{"EndOnNextWithEndDate", &DateRange{ID: "e2", Class: "c", StartDate: drTime(0), EndOnNext: true, EndDateSet: true, EndDate: drTime(10)}, "END-ON-NEXT"},
			{"EndOnNextWithDuration", &DateRange{ID: "e3", Class: "c", StartDate: drTime(0), EndOnNext: true, DurationSet: true, Duration: 10}, "END-ON-NEXT"},
			{"NegativeDuration", &DateRange{ID: "e4", StartDate: drTime(0), DurationSet: true, Duration: -1}, "DURATION"},
			{"EndBeforeStart", &DateRange{ID: "e5", StartDate: drTime(10), EndDateSet: true, EndDate: drTime(5)}, "END-DATE"},
			{"InconsistentEndAndDuration", &DateRange{ID: "e6", StartDate: drTime(0), EndDateSet: true, EndDate: drTime(30), DurationSet: true, Duration: 20}, "DURATION"},
			{"MissingStartDate", &DateRange{ID: "e7", Class: "c"}, "START-DATE"},
		}
		for _, c := range cases {
			err := p.SetDateRange(c.dr)
			if err == nil {
				t.Fatalf("%s: expected error", c.name)
			}
			drErr, ok := err.(*DateRangeError)
			if !ok {
				t.Fatalf("%s: error type = %T, want *DateRangeError", c.name, err)
			}
			if drErr.ID != c.dr.ID || drErr.Attribute != c.attr {
				t.Fatalf("%s: error carries %q/%q, want %q/%q", c.name, drErr.ID, drErr.Attribute, c.dr.ID, c.attr)
			}
			if _, found := p.DateRange(c.dr.ID); found {
				t.Fatalf("%s: rejected update left a range behind", c.name)
			}
		}
		// A consistent END-DATE plus DURATION pair is accepted.
		if err := p.SetDateRange(&DateRange{ID: "ok", StartDate: drTime(0), EndDateSet: true, EndDate: drTime(30), DurationSet: true, Duration: 30}); err != nil {
			t.Fatalf("consistent range rejected: %v", err)
		}
		mustEnd(t, p, "ok", drTime(30))
	})

	t.Run("PlaylistFeaturesSurvive", func(t *testing.T) {
		text := "#EXTM3U\n" +
			"#EXT-X-VERSION:5\n" +
			"#EXT-X-TARGETDURATION:6\n" +
			"#EXT-X-MEDIA-SEQUENCE:7\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"https://example.com/key.bin\"\n" +
			"#EXT-X-MAP:URI=\"init.mp4\",BYTERANGE=720@0\n" +
			"#EXT-X-DATERANGE:ID=\"ad\",CLASS=\"break\",START-DATE=\"2026-01-01T00:00:00Z\",DURATION=30\n" +
			"#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00Z\n" +
			"#EXT-X-CUE-OUT:30\n" +
			"#EXTINF:6,\nseg7.ts\n" +
			"#EXT-X-DISCONTINUITY\n" +
			"#EXT-X-BYTERANGE:1000@0\n" +
			"#EXTINF:6,\nseg8.ts\n" +
			"#EXT-X-ENDLIST\n"
		p := decodeMedia(t, text)
		out := p.Encode().String()
		p2 := decodeMedia(t, out)

		if p2.Count() != 2 || p2.Segments[0].URI != "seg7.ts" || p2.Segments[1].URI != "seg8.ts" {
			t.Fatalf("segments lost: %+v", p2.Segments)
		}
		if p2.Segments[0].ProgramDateTime.IsZero() {
			t.Fatal("PROGRAM-DATE-TIME lost")
		}
		if p2.Key == nil || p2.Key.URI != "https://example.com/key.bin" {
			t.Fatal("EXT-X-KEY lost")
		}
		if p2.Map == nil || p2.Map.URI != "init.mp4" || p2.Map.Limit != 720 {
			t.Fatal("EXT-X-MAP lost")
		}
		if !p2.Segments[1].Discontinuity {
			t.Fatal("EXT-X-DISCONTINUITY lost")
		}
		if p2.Segments[0].SCTE == nil || p2.Segments[0].SCTE.Time != 30 {
			t.Fatal("SCTE cue lost")
		}
		if p2.Segments[1].Limit != 1000 {
			t.Fatal("EXT-X-BYTERANGE lost")
		}
		// DURATION gives a known end; the range survives the roundtrip once.
		mustEnd(t, p2, "ad", drTime(30))
		if n := strings.Count(p2.Encode().String(), "#EXT-X-DATERANGE:"); n != 1 {
			t.Fatalf("daterange duplicated after roundtrip: %d", n)
		}
	})
}
