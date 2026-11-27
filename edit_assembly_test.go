package astisub_test

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	astisub "github.com/asticode/go-astisub"
	"github.com/asticode/go-astisub/task_support/taskcheck"
)

func editAssemblyTextItem(start, end time.Duration, text string) *astisub.Item {
	return &astisub.Item{
		EndAt:   end,
		Lines:   []astisub.Line{{Items: []astisub.LineItem{{Text: text}}}},
		StartAt: start,
	}
}

func TestEditAssemblyTimeline(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Source with two overlapping events
		var src = astisub.NewSubtitles()
		src.Items = []*astisub.Item{
			editAssemblyTextItem(time.Second, 5*time.Second, "A"),
			editAssemblyTextItem(4*time.Second, 7*time.Second, "B"),
		}

		// The edit list is concatenated in the given order starting at zero,
		// may reuse a source and may select intervals out of order
		var clips = []astisub.EditClip{
			{Source: src, In: 3 * time.Second, Out: 6 * time.Second, Duration: 6 * time.Second},
			{Source: src, In: 0, Out: 2 * time.Second, Duration: time.Second},
		}
		out, err := astisub.Assemble(clips)
		c.Check(err == nil, "assemble failed: %v", err)
		if err != nil {
			return
		}
		c.Check(len(out.Items) == 3, "expected 3 items, got %d", len(out.Items))
		if len(out.Items) != 3 {
			return
		}
		// Event A [1,5) clipped to [3,5) plays for 4s at 2x duration: [0,4)
		c.Check(out.Items[0].StartAt == 0, "item 0 start: got %s", out.Items[0].StartAt)
		c.Check(out.Items[0].EndAt == 4*time.Second, "item 0 end: got %s", out.Items[0].EndAt)
		c.Check(out.Items[0].String() == "A", "item 0 text: got %q", out.Items[0].String())
		// Event B [4,7) clipped to [4,6) overlaps A in the output: [2,6)
		c.Check(out.Items[1].StartAt == 2*time.Second, "item 1 start: got %s", out.Items[1].StartAt)
		c.Check(out.Items[1].EndAt == 6*time.Second, "item 1 end: got %s", out.Items[1].EndAt)
		c.Check(out.Items[1].String() == "B", "item 1 text: got %q", out.Items[1].String())
		// Event A clipped to [1,2) plays for 500ms after the first 6s: [6.5,7)
		c.Check(out.Items[2].StartAt == 6500*time.Millisecond, "item 2 start: got %s", out.Items[2].StartAt)
		c.Check(out.Items[2].EndAt == 7*time.Second, "item 2 end: got %s", out.Items[2].EndAt)
		c.Check(out.Items[2].String() == "A", "item 2 text: got %q", out.Items[2].String())
		// Items are renumbered consecutively, not merged by text
		c.Check(out.Items[0].Index == 1 && out.Items[1].Index == 2 && out.Items[2].Index == 3,
			"indexes: got %d, %d, %d", out.Items[0].Index, out.Items[1].Index, out.Items[2].Index)

		// Inputs are not modified
		c.Check(src.Items[0].StartAt == time.Second && src.Items[0].EndAt == 5*time.Second,
			"source item 0 mutated: [%s, %s]", src.Items[0].StartAt, src.Items[0].EndAt)
		c.Check(clips[0].In == 3*time.Second && clips[0].Out == 6*time.Second && clips[1].Duration == time.Second,
			"edit list mutated: %+v", clips)
		// Output events, lines and line items are fresh objects
		c.Check(out.Items[0] != src.Items[0], "output item aliases source item")
		c.Check(&out.Items[0].Lines[0].Items[0] != &src.Items[0].Lines[0].Items[0], "output line item aliases source line item")

		// Speed change: a 2s selection played over 1s scales and rounds down
		// to the nanosecond
		out, err = astisub.Assemble([]astisub.EditClip{{Source: src, In: 0, Out: 3 * time.Second, Duration: time.Second}})
		c.Check(err == nil, "assemble failed: %v", err)
		if err == nil {
			c.Check(len(out.Items) == 1, "expected 1 item, got %d", len(out.Items))
			if len(out.Items) == 1 {
				c.Check(out.Items[0].StartAt == 333333333*time.Nanosecond, "scaled start: got %s", out.Items[0].StartAt)
				c.Check(out.Items[0].EndAt == time.Second, "scaled end: got %s", out.Items[0].EndAt)
			}
		}

		// Slow motion: a 2s selection played over 4s
		out, err = astisub.Assemble([]astisub.EditClip{{Source: src, In: time.Second, Out: 3 * time.Second, Duration: 4 * time.Second}})
		c.Check(err == nil, "assemble failed: %v", err)
		if err == nil {
			c.Check(len(out.Items) == 1, "expected 1 item, got %d", len(out.Items))
			if len(out.Items) == 1 {
				c.Check(out.Items[0].StartAt == 0, "slow start: got %s", out.Items[0].StartAt)
				c.Check(out.Items[0].EndAt == 4*time.Second, "slow end: got %s", out.Items[0].EndAt)
			}
		}

		// Repeated selection of the same interval produces independent items
		out, err = astisub.Assemble([]astisub.EditClip{
			{Source: src, In: 0, Out: 2 * time.Second, Duration: time.Second},
			{Source: src, In: 0, Out: 2 * time.Second, Duration: time.Second},
		})
		c.Check(err == nil, "assemble failed: %v", err)
		if err == nil {
			c.Check(len(out.Items) == 2, "expected 2 items, got %d", len(out.Items))
			if len(out.Items) == 2 {
				c.Check(out.Items[0].StartAt == 500*time.Millisecond && out.Items[0].EndAt == time.Second,
					"first repeat: got [%s, %s]", out.Items[0].StartAt, out.Items[0].EndAt)
				c.Check(out.Items[1].StartAt == 1500*time.Millisecond && out.Items[1].EndAt == 2*time.Second,
					"second repeat: got [%s, %s]", out.Items[1].StartAt, out.Items[1].EndAt)
				c.Check(out.Items[0].String() == "A" && out.Items[1].String() == "A", "repeat texts: %q, %q",
					out.Items[0].String(), out.Items[1].String())
			}
		}

		// Events tied on output start keep the source items order
		var srcTie = astisub.NewSubtitles()
		srcTie.Items = []*astisub.Item{
			editAssemblyTextItem(0, 2*time.Second, "X"),
			editAssemblyTextItem(0, 2*time.Second, "Y"),
		}
		out, err = astisub.Assemble([]astisub.EditClip{{Source: srcTie, In: 0, Out: 2 * time.Second, Duration: 2 * time.Second}})
		c.Check(err == nil, "assemble failed: %v", err)
		if err == nil && len(out.Items) == 2 {
			c.Check(out.Items[0].String() == "X" && out.Items[1].String() == "Y", "tie order: %q, %q",
				out.Items[0].String(), out.Items[1].String())
		} else if err == nil {
			c.Check(false, "expected 2 items, got %d", len(out.Items))
		}

		// An empty edit list returns usable empty subtitles
		out, err = astisub.Assemble(nil)
		c.Check(err == nil, "empty assemble failed: %v", err)
		c.Check(out != nil && out.IsEmpty(), "expected empty subtitles")
		c.Check(out.Styles != nil && out.Regions != nil, "empty subtitles not usable")

		// Invalid edit lists return an error and a nil result
		var bad = []struct {
			name  string
			clips []astisub.EditClip
		}{
			{"nil source", []astisub.EditClip{{Source: nil, In: 0, Out: time.Second, Duration: time.Second}}},
			{"negative in", []astisub.EditClip{{Source: src, In: -time.Second, Out: time.Second, Duration: time.Second}}},
			{"out equal to in", []astisub.EditClip{{Source: src, In: time.Second, Out: time.Second, Duration: time.Second}}},
			{"out before in", []astisub.EditClip{{Source: src, In: 2 * time.Second, Out: time.Second, Duration: time.Second}}},
			{"zero duration", []astisub.EditClip{{Source: src, In: 0, Out: time.Second, Duration: 0}}},
			{"negative duration", []astisub.EditClip{{Source: src, In: 0, Out: time.Second, Duration: -time.Second}}},
			{"cumulative overflow", []astisub.EditClip{
				{Source: src, In: 0, Out: time.Second, Duration: time.Duration(1<<63 - 1)},
				{Source: src, In: 0, Out: time.Second, Duration: time.Second},
			}},
		}
		for _, b := range bad {
			var o *astisub.Subtitles
			o, err = astisub.Assemble(b.clips)
			c.Check(err != nil, "%s: expected an error", b.name)
			c.Check(o == nil, "%s: expected a nil result", b.name)
		}

		// Source events with inverted times are rejected
		var srcInverted = astisub.NewSubtitles()
		srcInverted.Items = []*astisub.Item{editAssemblyTextItem(5*time.Second, 3*time.Second, "bad")}
		out, err = astisub.Assemble([]astisub.EditClip{{Source: srcInverted, In: 0, Out: 10 * time.Second, Duration: 10 * time.Second}})
		c.Check(err != nil, "inverted event: expected an error")
		c.Check(out == nil, "inverted event: expected a nil result")
	})
}

func TestEditAssemblyWords(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Source with word-level timestamps: groups start at 1s (default), 2s
		// and 4s inside the [1,5) event
		src, err := astisub.ReadFromWebVTT(strings.NewReader("WEBVTT\n\n1\n00:00:01.000 --> 00:00:05.000\n<v Alice><b>one <00:00:02.000>two <00:00:04.000>three</b>\n"))
		c.Check(err == nil, "read failed: %v", err)
		if err != nil || len(src.Items) != 1 {
			c.Check(len(src.Items) == 1, "expected 1 source item, got %d", len(src.Items))
			return
		}
		c.Check(len(src.Items[0].Lines[0].Items) == 3, "expected 3 source line items, got %d", len(src.Items[0].Lines[0].Items))

		// Cut in the middle of the first and last groups, played at half
		// speed: [1.5,4.5) over 6s
		out, err := astisub.Assemble([]astisub.EditClip{{Source: src, In: 1500 * time.Millisecond, Out: 4500 * time.Millisecond, Duration: 6 * time.Second}})
		c.Check(err == nil, "assemble failed: %v", err)
		if err != nil {
			return
		}
		c.Check(len(out.Items) == 1, "expected 1 item, got %d", len(out.Items))
		if len(out.Items) != 1 {
			return
		}
		var item = out.Items[0]
		c.Check(item.StartAt == 0 && item.EndAt == 6*time.Second, "event: got [%s, %s]", item.StartAt, item.EndAt)
		c.Check(len(item.Lines) == 1, "expected 1 line, got %d", len(item.Lines))
		if len(item.Lines) != 1 {
			return
		}
		var line = item.Lines[0]
		c.Check(line.VoiceName == "Alice", "voice: got %q", line.VoiceName)
		c.Check(len(line.Items) == 3, "expected 3 line items, got %d", len(line.Items))
		if len(line.Items) != 3 {
			return
		}
		// The cut group is clamped to the intersection start and lands on the
		// new event start: its marker is written as zero
		c.Check(line.Items[0].Text == "one ", "text 0: got %q", line.Items[0].Text)
		c.Check(line.Items[0].StartAt == 0, "start 0: got %s", line.Items[0].StartAt)
		c.Check(line.Items[1].Text == "two ", "text 1: got %q", line.Items[1].Text)
		c.Check(line.Items[1].StartAt == time.Second, "start 1: got %s", line.Items[1].StartAt)
		c.Check(line.Items[2].Text == "three", "text 2: got %q", line.Items[2].Text)
		c.Check(line.Items[2].StartAt == 5*time.Second, "start 2: got %s", line.Items[2].StartAt)
		// Tags are kept
		c.Check(line.Items[0].InlineStyle != nil && len(line.Items[0].InlineStyle.WebVTTTags) == 1 &&
			line.Items[0].InlineStyle.WebVTTTags[0].Name == "b", "tags of item 0 missing")

		// A group fully outside the cut leaves no empty marker behind
		out, err = astisub.Assemble([]astisub.EditClip{{Source: src, In: time.Second, Out: 3 * time.Second, Duration: 2 * time.Second}})
		c.Check(err == nil, "assemble failed: %v", err)
		if err == nil && len(out.Items) == 1 && len(out.Items[0].Lines) == 1 {
			var items = out.Items[0].Lines[0].Items
			c.Check(len(items) == 2, "expected 2 line items, got %d", len(items))
			if len(items) == 2 {
				c.Check(items[0].Text == "one " && items[0].StartAt == 0, "item 0: got %q at %s", items[0].Text, items[0].StartAt)
				c.Check(items[1].Text == "two " && items[1].StartAt == time.Second, "item 1: got %q at %s", items[1].Text, items[1].StartAt)
			}
		} else if err == nil {
			c.Check(false, "unexpected output: %d items", len(out.Items))
		}

		// A cut exactly on a group boundary turns the marker into a zero
		// continuation
		out, err = astisub.Assemble([]astisub.EditClip{{Source: src, In: 2 * time.Second, Out: 4 * time.Second, Duration: 2 * time.Second}})
		c.Check(err == nil, "assemble failed: %v", err)
		if err == nil && len(out.Items) == 1 && len(out.Items[0].Lines) == 1 {
			var items = out.Items[0].Lines[0].Items
			c.Check(len(items) == 1, "expected 1 line item, got %d", len(items))
			if len(items) == 1 {
				c.Check(items[0].Text == "two ", "text: got %q", items[0].Text)
				c.Check(items[0].StartAt == 0, "start: got %s", items[0].StartAt)
			}
		} else if err == nil {
			c.Check(false, "unexpected output: %d items", len(out.Items))
		}

		// Two consecutive clips split the event across a cut point; each side
		// gets its own word markers on the new timeline
		out, err = astisub.Assemble([]astisub.EditClip{
			{Source: src, In: time.Second, Out: 3 * time.Second, Duration: 2 * time.Second},
			{Source: src, In: 3 * time.Second, Out: 5 * time.Second, Duration: 2 * time.Second},
		})
		c.Check(err == nil, "assemble failed: %v", err)
		if err == nil {
			c.Check(len(out.Items) == 2, "expected 2 items, got %d", len(out.Items))
			if len(out.Items) == 2 {
				c.Check(out.Items[0].StartAt == 0 && out.Items[0].EndAt == 2*time.Second,
					"cue 1: got [%s, %s]", out.Items[0].StartAt, out.Items[0].EndAt)
				c.Check(out.Items[1].StartAt == 2*time.Second && out.Items[1].EndAt == 4*time.Second,
					"cue 2: got [%s, %s]", out.Items[1].StartAt, out.Items[1].EndAt)
				var items1 = out.Items[0].Lines[0].Items
				c.Check(len(items1) == 2 && items1[0].StartAt == 0 && items1[1].StartAt == time.Second,
					"cue 1 word markers wrong")
				var items2 = out.Items[1].Lines[0].Items
				c.Check(len(items2) == 2, "expected 2 line items in cue 2, got %d", len(items2))
				if len(items2) == 2 {
					c.Check(items2[0].Text == "two " && items2[0].StartAt == 0, "cue 2 item 0: got %q at %s", items2[0].Text, items2[0].StartAt)
					c.Check(items2[1].Text == "three" && items2[1].StartAt == 3*time.Second, "cue 2 item 1: got %q at %s", items2[1].Text, items2[1].StartAt)
				}
				// The written cues carry the mapped markers and no zero marker
				var buf bytes.Buffer
				c.Check(out.WriteToWebVTT(&buf) == nil, "write failed")
				c.Check(strings.Contains(buf.String(), "<00:00:01.000>"), "missing 1s marker: %q", buf.String())
				c.Check(strings.Contains(buf.String(), "<00:00:03.000>"), "missing 3s marker: %q", buf.String())
				c.Check(!strings.Contains(buf.String(), "<00:00:00.000>"), "unexpected zero marker: %q", buf.String())
			}
		}

		// Zero-valued fragments continue the previous group and are never
		// mapped as source time zero
		var srcCont = astisub.NewSubtitles()
		srcCont.Items = []*astisub.Item{{
			EndAt:   20 * time.Second,
			StartAt: 10 * time.Second,
			Lines: []astisub.Line{{Items: []astisub.LineItem{
				{Text: "a ", StartAt: 10 * time.Second},
				{Text: "b ", StartAt: 15 * time.Second},
				{Text: "c"},
			}}},
		}}
		out, err = astisub.Assemble([]astisub.EditClip{{Source: srcCont, In: 16 * time.Second, Out: 20 * time.Second, Duration: 4 * time.Second}})
		c.Check(err == nil, "assemble failed: %v", err)
		if err == nil && len(out.Items) == 1 && len(out.Items[0].Lines) == 1 {
			var items = out.Items[0].Lines[0].Items
			c.Check(len(items) == 2, "expected 2 line items, got %d", len(items))
			if len(items) == 2 {
				c.Check(items[0].Text == "b " && items[0].StartAt == 0, "item 0: got %q at %s", items[0].Text, items[0].StartAt)
				c.Check(items[1].Text == "c" && items[1].StartAt == 0, "item 1: got %q at %s", items[1].Text, items[1].StartAt)
			}
		} else if err == nil {
			c.Check(false, "unexpected output: %d items", len(out.Items))
		}

		// Invalid word group starts are rejected
		var wordAfter = astisub.NewSubtitles()
		wordAfter.Items = []*astisub.Item{{
			EndAt:   5 * time.Second,
			StartAt: time.Second,
			Lines:   []astisub.Line{{Items: []astisub.LineItem{{Text: "x", StartAt: 6 * time.Second}}}},
		}}
		out, err = astisub.Assemble([]astisub.EditClip{{Source: wordAfter, In: 0, Out: 10 * time.Second, Duration: 10 * time.Second}})
		c.Check(err != nil, "word start after event: expected an error")
		c.Check(out == nil, "word start after event: expected a nil result")

		var wordBefore = astisub.NewSubtitles()
		wordBefore.Items = []*astisub.Item{{
			EndAt:   5 * time.Second,
			StartAt: time.Second,
			Lines:   []astisub.Line{{Items: []astisub.LineItem{{Text: "x", StartAt: 500 * time.Millisecond}}}},
		}}
		out, err = astisub.Assemble([]astisub.EditClip{{Source: wordBefore, In: 0, Out: 10 * time.Second, Duration: 10 * time.Second}})
		c.Check(err != nil, "word start before event: expected an error")
		c.Check(out == nil, "word start before event: expected a nil result")

		var wordDecreasing = astisub.NewSubtitles()
		wordDecreasing.Items = []*astisub.Item{{
			EndAt:   5 * time.Second,
			StartAt: time.Second,
			Lines: []astisub.Line{{Items: []astisub.LineItem{
				{Text: "a ", StartAt: 3 * time.Second},
				{Text: "b", StartAt: 2 * time.Second},
			}}},
		}}
		out, err = astisub.Assemble([]astisub.EditClip{{Source: wordDecreasing, In: 0, Out: 10 * time.Second, Duration: 10 * time.Second}})
		c.Check(err != nil, "decreasing word starts: expected an error")
		c.Check(out == nil, "decreasing word starts: expected a nil result")
	})
}

const editAssemblyTTML1 = `<?xml version="1.0" encoding="UTF-8"?>
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling" xml:lang="en">
  <head>
    <styling>
      <style xml:id="default" tts:color="#ff0000" tts:fontStyle="italic"/>
      <style xml:id="accent" tts:color="#00ff00" style="default"/>
    </styling>
    <layout>
      <region xml:id="default" tts:origin="10% 10%" tts:extent="80% 10%"/>
    </layout>
  </head>
  <body>
    <div>
      <p begin="00:00:01.000" end="00:00:03.000" style="accent" region="default">Hello <span tts:color="#ff0000">red</span> world</p>
      <p begin="00:00:04.000" end="00:00:06.000" style="default" region="default">Second</p>
    </div>
  </body>
</tt>`

const editAssemblyTTML2 = `<?xml version="1.0" encoding="UTF-8"?>
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling" xml:lang="en">
  <head>
    <styling>
      <style xml:id="default" tts:color="#0000ff" tts:fontWeight="bold"/>
    </styling>
    <layout>
      <region xml:id="default" tts:origin="20% 80%" tts:extent="60% 10%"/>
    </layout>
  </head>
  <body>
    <div>
      <p begin="00:00:02.000" end="00:00:05.000" style="default" region="default">Blue</p>
    </div>
  </body>
</tt>`

func TestEditAssemblyExport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Two sources sharing the same style and region names but with
		// different colors and layouts
		src1, err := astisub.ReadFromTTML(strings.NewReader(editAssemblyTTML1))
		c.Check(err == nil, "read source 1 failed: %v", err)
		src2, err := astisub.ReadFromTTML(strings.NewReader(editAssemblyTTML2))
		c.Check(err == nil, "read source 2 failed: %v", err)
		if err != nil {
			return
		}

		out, err := astisub.Assemble([]astisub.EditClip{
			{Source: src1, In: 0, Out: 10 * time.Second, Duration: 10 * time.Second},
			{Source: src2, In: 0, Out: 10 * time.Second, Duration: 10 * time.Second},
		})
		c.Check(err == nil, "assemble failed: %v", err)
		if err != nil {
			return
		}

		// Events land on the concatenated timeline
		c.Check(len(out.Items) == 3, "expected 3 items, got %d", len(out.Items))
		if len(out.Items) != 3 {
			return
		}
		c.Check(out.Items[0].StartAt == time.Second && out.Items[0].EndAt == 3*time.Second,
			"item 0: got [%s, %s]", out.Items[0].StartAt, out.Items[0].EndAt)
		c.Check(out.Items[1].StartAt == 4*time.Second && out.Items[1].EndAt == 6*time.Second,
			"item 1: got [%s, %s]", out.Items[1].StartAt, out.Items[1].EndAt)
		c.Check(out.Items[2].StartAt == 12*time.Second && out.Items[2].EndAt == 15*time.Second,
			"item 2: got [%s, %s]", out.Items[2].StartAt, out.Items[2].EndAt)

		// Same-named styles and regions are namespaced per source
		_, ok := out.Styles["default"]
		c.Check(!ok, "unprefixed default style should not exist")
		c.Check(out.Styles["s1_default"] != nil && out.Styles["s1_default"].InlineStyle.TTMLColor.HTMLString() == "#ff0000",
			"s1_default style wrong")
		c.Check(out.Styles["s2_default"] != nil && out.Styles["s2_default"].InlineStyle.TTMLColor.HTMLString() == "#0000ff",
			"s2_default style wrong")
		c.Check(out.Styles["s1_accent"] != nil && out.Styles["s1_accent"].Style == out.Styles["s1_default"],
			"s1_accent parent chain broken")
		c.Check(out.Regions["s1_default"] != nil && out.Regions["s2_default"] != nil, "namespaced regions missing")
		c.Check(out.Items[0].Style == out.Styles["s1_accent"], "item 0 style wrong")
		c.Check(out.Items[0].Region == out.Regions["s1_default"], "item 0 region wrong")
		c.Check(out.Items[2].Style == out.Styles["s2_default"], "item 2 style wrong")
		c.Check(out.Items[2].Region == out.Regions["s2_default"], "item 2 region wrong")

		// Sources are not modified and no metadata is merged
		c.Check(src1.Styles["default"] != nil && src1.Styles["default"].ID == "default", "source 1 styles mutated")
		c.Check(src1.Regions["default"] != nil && src1.Regions["default"].ID == "default", "source 1 regions mutated")
		c.Check(out.Metadata == nil, "output should not inherit metadata")

		// TTML round trip keeps times, text, colors and layout
		var ttmlBuf bytes.Buffer
		c.Check(out.WriteToTTML(&ttmlBuf) == nil, "write ttml failed")
		rt, err := astisub.ReadFromTTML(&ttmlBuf)
		c.Check(err == nil, "re-read ttml failed: %v", err)
		if err == nil {
			c.Check(len(rt.Items) == 3, "ttml: expected 3 items, got %d", len(rt.Items))
			if len(rt.Items) == 3 {
				c.Check(rt.Items[0].StartAt == time.Second && rt.Items[0].EndAt == 3*time.Second,
					"ttml item 0: got [%s, %s]", rt.Items[0].StartAt, rt.Items[0].EndAt)
				c.Check(rt.Items[2].StartAt == 12*time.Second && rt.Items[2].EndAt == 15*time.Second,
					"ttml item 2: got [%s, %s]", rt.Items[2].StartAt, rt.Items[2].EndAt)
				c.Check(rt.Items[0].String() == "Hello red world", "ttml item 0 text: got %q", rt.Items[0].String())
				c.Check(rt.Items[2].String() == "Blue", "ttml item 2 text: got %q", rt.Items[2].String())
				c.Check(rt.Items[0].Style != nil && rt.Items[0].Style.ID == "s1_accent", "ttml item 0 style wrong")
				c.Check(rt.Items[2].Style != nil && rt.Items[2].Style.ID == "s2_default", "ttml item 2 style wrong")
			}
			c.Check(rt.Styles["s1_default"] != nil && rt.Styles["s1_default"].InlineStyle.TTMLColor.HTMLString() == "#ff0000",
				"ttml s1_default color wrong")
			c.Check(rt.Styles["s2_default"] != nil && rt.Styles["s2_default"].InlineStyle.TTMLColor.HTMLString() == "#0000ff",
				"ttml s2_default color wrong")
			c.Check(rt.Styles["s1_accent"] != nil && rt.Styles["s1_accent"].Style == rt.Styles["s1_default"],
				"ttml s1_accent parent chain broken")
			c.Check(rt.Regions["s1_default"] != nil && rt.Regions["s1_default"].InlineStyle.TTMLOrigin != nil &&
				*rt.Regions["s1_default"].InlineStyle.TTMLOrigin == "10% 10%", "ttml s1_default origin wrong")
			c.Check(rt.Regions["s2_default"] != nil && rt.Regions["s2_default"].InlineStyle.TTMLOrigin != nil &&
				*rt.Regions["s2_default"].InlineStyle.TTMLOrigin == "20% 80%", "ttml s2_default origin wrong")
			if len(rt.Items) == 3 && len(rt.Items[0].Lines) == 1 && len(rt.Items[0].Lines[0].Items) == 3 {
				var span = rt.Items[0].Lines[0].Items[1]
				c.Check(span.Text == "red" && span.InlineStyle != nil && span.InlineStyle.TTMLColor.HTMLString() == "#ff0000",
					"ttml span: got %q", span.Text)
			} else {
				c.Check(false, "ttml item 0 lines wrong")
			}
		}

		// WebVTT round trip keeps times, text and the formatting it can
		// express
		var vttBuf bytes.Buffer
		c.Check(out.WriteToWebVTT(&vttBuf) == nil, "write webvtt failed")
		rv, err := astisub.ReadFromWebVTT(&vttBuf)
		c.Check(err == nil, "re-read webvtt failed: %v", err)
		if err == nil {
			c.Check(len(rv.Items) == 3, "webvtt: expected 3 items, got %d", len(rv.Items))
			if len(rv.Items) == 3 {
				c.Check(rv.Items[0].StartAt == time.Second && rv.Items[0].EndAt == 3*time.Second,
					"webvtt item 0: got [%s, %s]", rv.Items[0].StartAt, rv.Items[0].EndAt)
				c.Check(rv.Items[2].StartAt == 12*time.Second && rv.Items[2].EndAt == 15*time.Second,
					"webvtt item 2: got [%s, %s]", rv.Items[2].StartAt, rv.Items[2].EndAt)
				c.Check(rv.Items[0].String() == "Hello red world", "webvtt item 0 text: got %q", rv.Items[0].String())
				c.Check(rv.Items[2].String() == "Blue", "webvtt item 2 text: got %q", rv.Items[2].String())
				c.Check(rv.Items[0].Region != nil && rv.Items[0].Region.ID == "s1_default", "webvtt item 0 region wrong")
				c.Check(rv.Items[2].Region != nil && rv.Items[2].Region.ID == "s2_default", "webvtt item 2 region wrong")
				if len(rv.Items[0].Lines) == 1 && len(rv.Items[0].Lines[0].Items) == 3 {
					var span = rv.Items[0].Lines[0].Items[1]
					c.Check(span.Text == "red" && span.InlineStyle != nil && span.InlineStyle.TTMLColor.HTMLString() == "#ff0000",
						"webvtt span: got %q", span.Text)
				} else {
					c.Check(false, "webvtt item 0 lines wrong")
				}
			}
			c.Check(rv.Regions["s1_default"] != nil && rv.Regions["s1_default"].InlineStyle.WebVTTWidth == "80%" &&
				rv.Regions["s1_default"].InlineStyle.WebVTTLines == 2, "webvtt s1_default layout wrong")
			c.Check(rv.Regions["s2_default"] != nil && rv.Regions["s2_default"].InlineStyle.WebVTTWidth == "60%",
				"webvtt s2_default layout wrong")
		}

		// The CLI assembles the same edit list as the library
		var dir = t.TempDir()
		c.Check(os.WriteFile(filepath.Join(dir, "src1.vtt"), []byte("WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00:00.000,MPEGTS:900000\n\nREGION\nid:bottom\nwidth:40%\nlines:3\n\n1\n00:00:01.000 --> 00:00:03.000 region:bottom\nFirst\n\n2\n00:00:04.000 --> 00:00:05.000\nSecond\n"), 0644) == nil, "write src1 failed")
		c.Check(os.WriteFile(filepath.Join(dir, "src2.vtt"), []byte("WEBVTT\n\n1\n00:00:00.500 --> 00:00:02.000\nOther\n"), 0644) == nil, "write src2 failed")
		var manifest = "[\n" +
			"  {\"source\": \"src1.vtt\", \"in\": \"1s\", \"out\": \"3s\", \"duration\": \"4s\"},\n" +
			"  {\"source\": \"./src1.vtt\", \"in\": \"1s\", \"out\": \"3s\", \"duration\": \"4s\"},\n" +
			"  {\"source\": \"src2.vtt\", \"in\": \"0s\", \"out\": \"2s\", \"duration\": \"2s\"}\n" +
			"]\n"
		var manifestPath = filepath.Join(dir, "clips.json")
		c.Check(os.WriteFile(manifestPath, []byte(manifest), 0644) == nil, "write manifest failed")

		// Library assembly of the same edit list; the two src1.vtt entries
		// share one source identity
		lib1, err := astisub.OpenFile(filepath.Join(dir, "src1.vtt"))
		c.Check(err == nil, "open src1 failed: %v", err)
		lib2, err := astisub.OpenFile(filepath.Join(dir, "src2.vtt"))
		c.Check(err == nil, "open src2 failed: %v", err)
		libOut, err := astisub.Assemble([]astisub.EditClip{
			{Source: lib1, In: time.Second, Out: 3 * time.Second, Duration: 4 * time.Second},
			{Source: lib1, In: time.Second, Out: 3 * time.Second, Duration: 4 * time.Second},
			{Source: lib2, In: 0, Out: 2 * time.Second, Duration: 2 * time.Second},
		})
		c.Check(err == nil, "library assemble failed: %v", err)
		if err != nil {
			return
		}
		var libBuf bytes.Buffer
		c.Check(libOut.WriteToWebVTT(&libBuf) == nil, "library write failed")

		// Build and run the CLI
		var bin = filepath.Join(dir, "astisub-cli")
		b, err := exec.Command("go", "build", "-o", bin, "./cmd/astisub").CombinedOutput()
		c.Check(err == nil, "build cli failed: %v: %s", err, b)
		if err != nil {
			return
		}
		var cliPath = filepath.Join(dir, "cli-out.vtt")
		b, err = exec.Command(bin, "assemble", "-i", manifestPath, "-o", cliPath).CombinedOutput()
		c.Check(err == nil, "cli assemble failed: %v: %s", err, b)
		cliBytes, err := os.ReadFile(cliPath)
		c.Check(err == nil, "read cli output failed: %v", err)
		if err == nil {
			c.Check(bytes.Equal(cliBytes, libBuf.Bytes()), "cli and library outputs differ:\n%s\nvs\n%s", cliBytes, libBuf.Bytes())
			c.Check(strings.Contains(string(cliBytes), "id:s1_bottom"), "cli output missing s1_bottom region")
			c.Check(strings.Contains(string(cliBytes), "region:s1_bottom"), "cli output missing s1_bottom reference")
			c.Check(!strings.Contains(string(cliBytes), "s2_bottom"), "source loaded twice: %q", cliBytes)
			c.Check(!strings.Contains(string(cliBytes), "X-TIMESTAMP-MAP"), "timestamp map leaked: %q", cliBytes)
			c.Check(strings.Contains(string(cliBytes), "00:00:08.500 --> 00:00:10.000"), "cli output missing shifted cue: %q", cliBytes)
		}

		// A bad manifest fails without truncating an existing output
		var badManifests = []string{
			`[{"source": "src1.vtt", "in": "bad", "out": "3s", "duration": "4s"}]`,
			`[{"source": "missing.vtt", "in": "0s", "out": "1s", "duration": "1s"}]`,
			`[{"source": "src1.vtt", "in": "3s", "out": "1s", "duration": "1s"}]`,
			`this is not json`,
		}
		for i, bm := range badManifests {
			var bmPath = filepath.Join(dir, "bad.json")
			c.Check(os.WriteFile(bmPath, []byte(bm), 0644) == nil, "write bad manifest failed")
			var outPath = filepath.Join(dir, "existing.vtt")
			c.Check(os.WriteFile(outPath, []byte("sentinel"), 0644) == nil, "write existing output failed")
			_, err = exec.Command(bin, "assemble", "-i", bmPath, "-o", outPath).CombinedOutput()
			c.Check(err != nil, "bad manifest %d: expected the cli to fail", i)
			var content []byte
			content, err = os.ReadFile(outPath)
			c.Check(err == nil && string(content) == "sentinel", "bad manifest %d: existing output was modified", i)
		}
	})
}
