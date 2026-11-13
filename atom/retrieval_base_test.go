package atom_test

import (
	"strings"
	"testing"

	"github.com/mmcdole/gofeed/atom"
	"github.com/mmcdole/gofeed/internal/testutil"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

const baseAtomDoc = `<feed xmlns="http://www.w3.org/2005/Atom" xml:base="../shared/">
<title>Feed</title>
<id>urn:feed</id>
<updated>2026-01-01T00:00:00Z</updated>
<link href="home"/>
<icon>icon.png</icon>
<logo>logo.png</logo>
<generator uri="make/">g</generator>
<author><name>A</name><uri>people/a</uri></author>
<entry xml:base="items/">
  <title>One</title>
  <id>urn:one</id>
  <link href="one"/>
  <link rel="enclosure" href="e.mp3"/>
  <content type="html">&lt;img src="pic.png"&gt;&lt;a href="page.html"&gt;x&lt;/a&gt;</content>
</entry>
<entry>
  <title>Two</title>
  <id>urn:two</id>
  <link href="two"/>
  <content type="xhtml"><div xmlns="http://www.w3.org/1999/xhtml"><img src="pic2.png"/></div></content>
</entry>
<entry xml:base="https://absolute.invalid/root/">
  <title>Abs</title>
  <id>urn:abs</id>
  <link href="z"/>
</entry>
<entry>
  <title>Back</title>
  <id>urn:back</id>
  <link href="three"/>
</entry>
</feed>`

func TestRetrievalBaseAtom(t *testing.T) {
	const docBase = "https://example.invalid/feeds/2026/current.xml"

	t.Run("relative, nested and absolute declarations", func(t *testing.T) {
		f, err := (&atom.Parser{}).ParseWithBase(strings.NewReader(baseAtomDoc), docBase)
		require.NoError(t, err)
		require.Len(t, f.Entries, 4)

		assert.Equal(t, "https://example.invalid/feeds/shared/home", f.Links[0].Href)
		assert.Equal(t, "https://example.invalid/feeds/shared/icon.png", f.Icon)
		assert.Equal(t, "https://example.invalid/feeds/shared/logo.png", f.Logo)
		assert.Equal(t, "https://example.invalid/feeds/shared/make/", f.Generator.URI)
		require.Len(t, f.Authors, 1)
		assert.Equal(t, "https://example.invalid/feeds/shared/people/a", f.Authors[0].URI)

		// Relative declaration in a nested entry resolves against the root
		// scope, then inherits into every URL field of that entry.
		assert.Equal(t, "https://example.invalid/feeds/shared/items/one", f.Entries[0].Links[0].Href)
		assert.Equal(t, "https://example.invalid/feeds/shared/items/e.mp3", f.Entries[0].Links[1].Href)
		assert.Contains(t, f.Entries[0].Content.Value, "https://example.invalid/feeds/shared/items/pic.png")
		assert.Contains(t, f.Entries[0].Content.Value, "https://example.invalid/feeds/shared/items/page.html")

		// Sibling without its own declaration keeps the parent scope.
		assert.Equal(t, "https://example.invalid/feeds/shared/two", f.Entries[1].Links[0].Href)
		assert.Contains(t, f.Entries[1].Content.Value, "https://example.invalid/feeds/shared/pic2.png")

		// Absolute declaration replaces the scope for that subtree.
		assert.Equal(t, "https://absolute.invalid/root/z", f.Entries[2].Links[0].Href)

		// Leaving the subtree restores the parent scope.
		assert.Equal(t, "https://example.invalid/feeds/shared/three", f.Entries[3].Links[0].Href)
	})

	t.Run("ordinary Parse is unaffected", func(t *testing.T) {
		f, err := (&atom.Parser{}).Parse(strings.NewReader(baseAtomDoc))
		require.NoError(t, err)
		require.Len(t, f.Entries, 4)
		assert.Equal(t, "/shared/home", f.Links[0].Href)
		assert.Equal(t, "/shared/items/one", f.Entries[0].Links[0].Href)
		assert.Equal(t, "/shared/two", f.Entries[1].Links[0].Href)
		assert.Equal(t, "https://absolute.invalid/root/z", f.Entries[2].Links[0].Href)
		assert.Equal(t, "/shared/three", f.Entries[3].Links[0].Href)
	})

	t.Run("empty and invalid declarations inherit and leave text alone", func(t *testing.T) {
		doc := `<feed xmlns="http://www.w3.org/2005/Atom" xml:base="sub/">
<title>caf&#233; &amp; news</title>
<id>urn:feed</id>
<entry xml:base=""><title>A</title><id>urn:a</id><link href="x"/></entry>
<entry xml:base="http://%zz"><title>B</title><id>urn:b</id><link href="y"/></entry>
<entry><title>C</title><id>urn:c</id><link href="z"/></entry>
</feed>`
		f, err := (&atom.Parser{}).ParseWithBase(strings.NewReader(doc), docBase)
		require.NoError(t, err)

		// An empty declaration and an unparseable one both keep the parent
		// scope rather than becoming a new base.
		assert.Equal(t, "https://example.invalid/feeds/2026/sub/x", f.Entries[0].Links[0].Href)
		assert.Equal(t, "https://example.invalid/feeds/2026/sub/y", f.Entries[1].Links[0].Href)
		assert.Equal(t, "https://example.invalid/feeds/2026/sub/z", f.Entries[2].Links[0].Href)

		// Titles, ids and identities are not addresses and stay untouched.
		assert.Equal(t, "café & news", f.Title)
		assert.Equal(t, "urn:feed", f.ID)
		assert.Equal(t, "urn:a", f.Entries[0].ID)
	})

	t.Run("content-level base scopes its HTML but not the next entry", func(t *testing.T) {
		doc := `<feed xmlns="http://www.w3.org/2005/Atom">
<title>f</title>
<id>urn:f</id>
<entry><title>A</title><id>urn:a</id><link href="one"/>
  <content type="html" xml:base="media/">&lt;img src="p.png"&gt;&lt;a href="l.html"&gt;x&lt;/a&gt;</content>
</entry>
<entry><title>B</title><id>urn:b</id><link href="two"/>
  <content type="html">&lt;img src="q.png"&gt;</content>
</entry>
</feed>`
		f, err := (&atom.Parser{}).ParseWithBase(strings.NewReader(doc), docBase)
		require.NoError(t, err)

		assert.Contains(t, f.Entries[0].Content.Value, "https://example.invalid/feeds/2026/media/p.png")
		assert.Contains(t, f.Entries[0].Content.Value, "https://example.invalid/feeds/2026/media/l.html")
		// The content's own base must not bleed into the sibling entry.
		assert.Equal(t, "https://example.invalid/feeds/2026/two", f.Entries[1].Links[0].Href)
		assert.Contains(t, f.Entries[1].Content.Value, "https://example.invalid/feeds/2026/q.png")
		assert.NotContains(t, f.Entries[1].Content.Value, "media/")
	})

	t.Run("source subtree uses its own scope", func(t *testing.T) {
		doc := `<feed xmlns="http://www.w3.org/2005/Atom" xml:base="../shared/"><title>f</title>
<entry><title>A</title><id>urn:a</id>
  <source xml:base="source/"><title>S</title><id>urn:s</id><link href="s"/></source>
</entry>
<entry><title>B</title><id>urn:b</id><link href="b"/></entry>
</feed>`
		f, err := (&atom.Parser{}).ParseWithBase(strings.NewReader(doc), docBase)
		require.NoError(t, err)
		require.NotNil(t, f.Entries[0].Source)
		assert.Equal(t, "https://example.invalid/feeds/shared/source/s", f.Entries[0].Source.Links[0].Href)
		assert.Equal(t, "https://example.invalid/feeds/shared/b", f.Entries[1].Links[0].Href)
	})

	t.Run("base fragment is dropped before resolution", func(t *testing.T) {
		doc := `<feed xmlns="http://www.w3.org/2005/Atom"><title>f</title><id>urn:f</id>` +
			`<entry><title>A</title><id>urn:a</id><link href="x"/></entry></feed>`
		f, err := (&atom.Parser{}).ParseWithBase(strings.NewReader(doc), "https://example.invalid/dir/feed.xml#section")
		require.NoError(t, err)
		assert.Equal(t, "https://example.invalid/dir/x", f.Entries[0].Links[0].Href)
	})

	t.Run("non-UTF-8 declaration still converts", func(t *testing.T) {
		latin1 := "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>" +
			`<feed xmlns="http://www.w3.org/2005/Atom"><title>caf` + "\xe9" + `</title>` +
			`<entry><title>E</title><id>urn:e</id><link href="caf` + "\xe9" + `.html"/></entry></feed>`
		f, err := (&atom.Parser{}).ParseWithBase(strings.NewReader(latin1), docBase)
		require.NoError(t, err)
		assert.Equal(t, "café", f.Title)
		assert.Equal(t, "https://example.invalid/feeds/2026/caf%C3%A9.html", f.Entries[0].Links[0].Href)
	})

	t.Run("invalid base is rejected before reading", func(t *testing.T) {
		for _, base := range []string{"", "   ", "/relative", "not a url", "ftp://example.invalid/x", "https:///nohost", "http://"} {
			_, err := (&atom.Parser{}).ParseWithBase(testutil.PanicReader{TB: t}, base)
			assert.Error(t, err, "base %q", base)
		}
	})
}
