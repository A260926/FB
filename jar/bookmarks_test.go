package jar

import (
	"github.com/headzoo/surf/internal/testassert"
	"os"
	"testing"
)

func TestMemoryBookmarks(t *testing.T) {

	b := NewMemoryBookmarks()
	assertBookmarks(t, b)
}

func TestFileBookmarks(t *testing.T) {

	b, err := NewFileBookmarks("./bookmarks.json")
	testassert.Nil(t, err)
	defer func() {
		err = os.Remove("./bookmarks.json")
	}()
	assertBookmarks(t, b)
}

// assertBookmarks tests the given bookmark jar.
func assertBookmarks(t *testing.T, b BookmarksJar) {
	err := b.Save("test1", "http://localhost")
	testassert.Nil(t, err)
	err = b.Save("test2", "http://127.0.0.1")
	testassert.Nil(t, err)
	err = b.Save("test1", "http://localhost")
	testassert.NotNil(t, err)

	url, err := b.Read("test1")
	testassert.Nil(t, err)
	testassert.Equal(t, "http://localhost", url)
	url, err = b.Read("test2")
	testassert.Equal(t, "http://127.0.0.1", url)
	url, err = b.Read("test3")
	testassert.NotNil(t, err)

	r := b.Remove("test2")
	testassert.True(t, r)
	r = b.Remove("test3")
	testassert.False(t, r)

	r = b.Has("test1")
	testassert.True(t, r)
	r = b.Has("test4")
	testassert.False(t, r)
}
