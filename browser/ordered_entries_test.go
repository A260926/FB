package browser

import (
	"bytes"
	"io"
	"mime"
	"mime/multipart"
	"net/http"
	"net/url"
	"strings"
	"testing"

	surferrors "github.com/headzoo/surf/errors"
	"github.com/headzoo/surf/internal/testassert"
)

// capturedRequest is the independently decoded record of one sent request.
type capturedRequest struct {
	method      string
	rawQuery    string
	contentType string
	body        []byte
}

// captureTransport records every request without opening any network socket.
type captureTransport struct {
	html string
	last *capturedRequest
}

func (c *captureTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	captured := &capturedRequest{
		method:      req.Method,
		rawQuery:    req.URL.RawQuery,
		contentType: req.Header.Get("Content-Type"),
	}
	if req.Body != nil {
		body, err := io.ReadAll(req.Body)
		if err != nil {
			return nil, err
		}
		captured.body = body
	}
	c.last = captured

	return &http.Response{
		StatusCode: 200,
		Status:     "200 OK",
		Header:     http.Header{"Content-Type": {"text/html"}},
		Body:       io.NopCloser(strings.NewReader(c.html)),
		Request:    req,
	}, nil
}

// newCaptureBrowser opens the given page behind a recording transport.
func newCaptureBrowser(t *testing.T, pageHTML string) (*Browser, *captureTransport) {
	t.Helper()
	bow := newBrowser()
	transport := &captureTransport{html: pageHTML}
	bow.SetTransport(transport)
	testassert.Nil(t, bow.Open("http://capture.invalid/form"))
	return bow, transport
}

// capturedPart is one multipart part in wire order.
type capturedPart struct {
	name     string
	filename string
	data     []byte
}

// decodeMultipart reads the parts back in the order they were written.
func decodeMultipart(t *testing.T, contentType string, body []byte) []capturedPart {
	t.Helper()
	mediaType, params, err := mime.ParseMediaType(contentType)
	testassert.Nil(t, err)
	testassert.Equal(t, "multipart/form-data", mediaType)

	reader := multipart.NewReader(bytes.NewReader(body), params["boundary"])
	var parts []capturedPart
	for {
		part, err := reader.NextPart()
		if err == io.EOF {
			break
		}
		testassert.Nil(t, err)
		data, err := io.ReadAll(part)
		testassert.Nil(t, err)
		parts = append(parts, capturedPart{
			name:     part.FormName(),
			filename: part.FileName(),
			data:     data,
		})
	}
	return parts
}

// entryNames renders the ordered entries as name/value pairs for assertions.
func entryNames(entries []FormEntry) []string {
	pairs := make([]string, 0, len(entries))
	for _, entry := range entries {
		pairs = append(pairs, entry.Name+"="+entry.Value)
	}
	return pairs
}

func TestEntryOwnership(t *testing.T) {
	t.Run("form attribute ownership and reassigned descendants", func(t *testing.T) {
		page := `
<html><body>
	<input form="f" name="outside" value="A">
	<form id="f" method="post" action="/p">
		<input name="inside" value="1">
		<input form="nowhere" name="stranded" value="S">
	</form>
	<form id="g">
		<input form="f" name="reassigned" value="R">
		<input name="onlyG" value="G">
	</form>
	<div id="notAForm"></div>
	<input form="missing" name="orphan" value="O">
	<input form="notAForm" name="wrongType" value="W">
</body></html>`
		bow, _ := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		// External association is included, reassigned descendants are pulled
		// in, while failed associations never fall back to an ancestor form.
		testassert.Equal(t, []string{
			"outside=A", "inside=1", "reassigned=R",
		}, entryNames(form.Entries()))

		// The returned slice is independent: mutating it leaves the form alone.
		entries := form.Entries()
		entries[0] = FormEntry{Name: "tampered", Value: "x"}
		testassert.Equal(t, "outside", form.Entries()[0].Name)

		other, err := bow.Form("#g")
		testassert.Nil(t, err)
		testassert.Equal(t, []string{"onlyG=G"}, entryNames(other.Entries()))
	})

	t.Run("fieldset disabled inheritance with legend exemption", func(t *testing.T) {
		page := `
<html><body>
<form id="f" method="post" action="/p">
	<input name="plain" value="1">
	<fieldset disabled>
		<input name="blocked" value="bad">
		<legend><input name="legendField" value="ok"></legend>
	</fieldset>
	<fieldset>
		<legend><input name="outerLegend" value="lo"></legend>
		<fieldset disabled>
			<legend><input name="innerLegend" value="li"></legend>
			<input name="deepBlocked" value="no">
		</fieldset>
	</fieldset>
	<fieldset disabled>
		<fieldset>
			<legend><input name="trapped" value="t"></legend>
		</fieldset>
	</fieldset>
	<input disabled name="selfDisabled" value="x">
</form>
</body></html>`
		bow, _ := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		testassert.Equal(t, []string{
			"plain=1", "legendField=ok", "outerLegend=lo", "innerLegend=li",
		}, entryNames(form.Entries()))
	})

	t.Run("boolean attributes, empty names, buttons and textarea content", func(t *testing.T) {
		page := `
<html><body>
<form id="f" method="post" action="/p">
	<input type="checkbox" name="bareChecked" checked>
	<input type="checkbox" name="noValue" checked value="">
	<input type="checkbox" name="uncheckedBox" value="x">
	<input type="radio" name="r" value="a" checked>
	<input type="radio" name="r" value="b">
	<input type="reset" name="resetBtn" value="reset">
	<button type="button" name="plainBtn" value="b">x</button>
	<input name="" value="noname">
	<textarea name="memo">hello
world</textarea>
	<input type="submit" name="go" value="1">
</form>
</body></html>`
		bow, _ := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		// Bare checked attributes count, value-less checkboxes send "on",
		// unchecked radios/checkboxes, reset/button controls and empty names
		// stay out, and the textarea contributes its text content.
		testassert.Equal(t, []string{
			"bareChecked=on", "noValue=on", "r=a", "memo=hello\nworld",
		}, entryNames(form.Entries()))
	})
}

func TestEntryMutation(t *testing.T) {
	t.Run("repeated values keep order through set, input and removal", func(t *testing.T) {
		page := `
<html><body>
<form id="f" method="post" action="/p">
	<input name="z" value="1">
	<input name="a" value="2">
	<input name="z" value="3">
</form>
</body></html>`
		bow, _ := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		testassert.Equal(t, []string{"z=1", "a=2", "z=3"}, entryNames(form.Entries()))

		testassert.Nil(t, form.Input("a", "9"))
		testassert.Equal(t, []string{"z=1", "a=9", "z=3"}, entryNames(form.Entries()))

		// A new name is appended at the end.
		testassert.Nil(t, form.Set("new", "x"))
		testassert.Equal(t, []string{"z=1", "a=9", "z=3", "new=x"}, entryNames(form.Entries()))

		// Set replaces the whole repeated group with one value anchored at the
		// position of the group's first slot.
		testassert.Nil(t, form.Set("z", "only"))
		testassert.Equal(t, []string{"z=only", "a=9", "new=x"}, entryNames(form.Entries()))

		testassert.Nil(t, form.RemoveValue("new", "x"))
		testassert.Equal(t, []string{"z=only", "a=9"}, entryNames(form.Entries()))

		form.Remove("a")
		testassert.Equal(t, []string{"z=only"}, entryNames(form.Entries()))

		// Input on a missing name still errors instead of appending.
		testassert.Equal(t,
			surferrors.NewElementNotFound("No input found with name '%s'.", "ghost"),
			form.Input("ghost", "v"))
	})

	t.Run("check restores every checkbox slot in its original position", func(t *testing.T) {
		page := `
<html><body>
<form id="f" method="post" action="/p">
	<input type="checkbox" name="c" value="1">
	<input name="t" value="x">
	<input type="checkbox" name="c" value="2">
</form>
</body></html>`
		bow, _ := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		testassert.Equal(t, []string{"t=x"}, entryNames(form.Entries()))

		testassert.Nil(t, form.Check("c"))
		checked, err := form.IsChecked("c")
		testassert.Nil(t, err)
		testassert.Equal(t, true, checked)
		testassert.Equal(t, []string{"c=1", "t=x", "c=2"}, entryNames(form.Entries()))

		testassert.Nil(t, form.UnCheck("c"))
		checked, err = form.IsChecked("c")
		testassert.Nil(t, err)
		testassert.Equal(t, false, checked)
		testassert.Equal(t, []string{"t=x"}, entryNames(form.Entries()))

		// Re-checking restores both slots again, not a single merged value.
		testassert.Nil(t, form.Check("c"))
		testassert.Equal(t, []string{"c=1", "t=x", "c=2"}, entryNames(form.Entries()))
	})

	t.Run("selects follow option order and honor disabled options", func(t *testing.T) {
		page := `
<html><body>
<form id="f" method="post" action="/p">
	<select name="s">
		<option value="1">One</option>
		<option value="2" disabled>Two</option>
		<option value="3">Three</option>
	</select>
	<select name="m" multiple>
		<optgroup label="g" disabled><option value="a">A</option></optgroup>
		<option value="b">B</option>
		<option value="c">C</option>
	</select>
</form>
</body></html>`
		bow, _ := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		// Single select defaults to the first enabled option; a multiple
		// select with nothing selected sends nothing.
		testassert.Equal(t, []string{"s=1"}, entryNames(form.Entries()))

		testassert.Nil(t, form.SelectByOptionValue("s", "3"))
		testassert.Equal(t, []string{"s=3"}, entryNames(form.Entries()))

		err = form.SelectByOptionValue("s", "2")
		testassert.Equal(t,
			surferrors.NewElementNotFound("The select element with name %q does not have an option with value %q", "s", "2"),
			err)

		// Disabled optgroup options cannot be chosen.
		err = form.SelectByOptionValue("m", "a")
		testassert.Equal(t,
			surferrors.NewElementNotFound("The select element with name %q does not have an option with value %q", "m", "a"),
			err)

		// Arguments out of order are still emitted in option order.
		testassert.Nil(t, form.SelectByOptionValue("m", "c", "b"))
		testassert.Equal(t, []string{"s=3", "m=b", "m=c"}, entryNames(form.Entries()))

		values, err := form.SelectValues("m")
		testassert.Nil(t, err)
		testassert.Equal(t, []string{"b", "c"}, values)
		labels, err := form.SelectLabels("m")
		testassert.Nil(t, err)
		testassert.Equal(t, []string{"B", "C"}, labels)

		testassert.Nil(t, form.SelectByOptionLabel("m", "C"))
		testassert.Equal(t, []string{"s=3", "m=c"}, entryNames(form.Entries()))

		// A single select rejects multiple choices.
		err = form.SelectByOptionValue("s", "1", "3")
		testassert.Equal(t,
			surferrors.NewElementNotFound("The select element with name '%s' is not a select miltiple.", "s"),
			err)

		// Removing one value of a multiple select keeps the remaining value in
		// place; removing the last value drops the name entirely.
		testassert.Nil(t, form.SelectByOptionValue("m", "b", "c"))
		testassert.Nil(t, form.RemoveValue("m", "c"))
		testassert.Equal(t, []string{"s=3", "m=b"}, entryNames(form.Entries()))
		testassert.Nil(t, form.RemoveValue("m", "b"))
		testassert.Equal(t, []string{"s=3"}, entryNames(form.Entries()))
	})

	t.Run("files stay in their control slot and unknown files append", func(t *testing.T) {
		page := `
<html><body>
<form id="f" method="post" enctype="multipart/form-data" action="/p">
	<input type="text" name="note" value="n">
	<input type="file" name="upload">
	<input type="submit" name="go" value="1">
</form>
</body></html>`
		bow, _ := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		err = form.File("missing", "x.bin", strings.NewReader("x"))
		testassert.Equal(t,
			surferrors.NewElementNotFound("No input type 'file' found with name '%s'.", "missing"),
			err)

		first := []byte("first-bytes")
		testassert.Nil(t, form.File("upload", "first.png", bytes.NewReader(first)))
		testassert.Equal(t, []string{
			"note=n", "upload=first.png",
		}, entryNames(form.Entries()))

		// An unknown file name appends a new slot at the end, after the button.
		extra := []byte("extra-bytes")
		form.SetFile("extra", "extra.bin", bytes.NewReader(extra))
		entries := form.Entries()
		testassert.Equal(t, "extra", entries[len(entries)-1].Name)
		testassert.Equal(t, true, entries[len(entries)-1].IsFile)

		// Replacing the known file keeps its original control position.
		second := []byte("second-bytes")
		form.SetFile("upload", "second.png", bytes.NewReader(second))
		testassert.Equal(t, "upload", form.Entries()[1].Name)
	})
}

func TestEntryWireOrder(t *testing.T) {
	t.Run("GET keeps order and replaces the action query string", func(t *testing.T) {
		page := `
<html><body>
<form id="f" method="get" action="/search?old=gone">
	<input name="z" value="1">
	<input name="a" value="2">
	<input name="z" value="3">
	<input type="file" name="upload">
	<input type="submit" name="go" value="1">
</form>
</body></html>`
		bow, transport := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		form.SetFile("upload", "report.png", strings.NewReader("ignored-on-get"))
		testassert.Nil(t, form.Click("go"))

		captured := transport.last
		testassert.Equal(t, "GET", captured.method)
		testassert.Equal(t,
			"z=1&a=2&z=3&upload=report.png&go=1",
			captured.rawQuery)
		testassert.Equal(t, "", string(captured.body))

		// Decode independently: repeated parameters survive in order and the
		// action's original query is gone.
		parsed, err := url.ParseQuery(captured.rawQuery)
		testassert.Nil(t, err)
		testassert.Equal(t, []string{"1", "3"}, parsed["z"])
		testassert.Equal(t, "2", parsed.Get("a"))
		testassert.Equal(t, "report.png", parsed.Get("upload"))
		if _, ok := parsed["old"]; ok {
			t.Fatalf("action query string was not replaced: %v", parsed)
		}
	})

	t.Run("urlencoded POST keeps raw order and normalizes newlines", func(t *testing.T) {
		page := "<html><body><form id=\"f\" method=\"post\" action=\"/p\">" +
			"<input name=\"z\" value=\"1\"><input name=\"a\" value=\"2\">" +
			"<input name=\"z\" value=\"3\">" +
			"<textarea name=\"memo\">line1\nline2</textarea>" +
			"<input type=\"submit\" name=\"go\" value=\"1\"></form></body></html>"
		bow, transport := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		testassert.Nil(t, form.Submit())
		captured := transport.last
		testassert.Equal(t, "POST", captured.method)
		testassert.Equal(t,
			"application/x-www-form-urlencoded",
			captured.contentType)
		wantBody := "z=1&a=2&z=3&memo=line1%0D%0Aline2&go=1"
		testassert.Equal(t, wantBody, string(captured.body))

		parsed, err := url.ParseQuery(string(captured.body))
		testassert.Nil(t, err)
		testassert.Equal(t, []string{"1", "3"}, parsed["z"])
		testassert.Equal(t, "line1\r\nline2", parsed.Get("memo"))
		testassert.Equal(t, "1", parsed.Get("go"))
	})

	t.Run("multipart POST interleaves fields and files in entry order", func(t *testing.T) {
		page := `
<html><body>
<form id="f" method="post" enctype="multipart/form-data" action="/p">
	<input type="text" name="before" value="b">
	<input type="file" name="firstFile">
	<input type="text" name="middle" value="m">
	<input type="file" name="secondFile">
	<input type="submit" name="go" value="1">
</form>
</body></html>`
		bow, transport := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		firstBytes := []byte("abc\x00\x01\x02def")
		secondBytes := []byte("second-file-content")
		testassert.Nil(t, form.File("firstFile", "one.bin", bytes.NewReader(firstBytes)))
		testassert.Nil(t, form.File("secondFile", "two.bin", bytes.NewReader(secondBytes)))

		// Click the second submit value path to confirm only the clicked button
		// is sent, positioned at its original slot.
		testassert.Nil(t, form.Click("go"))
		captured := transport.last
		testassert.Equal(t, "POST", captured.method)

		parts := decodeMultipart(t, captured.contentType, captured.body)
		testassert.Equal(t, 5, len(parts))
		testassert.Equal(t, capturedPart{name: "before", data: []byte("b")}, parts[0])
		testassert.Equal(t, "firstFile", parts[1].name)
		testassert.Equal(t, "one.bin", parts[1].filename)
		testassert.Equal(t, firstBytes, parts[1].data)
		testassert.Equal(t, capturedPart{name: "middle", data: []byte("m")}, parts[2])
		testassert.Equal(t, "secondFile", parts[3].name)
		testassert.Equal(t, "two.bin", parts[3].filename)
		testassert.Equal(t, secondBytes, parts[3].data)
		testassert.Equal(t, capturedPart{name: "go", data: []byte("1")}, parts[4])

		// A second, independent send decodes the same ordered payload again.
		// Readers stay owned by the caller, so a fresh reader is attached for
		// the resend rather than the form buffering attachment bytes.
		testassert.Nil(t, form.File("firstFile", "one.bin", bytes.NewReader(firstBytes)))
		testassert.Nil(t, form.File("secondFile", "two.bin", bytes.NewReader(secondBytes)))
		testassert.Nil(t, form.Submit())
		again := decodeMultipart(t, transport.last.contentType, transport.last.body)
		testassert.Equal(t, len(parts), len(again))
		for i := range parts {
			testassert.Equal(t, parts[i].name, again[i].name)
			testassert.Equal(t, parts[i].filename, again[i].filename)
			testassert.Equal(t, parts[i].data, again[i].data)
		}
	})

	t.Run("clicked duplicate button sends the first matching value", func(t *testing.T) {
		page := `
<html><body>
<form id="f" method="post" action="/p">
	<input name="field" value="v">
	<input type="submit" name="go" value="first">
	<input type="submit" name="go" value="second">
</form>
</body></html>`
		bow, transport := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		// Submit chooses the first available submit button in tree order.
		testassert.Nil(t, form.Submit())
		testassert.Equal(t, "field=v&go=first", string(transport.last.body))

		// Clicking the name also takes the first button.
		testassert.Nil(t, form.Click("go"))
		testassert.Equal(t, "field=v&go=first", string(transport.last.body))

		// An explicit value selects that button while keeping its slot.
		testassert.Nil(t, form.ClickByValue("go", "second"))
		testassert.Equal(t, "field=v&go=second", string(transport.last.body))

		err = form.ClickByValue("go", "missing")
		testassert.Equal(t,
			surferrors.NewInvalidFormValue("Form does not contain a button with the name '%s' and value '%s'.", "go", "missing"),
			err)
	})

	t.Run("duplicate same-value button is only sent once", func(t *testing.T) {
		page := `
<html><body>
<form id="f" method="post" action="/p">
	<input name="field" value="v">
	<input type="submit" name="go" value="same">
	<input type="submit" name="go" value="same">
</form>
</body></html>`
		bow, transport := newCaptureBrowser(t, page)
		form, err := bow.Form("#f")
		testassert.Nil(t, err)

		// Submit, Click and ClickByValue all resolve to the first matching
		// button; the duplicate never produces a second parameter.
		testassert.Nil(t, form.Submit())
		testassert.Equal(t, "field=v&go=same", string(transport.last.body))
		testassert.Nil(t, form.Click("go"))
		testassert.Equal(t, "field=v&go=same", string(transport.last.body))
		testassert.Nil(t, form.ClickByValue("go", "same"))
		testassert.Equal(t, "field=v&go=same", string(transport.last.body))
	})
}
