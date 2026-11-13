package rss_test

import (
	"strings"
	"testing"

	"github.com/mmcdole/gofeed/internal/testutil"
	"github.com/mmcdole/gofeed/rss"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

const baseRSSDoc = `<rss version="2.0" xml:base="../shared/">
<channel>
<title>Feed</title>
<link>home</link>
<description>d</description>
<docs>docs.html</docs>
<image><url>logo.png</url><link>imgpage</link><title>t</title></image>
<item xml:base="items/">
  <title>One</title>
  <link>one</link>
  <comments>c</comments>
  <enclosure url="e.mp3" length="1" type="audio/mpeg"/>
</item>
<item>
  <title>Two</title>
  <link>two</link>
</item>
<item xml:base="https://absolute.invalid/root/">
  <title>Abs</title>
  <link>z</link>
</item>
<item>
  <title>Back</title>
  <link>three</link>
</item>
</channel>
</rss>`

func TestRetrievalBaseRSS(t *testing.T) {
	const docBase = "https://example.invalid/feeds/2026/current.xml"

	t.Run("relative, nested and absolute declarations", func(t *testing.T) {
		f, err := (&rss.Parser{}).ParseWithBase(strings.NewReader(baseRSSDoc), docBase)
		require.NoError(t, err)
		require.Len(t, f.Items, 4)

		assert.Equal(t, "https://example.invalid/feeds/shared/home", f.Link)
		assert.Equal(t, "https://example.invalid/feeds/shared/docs.html", f.Docs)
		require.NotNil(t, f.Image)
		assert.Equal(t, "https://example.invalid/feeds/shared/logo.png", f.Image.URL)
		assert.Equal(t, "https://example.invalid/feeds/shared/imgpage", f.Image.Link)

		assert.Equal(t, "https://example.invalid/feeds/shared/items/one", f.Items[0].Link)
		assert.Equal(t, "https://example.invalid/feeds/shared/items/c", f.Items[0].Comments)
		require.NotNil(t, f.Items[0].Enclosure)
		assert.Equal(t, "https://example.invalid/feeds/shared/items/e.mp3", f.Items[0].Enclosure.URL)

		assert.Equal(t, "https://example.invalid/feeds/shared/two", f.Items[1].Link)
		assert.Equal(t, "https://absolute.invalid/root/z", f.Items[2].Link)
		assert.Equal(t, "https://example.invalid/feeds/shared/three", f.Items[3].Link)
	})

	t.Run("ordinary Parse is unaffected", func(t *testing.T) {
		f, err := (&rss.Parser{}).Parse(strings.NewReader(baseRSSDoc))
		require.NoError(t, err)
		require.Len(t, f.Items, 4)
		assert.Equal(t, "/shared/home", f.Link)
		assert.Equal(t, "/shared/docs.html", f.Docs)
		assert.Equal(t, "/shared/items/one", f.Items[0].Link)
		assert.Equal(t, "/shared/two", f.Items[1].Link)
		assert.Equal(t, "https://absolute.invalid/root/z", f.Items[2].Link)
		assert.Equal(t, "/shared/three", f.Items[3].Link)
	})

	t.Run("empty and invalid declarations inherit, identity text untouched", func(t *testing.T) {
		doc := `<rss version="2.0" xml:base="sub/"><channel>
<title>Fish &amp; Chips</title>
<description>plain text, no rewrite</description>
<item><guid>urn:item-1</guid><title>A</title><link>x</link></item>
<item xml:base=""><guid>urn:item-2</guid><title>B</title><link>y</link></item>
<item xml:base="http://%zz"><guid>urn:item-3</guid><title>C</title><link>z</link></item>
<item><guid isPermaLink="true">/shared/relative-guid</guid><title>D</title><link>w</link></item>
</channel></rss>`
		f, err := (&rss.Parser{}).ParseWithBase(strings.NewReader(doc), docBase)
		require.NoError(t, err)
		require.Len(t, f.Items, 4)

		assert.Equal(t, "https://example.invalid/feeds/2026/sub/x", f.Items[0].Link)
		assert.Equal(t, "https://example.invalid/feeds/2026/sub/y", f.Items[1].Link)
		assert.Equal(t, "https://example.invalid/feeds/2026/sub/z", f.Items[2].Link)

		assert.Equal(t, "Fish & Chips", f.Title)
		assert.Equal(t, "plain text, no rewrite", f.Description)
		// A GUID is an identity, not a URL to resolve, even when it looks
		// permalink-ish.
		assert.Equal(t, "urn:item-1", f.Items[0].GUID.Value)
		assert.Equal(t, "/shared/relative-guid", f.Items[3].GUID.Value)
	})

	t.Run("nested item base does not leak into the next item", func(t *testing.T) {
		// RSS has no HTML URL rewriting; this checks that a per-item xml:base
		// applies to that item's URL fields and ends with its subtree.
		doc := `<rss version="2.0" xml:base="../shared/"><channel>
<item xml:base="media/">
  <title>A</title><link>one</link><comments>c</comments>
  <enclosure url="p.mp3" length="1" type="audio/mpeg"/>
</item>
<item><title>B</title><link>two</link><comments>d</comments></item>
</channel></rss>`
		f, err := (&rss.Parser{}).ParseWithBase(strings.NewReader(doc), docBase)
		require.NoError(t, err)
		assert.Equal(t, "https://example.invalid/feeds/shared/media/one", f.Items[0].Link)
		assert.Equal(t, "https://example.invalid/feeds/shared/media/c", f.Items[0].Comments)
		assert.Equal(t, "https://example.invalid/feeds/shared/media/p.mp3", f.Items[0].Enclosure.URL)
		// Back in the channel scope.
		assert.Equal(t, "https://example.invalid/feeds/shared/two", f.Items[1].Link)
		assert.Equal(t, "https://example.invalid/feeds/shared/d", f.Items[1].Comments)
	})

	t.Run("non-UTF-8 declaration still converts", func(t *testing.T) {
		latin1 := "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>" +
			`<rss version="2.0"><channel><title>caf` + "\xe9" + `</title>` +
			`<item><link>caf` + "\xe9" + `.html</link></item></channel></rss>`
		f, err := (&rss.Parser{}).ParseWithBase(strings.NewReader(latin1), docBase)
		require.NoError(t, err)
		assert.Equal(t, "café", f.Title)
		assert.Equal(t, "https://example.invalid/feeds/2026/caf%C3%A9.html", f.Items[0].Link)
	})

	t.Run("invalid base is rejected before reading", func(t *testing.T) {
		for _, base := range []string{"", "   ", "/relative", "not a url", "ftp://example.invalid/x", "https:///nohost", "http://"} {
			_, err := (&rss.Parser{}).ParseWithBase(testutil.PanicReader{TB: t}, base)
			assert.Error(t, err, "base %q", base)
		}
	})
}
