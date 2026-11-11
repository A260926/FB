package oggvorbis_test

import (
	"errors"
	"github.com/jfreymuth/oggvorbis"
	"os"
	"testing"
)

var testSeekError = errors.New("one-shot seek error")

type testFaultSource struct {
	*os.File
	armed bool
}

func (f *testFaultSource) Seek(off int64, whence int) (int64, error) {
	if f.armed {
		f.armed = false
		return 0, testSeekError
	}
	return f.File.Seek(off, whence)
}
func TestRegressSeekEOF(t *testing.T) {
	f, e := os.Open("testdata/long.ogg")
	if e != nil {
		t.Fatal(e)
	}
	defer f.Close()
	r, e := oggvorbis.NewReader(f)
	if e != nil {
		t.Fatal(e)
	}
	if e = r.SetPosition(600000); e != nil {
		t.Fatal(e)
	}
	if r.Position() != 600000 {
		t.Fatal(r.Position())
	}
	if e = r.SetPosition(r.Length()); e != nil {
		t.Fatal(e)
	}
	if r.Position() != r.Length() {
		t.Fatalf("EOF position=%d want=%d", r.Position(), r.Length())
	}
}
func TestRegressSeekFault(t *testing.T) {
	f, e := os.Open("testdata/long.ogg")
	if e != nil {
		t.Fatal(e)
	}
	defer f.Close()
	in := &testFaultSource{File: f}
	r, e := oggvorbis.NewReader(in)
	if e != nil {
		t.Fatal(e)
	}
	if _, e = r.Read(make([]float32, 17)); e != nil {
		t.Fatal(e)
	}
	before := r.Position()
	in.armed = true
	e = r.SetPosition(200000)
	if !errors.Is(e, testSeekError) {
		t.Fatalf("seek error=%v want one-shot fault; position before=%d after=%d", e, before, r.Position())
	}
	if r.Position() != before {
		t.Fatalf("position after failure=%d want=%d", r.Position(), before)
	}
}
