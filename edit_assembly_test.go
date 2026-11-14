package astisub_test

import (
	"bytes"
	"encoding/json"
	"io/ioutil"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/asticode/go-astisub"
	"github.com/asticode/go-astisub/task_support/taskcheck"
)

func TestEditAssemblyTimeline(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A single source event exists between 1s and 5s.
		source := astisub.NewSubtitles()
		source.Items = []*astisub.Item{{
			StartAt: time.Second,
			EndAt:   5 * time.Second,
			Lines:   []astisub.Line{{Items: []astisub.LineItem{{Text: "hello"}}}},
		}}

		// Take [3s,6s) for 6s, then [0s,2s) for 1s. The example from the spec
		// expects [0s,4s) and [6.5s,7s), not one subtitle spanning both clips.
		out, err := astisub.Assemble([]astisub.EditClip{
			{Source: source, In: 3 * time.Second, Out: 6 * time.Second, Duration: 6 * time.Second},
			{Source: source, In: 0, Out: 2 * time.Second, Duration: time.Second},
		})
		c.Check(err == nil, "assemble returned %v", err)
		c.Check(out != nil, "assemble returned nil subtitles")
		if out == nil {
			return
		}
		c.Check(len(out.Items) == 2, "expected 2 items, got %d", len(out.Items))
		if len(out.Items) != 2 {
			return
		}
		c.Check(out.Items[0].StartAt == 0, "first start = %s, want 0", out.Items[0].StartAt)
		c.Check(out.Items[0].EndAt == 4*time.Second, "first end = %s, want 4s", out.Items[0].EndAt)
		c.Check(out.Items[1].StartAt == 6500*time.Millisecond, "second start = %s, want 6.5s", out.Items[1].StartAt)
		c.Check(out.Items[1].EndAt == 7*time.Second, "second end = %s, want 7s", out.Items[1].EndAt)
		c.Check(out.Items[0].Index == 1 && out.Items[1].Index == 2, "indices = %d,%d", out.Items[0].Index, out.Items[1].Index)
		c.Check(out.Items[0].Lines[0].Items[0].Text == "hello", "first text = %q", out.Items[0].Lines[0].Items[0].Text)

		// The input must not be modified.
		c.Check(len(source.Items) == 1 && source.Items[0].StartAt == time.Second && source.Items[0].EndAt == 5*time.Second,
			"input mutated: %#v", source.Items[0])

		// Reverse-order selection and slow motion on the same source: [4s,5s)
		// slowed to 10s, then [1s,2s) at normal speed.
		slow, err := astisub.Assemble([]astisub.EditClip{
			{Source: source, In: 4 * time.Second, Out: 5 * time.Second, Duration: 10 * time.Second},
			{Source: source, In: 1 * time.Second, Out: 2 * time.Second, Duration: time.Second},
		})
		c.Check(err == nil, "slow assemble returned %v", err)
		if slow != nil {
			c.Check(len(slow.Items) == 2, "slow: expected 2 items, got %d", len(slow.Items))
			if len(slow.Items) == 2 {
				c.Check(slow.Items[0].StartAt == 0 && slow.Items[0].EndAt == 10*time.Second,
					"slow first = [%s,%s), want [0s,10s)", slow.Items[0].StartAt, slow.Items[0].EndAt)
				c.Check(slow.Items[1].StartAt == 10*time.Second && slow.Items[1].EndAt == 11*time.Second,
					"slow second = [%s,%s), want [10s,11s)", slow.Items[1].StartAt, slow.Items[1].EndAt)
			}
		}

		// Quantization floors at the nanosecond and never accumulates the
		// rounded mini-lengths: a 1ns clip length over a 3ns source range.
		tiny, err := astisub.Assemble([]astisub.EditClip{
			{Source: source, In: time.Second, Out: time.Second + 3, Duration: time.Second},
		})
		c.Check(err == nil, "tiny assemble returned %v", err)
		if tiny != nil {
			c.Check(len(tiny.Items) == 1, "tiny: expected 1 item, got %d", len(tiny.Items))
			if len(tiny.Items) == 1 {
				// Both source endpoints map through a one-second clip length.
				c.Check(tiny.Items[0].StartAt == 0, "tiny start = %s", tiny.Items[0].StartAt)
				c.Check(tiny.Items[0].EndAt == time.Second, "tiny end = %s", tiny.Items[0].EndAt)
			}
		}

		// Overlapping subtitles are kept and ordered stably: by output start,
		// then clip order, then the source's original item order; numbering is
		// contiguous afterwards.
		overlapping := astisub.NewSubtitles()
		overlapping.Items = []*astisub.Item{
			{StartAt: 0, EndAt: 4 * time.Second, Lines: []astisub.Line{{Items: []astisub.LineItem{{Text: "a"}}}}},
			{StartAt: 0, EndAt: 4 * time.Second, Lines: []astisub.Line{{Items: []astisub.LineItem{{Text: "b"}}}}},
		}
		out2, err := astisub.Assemble([]astisub.EditClip{
			{Source: overlapping, In: 0, Out: 4 * time.Second, Duration: 2 * time.Second},
			{Source: overlapping, In: 0, Out: 4 * time.Second, Duration: 2 * time.Second},
		})
		c.Check(err == nil, "overlap assemble returned %v", err)
		if err == nil {
			c.Check(len(out2.Items) == 4, "overlap: expected 4 items, got %d", len(out2.Items))
			if len(out2.Items) == 4 {
				// Clip 1 contributes a then b at [0s,2s); clip 2 contributes a
				// then b at [2s,4s).
				c.Check(out2.Items[0].Lines[0].String() == "a", "overlap[0] = %q", out2.Items[0].Lines[0].String())
				c.Check(out2.Items[1].Lines[0].String() == "b", "overlap[1] = %q", out2.Items[1].Lines[0].String())
				c.Check(out2.Items[2].Lines[0].String() == "a", "overlap[2] = %q", out2.Items[2].Lines[0].String())
				c.Check(out2.Items[3].Lines[0].String() == "b", "overlap[3] = %q", out2.Items[3].Lines[0].String())
				c.Check(out2.Items[0].StartAt == 0 && out2.Items[1].StartAt == 0, "overlap starts should tie")
				c.Check(out2.Items[2].StartAt == 2*time.Second && out2.Items[3].StartAt == 2*time.Second, "overlap second clip starts wrong")
				c.Check(out2.Items[0].Index == 1 && out2.Items[3].Index == 4,
					"overlap indices = %d..%d", out2.Items[0].Index, out2.Items[3].Index)
			}
		}

		// Empty clip table yields a usable empty subtitle track.
		empty, err := astisub.Assemble(nil)
		c.Check(err == nil && empty != nil, "empty assemble: %v, %#v", err, empty)
		if empty != nil {
			c.Check(len(empty.Items) == 0 && empty.Styles != nil && empty.Regions != nil,
				"empty assemble is not usable: %#v", empty)
		}

		// Error cases all return a nil result.
		checks := []struct {
			name  string
			clips []astisub.EditClip
		}{
			{"nil source", []astisub.EditClip{{Source: nil, In: 0, Out: time.Second, Duration: time.Second}}},
			{"negative in", []astisub.EditClip{{Source: source, In: -time.Second, Out: time.Second, Duration: time.Second}}},
			{"out equals in", []astisub.EditClip{{Source: source, In: time.Second, Out: time.Second, Duration: time.Second}}},
			{"out before in", []astisub.EditClip{{Source: source, In: 2 * time.Second, Out: time.Second, Duration: time.Second}}},
			{"zero duration", []astisub.EditClip{{Source: source, In: 0, Out: time.Second, Duration: 0}}},
			{"negative duration", []astisub.EditClip{{Source: source, In: 0, Out: time.Second, Duration: -time.Second}}},
			{"overflow", []astisub.EditClip{
				{Source: source, In: 0, Out: time.Second, Duration: time.Duration(1 << 62)},
				{Source: source, In: 0, Out: time.Second, Duration: time.Duration(1 << 62)},
				{Source: source, In: 0, Out: time.Second, Duration: time.Duration(1 << 62)},
			}},
		}
		for _, check := range checks {
			result, err := astisub.Assemble(check.clips)
			c.Check(err != nil && result == nil, "%s: got %v, %#v", check.name, err, result)
		}

		// Inverted event time returns an error and nil.
		inverted := astisub.NewSubtitles()
		inverted.Items = []*astisub.Item{{StartAt: 3 * time.Second, EndAt: 2 * time.Second, Lines: []astisub.Line{{Items: []astisub.LineItem{{Text: "bad"}}}}}}
		result, err := astisub.Assemble([]astisub.EditClip{{Source: inverted, In: 0, Out: 5 * time.Second, Duration: time.Second}})
		c.Check(err != nil && result == nil, "inverted event: got %v, %#v", err, result)

		// A non-zero word start outside the event returns an error and nil.
		badWords := []*astisub.Item{
			{StartAt: time.Second, EndAt: 3 * time.Second, Lines: []astisub.Line{{Items: []astisub.LineItem{
				{Text: "a"},
				{StartAt: 4 * time.Second, Text: "b"},
			}}}},
		}
		badSource := astisub.NewSubtitles()
		badSource.Items = badWords
		result, err = astisub.Assemble([]astisub.EditClip{{Source: badSource, In: 0, Out: 5 * time.Second, Duration: time.Second}})
		c.Check(err != nil && result == nil, "word start outside event: got %v, %#v", err, result)

		// A decreasing non-zero word start returns an error and nil.
		decreasing := astisub.NewSubtitles()
		decreasing.Items = []*astisub.Item{{StartAt: 0, EndAt: 5 * time.Second, Lines: []astisub.Line{{Items: []astisub.LineItem{
			{StartAt: 3 * time.Second, Text: "a"},
			{StartAt: 2 * time.Second, Text: "b"},
		}}}}}
		result, err = astisub.Assemble([]astisub.EditClip{{Source: decreasing, In: 0, Out: 5 * time.Second, Duration: time.Second}})
		c.Check(err != nil && result == nil, "decreasing word start: got %v, %#v", err, result)
	})
}

func TestEditAssemblyWords(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Words: "one " starts at 1s, "two " at 3s, "three" at 4.5s. The clip
		// cuts at 3.5s so "three" is dropped and "two " is kept.
		source := astisub.NewSubtitles()
		source.Items = []*astisub.Item{{
			StartAt: time.Second,
			EndAt:   5 * time.Second,
			Lines: []astisub.Line{{VoiceName: "Alice", Items: []astisub.LineItem{
				{Text: "one "},
				{StartAt: 3 * time.Second, Text: "two "},
				{StartAt: 4500 * time.Millisecond, Text: "three"},
			}}},
		}}

		out, err := astisub.Assemble([]astisub.EditClip{
			{Source: source, In: 2 * time.Second, Out: 3500 * time.Millisecond, Duration: 3 * time.Second},
		})
		c.Check(err == nil, "assemble returned %v", err)
		if err != nil || len(out.Items) != 1 {
			return
		}
		lines := out.Items[0].Lines
		c.Check(len(lines) == 1, "expected 1 line, got %d", len(lines))
		if len(lines) != 1 {
			return
		}
		c.Check(lines[0].VoiceName == "Alice", "voice = %q", lines[0].VoiceName)
		// "one " ends at the 3s word marker and the clip starts at 2s, so it
		// still intersects and is kept as an on-screen fragment; "two " also
		// survives, clipped to the intersection start and mapped to zero.
		words := lines[0].Items
		c.Check(len(words) == 2, "expected 2 surviving words, got %d", len(words))
		if len(words) != 2 {
			return
		}
		c.Check(words[0].Text == "one " && words[0].StartAt == 0, "first = %q @%s, want zero marker", words[0].Text, words[0].StartAt)
		// "two " starts at 3s, one second into the 2s..3.5s clip: 1/1.5*3 = 2s.
		c.Check(words[1].Text == "two " && words[1].StartAt == 2*time.Second, "second = %q @%s, want 2s", words[1].Text, words[1].StartAt)

		// Same clip, but with a marker that lands strictly inside the new
		// event: [1s,5s) mapped onto 8s keeps "two " at its relative offset.
		out, err = astisub.Assemble([]astisub.EditClip{
			{Source: source, In: time.Second, Out: 5 * time.Second, Duration: 8 * time.Second},
		})
		c.Check(err == nil, "assemble returned %v", err)
		if err != nil || len(out.Items) != 1 || len(out.Items[0].Lines) != 1 {
			return
		}
		items := out.Items[0].Lines[0].Items
		c.Check(len(items) == 3, "expected 3 words, got %d", len(items))
		if len(items) != 3 {
			return
		}
		// "one " starts at the event start: zero marker.
		c.Check(items[0].Text == "one " && items[0].StartAt == 0, "first = %q @%s", items[0].Text, items[0].StartAt)
		// "two " starts at 2s inside a 4s source range stretched to 8s: 4s.
		c.Check(items[1].Text == "two " && items[1].StartAt == 4*time.Second, "second = %q @%s", items[1].Text, items[1].StartAt)
		// "three" starts at 3.5s: 3.5/4*8 = 7s.
		c.Check(items[2].Text == "three" && items[2].StartAt == 7*time.Second, "third = %q @%s", items[2].Text, items[2].StartAt)

		// A cut through a word's middle keeps the word's text in the group,
		// later groups are not coloured early, and dropped groups leave no
		// empty markers behind.
		fragmented := astisub.NewSubtitles()
		fragmented.Items = []*astisub.Item{{
			StartAt: time.Second,
			EndAt:   5 * time.Second,
			Lines: []astisub.Line{{Items: []astisub.LineItem{
				{Text: "alpha "},
				{StartAt: 2 * time.Second, Text: "beta "},
				{Text: "gamma"},
				{StartAt: 4 * time.Second, Text: "delta"},
			}}},
		}}
		out, err = astisub.Assemble([]astisub.EditClip{
			{Source: fragmented, In: 2500 * time.Millisecond, Out: 5 * time.Second, Duration: 5 * time.Second},
		})
		out, err = astisub.Assemble([]astisub.EditClip{
			{Source: fragmented, In: 2500 * time.Millisecond, Out: 5 * time.Second, Duration: 5 * time.Second},
		})
		c.Check(err == nil, "fragmented assemble returned %v", err)
		if err != nil || len(out.Items) != 1 || len(out.Items[0].Lines) != 1 {
			return
		}
		items = out.Items[0].Lines[0].Items
		// "alpha " ends at the 2s marker; "beta "+"gamma" belong to group
		// [2s,4s), which the clip still intersects; "gamma" keeps its zero
		// continuation marker.
		c.Check(len(items) == 3, "fragmented: expected 3 items, got %d", len(items))
		if len(items) != 3 {
			return
		}
		c.Check(items[0].Text == "beta " && items[0].StartAt == 0, "fragmented first = %q @%s", items[0].Text, items[0].StartAt)
		c.Check(items[1].Text == "gamma" && items[1].StartAt == 0, "fragmented continuation = %q @%s", items[1].Text, items[1].StartAt)
		// "delta" starts at 4s, 1.5s into the 2.5s..5s clip; stretched to a 5s
		// output that is 1.5/2.5*5 = 3s.
		c.Check(items[2].Text == "delta" && items[2].StartAt == 3*time.Second, "fragmented delta = %q @%s", items[2].Text, items[2].StartAt)

		// Tags, inline formatting, line breaks and comments survive.
		rich := astisub.NewSubtitles()
		rich.Items = []*astisub.Item{{
			StartAt:     0,
			EndAt:       4 * time.Second,
			Comments:    []string{"note one"},
			InlineStyle: &astisub.StyleAttributes{WebVTTAlign: "center"},
			Lines: []astisub.Line{
				{Items: []astisub.LineItem{
					{Text: "plain "},
					{StartAt: time.Second, Text: "bold", InlineStyle: &astisub.StyleAttributes{
						WebVTTTags: []astisub.WebVTTTag{{Name: "b"}},
						WebVTTBold: true,
					}},
				}},
				{Items: []astisub.LineItem{{Text: "second line"}}},
			},
		}}
		out, err = astisub.Assemble([]astisub.EditClip{{Source: rich, In: 0, Out: 2 * time.Second, Duration: 2 * time.Second}})
		c.Check(err == nil, "rich assemble returned %v", err)
		if err == nil && len(out.Items) == 1 {
			c.Check(len(out.Items[0].Comments) == 1 && out.Items[0].Comments[0] == "note one",
				"comments = %#v", out.Items[0].Comments)
			c.Check(len(out.Items[0].Lines) == 2, "lines = %d", len(out.Items[0].Lines))
			c.Check(out.Items[0].InlineStyle != nil && out.Items[0].InlineStyle.WebVTTAlign == "center",
				"inline style lost")
			if len(out.Items[0].Lines) == 1 && len(out.Items[0].Lines[0].Items) == 2 {
				// The bold tag object survives on the word item, and its
				// start marker maps to 0.5s.
				bold := out.Items[0].Lines[0].Items[1]
				c.Check(bold.Text == "bold" && bold.StartAt == 500*time.Millisecond, "bold = %q @%s", bold.Text, bold.StartAt)
				c.Check(bold.InlineStyle != nil && len(bold.InlineStyle.WebVTTTags) == 1 && bold.InlineStyle.WebVTTTags[0].Name == "b",
					"bold tag lost: %#v", bold.InlineStyle)
			}
		}
		// The input's item slice and line item slices must not be shared.
		c.Check(rich.Items[0].Lines[0].Items[0].Text == "plain ", "input text mutated")
		c.Check(rich.Items[0].EndAt == 4*time.Second, "input end mutated to %s", rich.Items[0].EndAt)
	})
}

func TestEditAssemblyExport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Two sources both name their style and region "default" but differ in
		// colour and layout.
		sourceA := astisub.NewSubtitles()
		sourceA.Styles["base"] = &astisub.Style{
			ID:          "base",
			InlineStyle: &astisub.StyleAttributes{TTMLDirection: strPtr("ltr")},
		}
		sourceA.Styles["default"] = &astisub.Style{
			ID:    "default",
			Style: sourceA.Styles["base"],
			InlineStyle: &astisub.StyleAttributes{
				TTMLColor:            astisub.ColorRed,
				TTMLFontFamily:       strPtr("sansSerif"),
				TTMLOrigin:           strPtr("0% 90%"),
				TTMLExtent:           strPtr("100% 10%"),
				TTMLTextAlign:        strPtr("center"),
				WebVTTAlign:          "center",
				WebVTTLine:           "90%",
				WebVTTWidth:          "100%",
				WebVTTRegionAnchor:   "0%,100%",
				WebVTTViewportAnchor: "0%,100%",
			},
		}
		sourceA.Regions["default"] = &astisub.Region{ID: "default", Style: sourceA.Styles["default"], InlineStyle: &astisub.StyleAttributes{}}
		sourceA.Items = []*astisub.Item{{
			StartAt:     0,
			EndAt:       2 * time.Second,
			Style:       sourceA.Styles["default"],
			Region:      sourceA.Regions["default"],
			InlineStyle: &astisub.StyleAttributes{},
			Lines:       []astisub.Line{{Items: []astisub.LineItem{{Style: sourceA.Styles["default"], Text: "red one"}}}},
		}}

		sourceB := astisub.NewSubtitles()
		sourceB.Styles["default"] = &astisub.Style{
			ID: "default",
			InlineStyle: &astisub.StyleAttributes{
				TTMLColor:            astisub.ColorBlue,
				TTMLFontFamily:       strPtr("serif"),
				TTMLOrigin:           strPtr("0% 80%"),
				TTMLExtent:           strPtr("100% 20%"),
				TTMLTextAlign:        strPtr("left"),
				WebVTTAlign:          "left",
				WebVTTLine:           "80%",
				WebVTTWidth:          "100%",
				WebVTTRegionAnchor:   "0%,100%",
				WebVTTViewportAnchor: "0%,100%",
			},
		}
		sourceB.Regions["default"] = &astisub.Region{ID: "default", Style: sourceB.Styles["default"], InlineStyle: &astisub.StyleAttributes{}}
		sourceB.Items = []*astisub.Item{{
			StartAt:     0,
			EndAt:       time.Second,
			Style:       sourceB.Styles["default"],
			Region:      sourceB.Regions["default"],
			InlineStyle: &astisub.StyleAttributes{},
			Lines:       []astisub.Line{{Items: []astisub.LineItem{{Style: sourceB.Styles["default"], Text: "blue two"}}}},
		}}

		out, err := astisub.Assemble([]astisub.EditClip{
			{Source: sourceA, In: 0, Out: 2 * time.Second, Duration: 2 * time.Second},
			{Source: sourceB, In: 0, Out: time.Second, Duration: time.Second},
		})
		c.Check(err == nil, "assemble returned %v", err)
		if err != nil {
			return
		}
		c.Check(len(out.Items) == 2, "items = %d", len(out.Items))
		c.Check(out.Styles["s1_default"] != nil && out.Styles["s2_default"] != nil,
			"namespaced styles missing: %#v", out.Styles)
		c.Check(out.Regions["s1_default"] != nil && out.Regions["s2_default"] != nil,
			"namespaced regions missing: %#v", out.Regions)
		c.Check(out.Items[0].Style != nil && out.Items[0].Style.ID == "s1_default", "first style = %#v", out.Items[0].Style)
		c.Check(out.Items[1].Style != nil && out.Items[1].Style.ID == "s2_default", "second style = %#v", out.Items[1].Style)
		c.Check(out.Items[0].Region != nil && out.Items[0].Region.ID == "s1_default", "first region = %#v", out.Items[0].Region)
		c.Check(out.Items[1].Region != nil && out.Items[1].Region.ID == "s2_default", "second region = %#v", out.Items[1].Region)
		c.Check(out.Regions["s1_default"].Style == out.Styles["s1_default"], "region->style chain broken for source A")
		c.Check(out.Regions["s2_default"].Style == out.Styles["s2_default"], "region->style chain broken for source B")
		// The parent style chain of source A is renamed too.
		c.Check(out.Styles["s1_default"].Style == out.Styles["s1_base"], "parent style chain broken for source A")
		c.Check(out.Styles["s1_base"].Style == nil, "source A base style gained a parent")
		c.Check(out.Styles["s1_default"].InlineStyle != out.Styles["s2_default"].InlineStyle, "style attributes shared")
		c.Check(out.Items[0].StartAt == 0 && out.Items[0].EndAt == 2*time.Second, "first item timing wrong")
		c.Check(out.Items[1].StartAt == 2*time.Second && out.Items[1].EndAt == 3*time.Second, "second item timing wrong: [%s,%s)", out.Items[1].StartAt, out.Items[1].EndAt)

		// TTML round trip.
		var ttml bytes.Buffer
		c.Check(out.WriteToTTML(&ttml) == nil, "WriteToTTML failed")
		c.Check(strings.Contains(ttml.String(), `xml:id="s1_default"`) && strings.Contains(ttml.String(), `xml:id="s2_default"`),
			"TTML output missing namespaced ids:\n%s", ttml.String())
		readTTML, err := astisub.ReadFromTTML(bytes.NewReader(ttml.Bytes()))
		c.Check(err == nil, "ReadFromTTML failed: %v", err)
		if err == nil {
			c.Check(len(readTTML.Items) == 2, "TTML read items = %d", len(readTTML.Items))
			if len(readTTML.Items) == 2 {
				c.Check(readTTML.Items[0].StartAt == 0 && readTTML.Items[0].EndAt == 2*time.Second, "TTML first timing wrong")
				c.Check(readTTML.Items[1].StartAt == 2*time.Second && readTTML.Items[1].EndAt == 3*time.Second, "TTML second timing wrong")
				c.Check(readTTML.Items[0].Lines[0].Items[0].Text == "red one", "TTML first text = %q", readTTML.Items[0].Lines[0].Items[0].Text)
				c.Check(readTTML.Items[1].Lines[0].Items[0].Text == "blue two", "TTML second text = %q", readTTML.Items[1].Lines[0].Items[0].Text)
				c.Check(readTTML.Items[0].Style != nil && readTTML.Items[0].Style.ID == "s1_default", "TTML first style = %#v", readTTML.Items[0].Style)
				c.Check(readTTML.Items[1].Style != nil && readTTML.Items[1].Style.ID == "s2_default", "TTML second style = %#v", readTTML.Items[1].Style)
				// TTML expresses the cue alignment through the namespaced style
				// attributes, so both directions survive the round trip.
				c.Check(readTTML.Styles["s1_default"].InlineStyle.TTMLColor.HTMLString() == "#ff0000", "TTML first colour lost")
				c.Check(readTTML.Styles["s2_default"].InlineStyle.TTMLColor.HTMLString() == "#0000ff", "TTML second colour lost")
				c.Check(readTTML.Items[0].Style != nil && readTTML.Items[0].Style.InlineStyle.TTMLTextAlign != nil &&
					*readTTML.Items[0].Style.InlineStyle.TTMLTextAlign == "center", "TTML first alignment lost")
				c.Check(readTTML.Items[1].Style != nil && readTTML.Items[1].Style.InlineStyle.TTMLTextAlign != nil &&
					*readTTML.Items[1].Style.InlineStyle.TTMLTextAlign == "left", "TTML second alignment lost")
				c.Check(readTTML.Styles["s1_default"].InlineStyle.TTMLFontFamily != nil && *readTTML.Styles["s1_default"].InlineStyle.TTMLFontFamily == "sansSerif",
					"TTML first font family lost")
				c.Check(readTTML.Styles["s2_default"].InlineStyle.TTMLFontFamily != nil && *readTTML.Styles["s2_default"].InlineStyle.TTMLFontFamily == "serif",
					"TTML second font family lost")
			}
		}

		// WebVTT round trip.
		var vtt bytes.Buffer
		c.Check(out.WriteToWebVTT(&vtt) == nil, "WriteToWebVTT failed")
		readVTT, err := astisub.ReadFromWebVTT(bytes.NewReader(vtt.Bytes()))
		c.Check(err == nil, "ReadFromWebVTT failed: %v\n%s", err, vtt.String())
		if err == nil {
			c.Check(len(readVTT.Items) == 2, "WebVTT read items = %d", len(readVTT.Items))
			if len(readVTT.Items) == 2 {
				c.Check(readVTT.Items[0].StartAt == 0 && readVTT.Items[0].EndAt == 2*time.Second, "WebVTT first timing wrong")
				c.Check(readVTT.Items[1].StartAt == 2*time.Second && readVTT.Items[1].EndAt == 3*time.Second, "WebVTT second timing wrong")
				c.Check(readVTT.Items[0].Lines[0].Items[0].Text == "red one", "WebVTT first text = %q", readVTT.Items[0].Lines[0].Items[0].Text)
				c.Check(readVTT.Items[1].Lines[0].Items[0].Text == "blue two", "WebVTT second text = %q", readVTT.Items[1].Lines[0].Items[0].Text)
				c.Check(readVTT.Items[0].Region != nil && readVTT.Items[0].Region.ID == "s1_default",
					"WebVTT first region = %#v", readVTT.Items[0].Region)
				c.Check(readVTT.Items[1].Region != nil && readVTT.Items[1].Region.ID == "s2_default",
					"WebVTT second region = %#v", readVTT.Items[1].Region)
				c.Check(readVTT.Regions["s1_default"] != nil && readVTT.Regions["s2_default"] != nil,
					"WebVTT namespaced regions missing: %#v", readVTT.Regions)
				// WebVTT can't express the region distinction for styles with
				// no CSS blocks, but cue alignment must survive: each cue keeps
				// the alignment of its own source's "default" style.
				c.Check(readVTT.Items[0].InlineStyle != nil && readVTT.Items[0].InlineStyle.WebVTTAlign == "center",
					"WebVTT first align = %q", readVTT.Items[0].InlineStyle.WebVTTAlign)
				c.Check(readVTT.Items[1].InlineStyle != nil && readVTT.Items[1].InlineStyle.WebVTTAlign == "left",
					"WebVTT second align = %q", readVTT.Items[1].InlineStyle.WebVTTAlign)
				c.Check(readVTT.Items[0].InlineStyle != readVTT.Items[1].InlineStyle, "WebVTT cues share inline style")
			}
		}

		// The output does not inherit the source's timestamp map.
		mapped := astisub.NewSubtitles()
		mapped.Metadata = &astisub.Metadata{WebVTTTimestampMap: &astisub.WebVTTTimestampMap{Local: time.Second, MpegTS: 90000}}
		mapped.Items = []*astisub.Item{{StartAt: 0, EndAt: time.Second, Lines: []astisub.Line{{Items: []astisub.LineItem{{Text: "mapped"}}}}}}
		out, err = astisub.Assemble([]astisub.EditClip{{Source: mapped, In: 0, Out: time.Second, Duration: time.Second}})
		c.Check(err == nil, "mapped assemble returned %v", err)
		c.Check(out != nil && (out.Metadata == nil || out.Metadata.WebVTTTimestampMap == nil), "timestamp map leaked into output")

		// Subprocess check: the CLI and the library agree.
		dir, err := ioutil.TempDir("", "astisub-assemble")
		c.Check(err == nil, "TempDir failed: %v", err)
		if err != nil {
			return
		}
		defer os.RemoveAll(dir)

		clipSourcePath := filepath.Join(dir, "source.vtt")
		sourceVTT := "WEBVTT\n\nREGION\nid:default\nwidth:100%\n\n1\n00:00:01.000 --> 00:00:05.000 region:default\n<v Alice><b>one <00:00:03.000>two</b>\n"
		c.Check(ioutil.WriteFile(clipSourcePath, []byte(sourceVTT), 0644) == nil, "writing source failed")
		manifestPath := filepath.Join(dir, "clips.json")
		// Both clips point at the same file, written two different ways to
		// check that the CLI normalizes paths and loads it once.
		manifest := []map[string]string{
			{"source": "source.vtt", "in": "3s", "out": "6s", "duration": "6s"},
			{"source": "./source.vtt", "in": "0s", "out": "2s", "duration": "1s"},
		}
		data, err := json.Marshal(manifest)
		c.Check(err == nil, "marshal manifest failed: %v", err)
		c.Check(ioutil.WriteFile(manifestPath, data, 0644) == nil, "writing manifest failed")
		outputPath := filepath.Join(dir, "out.vtt")

		cmd := exec.Command("go", "run", "github.com/asticode/go-astisub/cmd/astisub", "assemble", "-i", manifestPath, "-o", outputPath)
		cmd.Env = append(os.Environ(), "GOFLAGS=-mod=vendor", "GOPROXY=off", "GOTOOLCHAIN=local")
		cmd.Dir = moduleRoot(t)
		output, err := cmd.CombinedOutput()
		c.Check(err == nil, "CLI assemble failed: %v\n%s", err, output)

		// The library result for the same clips, loaded through the same
		// normalized path, must match the CLI output.
		sourceLoaded, err := astisub.OpenFile(clipSourcePath)
		c.Check(err == nil, "opening source failed: %v", err)
		want, err := astisub.Assemble([]astisub.EditClip{
			{Source: sourceLoaded, In: 3 * time.Second, Out: 6 * time.Second, Duration: 6 * time.Second},
			{Source: sourceLoaded, In: 0, Out: 2 * time.Second, Duration: time.Second},
		})
		c.Check(err == nil, "library assemble failed: %v", err)
		if err == nil {
			cliSubtitles, err := astisub.OpenFile(outputPath)
			c.Check(err == nil, "reading CLI output failed: %v", err)
			if err == nil {
				c.Check(len(cliSubtitles.Items) == len(want.Items), "CLI items = %d, library items = %d", len(cliSubtitles.Items), len(want.Items))
				if len(cliSubtitles.Items) == len(want.Items) && len(want.Items) == 2 {
					c.Check(cliSubtitles.Items[0].StartAt == want.Items[0].StartAt && cliSubtitles.Items[0].EndAt == want.Items[0].EndAt,
						"CLI first timing [%s,%s) != library [%s,%s)", cliSubtitles.Items[0].StartAt, cliSubtitles.Items[0].EndAt, want.Items[0].StartAt, want.Items[0].EndAt)
					c.Check(cliSubtitles.Items[1].StartAt == want.Items[1].StartAt && cliSubtitles.Items[1].EndAt == want.Items[1].EndAt,
						"CLI second timing [%s,%s) != library [%s,%s)", cliSubtitles.Items[1].StartAt, cliSubtitles.Items[1].EndAt, want.Items[1].StartAt, want.Items[1].EndAt)
					c.Check(cliSubtitles.Items[0].Lines[0].String() == want.Items[0].Lines[0].String(), "CLI first text = %q", cliSubtitles.Items[0].Lines[0].String())
					c.Check(cliSubtitles.Items[1].Lines[0].String() == want.Items[1].Lines[0].String(), "CLI second text = %q", cliSubtitles.Items[1].Lines[0].String())
					// Both clips reference the same normalized path, so the
					// CLI must load it once and reuse one "s1_" namespace
					// rather than producing a second one.
					c.Check(cliSubtitles.Items[0].Region != nil && cliSubtitles.Items[0].Region.ID == "s1_default",
						"CLI first region = %#v", cliSubtitles.Items[0].Region)
					c.Check(cliSubtitles.Items[1].Region != nil && cliSubtitles.Items[1].Region.ID == "s1_default",
						"CLI second region = %#v", cliSubtitles.Items[1].Region)
					c.Check(want.Items[0].Region != nil && cliSubtitles.Items[0].Region.ID == want.Items[0].Region.ID,
						"CLI region namespace %q differs from library %q", cliSubtitles.Items[0].Region.ID, want.Items[0].Region.ID)
					// Word markers are part of the assembled result too.
					c.Check(len(cliSubtitles.Items[0].Lines[0].Items) == len(want.Items[0].Lines[0].Items), "CLI first word count differs")
					if len(cliSubtitles.Items[0].Lines[0].Items) > 1 && len(want.Items[0].Lines[0].Items) > 1 {
						c.Check(cliSubtitles.Items[0].Lines[0].Items[1].StartAt == want.Items[0].Lines[0].Items[1].StartAt,
							"CLI first word start = %s, library = %s", cliSubtitles.Items[0].Lines[0].Items[1].StartAt, want.Items[0].Lines[0].Items[1].StartAt)
					}
				}
			}
		}

		// A bad manifest must not truncate an existing output file.
		badManifestPath := filepath.Join(dir, "bad.json")
		c.Check(ioutil.WriteFile(badManifestPath, []byte(`[{"source":"source.vtt","in":"3s","out":"2s","duration":"1s"}]`), 0644) == nil, "writing bad manifest failed")
		existing := []byte("keep me")
		c.Check(ioutil.WriteFile(outputPath, existing, 0644) == nil, "writing existing output failed")
		cmd = exec.Command("go", "run", "github.com/asticode/go-astisub/cmd/astisub", "assemble", "-i", badManifestPath, "-o", outputPath)
		cmd.Env = append(os.Environ(), "GOFLAGS=-mod=vendor", "GOPROXY=off", "GOTOOLCHAIN=local")
		cmd.Dir = moduleRoot(t)
		_, err = cmd.CombinedOutput()
		c.Check(err != nil, "bad manifest unexpectedly succeeded")
		kept, readErr := ioutil.ReadFile(outputPath)
		c.Check(readErr == nil && string(kept) == string(existing), "bad manifest truncated output: %q", kept)
	})
}

func strPtr(s string) *string { return &s }

// moduleRoot returns the repository root so subprocesses run from there.
func moduleRoot(t *testing.T) string {
	t.Helper()
	dir, err := os.Getwd()
	if err != nil {
		return "."
	}
	return dir
}
