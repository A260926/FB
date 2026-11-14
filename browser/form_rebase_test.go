package browser

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"

	"github.com/PuerkitoBio/goquery"
	"github.com/headzoo/surf/task_support/taskcheck"
)

// submission is the independently parsed view of a received form post.
type submission struct {
	values    url.Values
	files     map[string][]byte
	filenames map[string]string
}

// countingReader lets a test prove Rebase never consumes an attachment while
// still allowing the real submit to read it.
type countingReader struct {
	r     io.Reader
	reads int64
}

func (c *countingReader) Read(p []byte) (int, error) {
	c.reads++
	return c.r.Read(p)
}

func strSliceEqual(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func mustParseDoc(t *testing.T, html string) *goquery.Document {
	t.Helper()
	doc, err := goquery.NewDocumentFromReader(strings.NewReader(html))
	if err != nil {
		t.Fatalf("parse test document: %v", err)
	}
	return doc
}

// rebasePage renders the multi-step form for the intent test. The hidden token
// and several defaults change between versions, while option values stay
// stable and their labels rotate.
func rebasePage(version int) string {
	token := map[int]string{1: "token-1", 2: "token-2", 3: "token-3"}[version]
	personDefault := map[int]string{1: "Alice", 2: "Server", 3: "Robot"}[version]
	cityDefault := map[int]string{1: "OldCity", 2: "NewCity", 3: "ThirdCity"}[version]
	noteDefault := map[int]string{1: "hello", 2: "hi", 3: "hey"}[version]
	planLabel1 := map[int]string{1: "Plan One", 2: "Plan Uno", 3: "Plan Ein"}[version]
	planLabel2 := map[int]string{1: "Plan Two", 2: "Plan Dos", 3: "Plan Zwei"}[version]
	planSelected := 1
	if version == 2 {
		planSelected = 2
	}
	tagsDefault := map[int]string{1: ` selected="selected"`, 2: "", 3: ` selected="selected"`}
	button := `<input type="submit" name="save" value="go">`
	if version >= 2 {
		button = `<input type="submit" name="finish" value="done">`
	}
	fresh := ""
	if version >= 2 {
		fresh = `<input type="hidden" name="fresh" value="f` + string(rune('0'+version)) + `">`
	}
	return `<!doctype html><html><body>
<form id="apply" method="post" action="/submit" enctype="multipart/form-data">
  <input type="hidden" name="token" value="` + token + `">
  <input type="text" name="person" value="` + personDefault + `">
  <input type="text" name="city" value="` + cityDefault + `">
  <input type="checkbox" name="agree" value="yes" checked="checked">
  <input type="radio" name="grade" value="a">
  <input type="radio" name="grade" value="b" checked="checked">
  <select name="plan">
    <option value="p1" ` + map[bool]string{true: "selected", false: ""}[planSelected == 1] + `>` + planLabel1 + `</option>
    <option value="p2" ` + map[bool]string{true: "selected", false: ""}[planSelected == 2] + `>` + planLabel2 + `</option>
  </select>
  <select name="tags" multiple>
    <option value="x" selected="selected">X</option>
    <option value="y" selected="selected">Y</option>
    <option value="z"` + tagsDefault[version] + `>Z</option>
  </select>
  <textarea name="note">` + noteDefault + `</textarea>
  <input type="file" name="doc">
  ` + fresh + `
  ` + button + `
</form></body></html>`
}

func newRebaseIntentServer(t *testing.T, got *submission, posts *int) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodPost:
			*posts++
			if err := r.ParseMultipartForm(1 << 20); err != nil {
				t.Errorf("parse multipart: %v", err)
			}
			got.values = make(url.Values)
			for k, vs := range r.MultipartForm.Value {
				got.values[k] = append([]string(nil), vs...)
			}
			got.files = make(map[string][]byte)
			got.filenames = make(map[string]string)
			for k, fhs := range r.MultipartForm.File {
				for _, fh := range fhs {
					fh2, err := fh.Open()
					if err != nil {
						t.Errorf("open upload: %v", err)
						continue
					}
					b, err := io.ReadAll(fh2)
					fh2.Close()
					if err != nil {
						t.Errorf("read upload: %v", err)
						continue
					}
					got.files[k] = b
					got.filenames[k] = fh.Filename
				}
			}
			io.WriteString(w, "<title>receipt</title>")
		case r.URL.Path == "/v1":
			io.WriteString(w, rebasePage(1))
		case r.URL.Path == "/v2":
			io.WriteString(w, rebasePage(2))
		case r.URL.Path == "/v3":
			io.WriteString(w, rebasePage(3))
		default:
			http.NotFound(w, r)
		}
	}))
}

// TestFormRebaseIntent covers the editing-intent semantics: explicit reverts
// versus untouched defaults, validate-before-mutate select calls, untracked
// direct slice edits, deletion intent, value-based select migration, one-shot
// attachments and consecutive rebases.
func TestFormRebaseIntent(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		got := &submission{}
		posts := 0
		ts := newRebaseIntentServer(t, got, &posts)
		defer ts.Close()

		bow := newBrowser()
		if err := bow.Open(ts.URL + "/v1"); err != nil {
			c.Check(false, "open v1: %v", err)
			return
		}
		f, err := bow.Form("#apply")
		c.Check(err == nil, "form: %v", err)
		if err != nil {
			return
		}
		form := f.(*Form)

		token, err := form.Value("token")
		c.Check(err == nil && token == "token-1", "initial token: %q %v", token, err)

		// Explicitly write back the original default: this is still an edit.
		c.Check(form.Input("person", "Alice") == nil, "input person")
		// city is deliberately never edited.

		// A failed select call must validate everything first and record no
		// edit, leaving the current selection intact.
		err = form.SelectByOptionValue("plan", "nope")
		c.Check(err != nil, "bad select value should fail")
		planVals, err := form.SelectValues("plan")
		c.Check(err == nil && strSliceEqual(planVals, []string{"p1"}), "plan unchanged after failed select: %v", planVals)

		// A failed Input records nothing either.
		c.Check(form.Input("missing", "x") != nil, "input missing should fail")
		c.Check(!form.edited["missing"], "failed input must not record an edit")

		// Direct mutation of the returned slice is not tracked.
		form.Get("city")[0] = "Hack"

		// Explicitly restore the textarea's original value.
		c.Check(form.Input("note", "hello") == nil, "input note")
		// Uncheck a box that the next page defaults to checked.
		c.Check(form.UnCheck("agree") == nil, "uncheck agree")
		// Radio selection goes through Set because serializeForm only tracks
		// checkboxes in the checkbox map.
		c.Check(form.Set("grade", "a") == nil, "set grade")
		c.Check(form.SelectByOptionLabel("plan", "Plan One") == nil, "select plan label")
		c.Check(form.RemoveValue("tags", "y") == nil, "remove tag y")

		attachment := []byte("attachment-bytes-1234")
		reader := &countingReader{r: bytes.NewReader(attachment)}
		c.Check(form.File("doc", "a.txt", reader) == nil, "set file")
		c.Check(reader.reads == 0, "File must not read the attachment, reads=%d", reader.reads)

		if err := bow.Open(ts.URL + "/v2"); err != nil {
			c.Check(false, "open v2: %v", err)
			return
		}
		c.Check(form.Rebase("#apply") == nil, "rebase v2")
		c.Check(reader.reads == 0, "Rebase must not read the attachment, reads=%d", reader.reads)
		c.Check(posts == 0, "Rebase must not submit, posts=%d", posts)

		person, _ := form.Value("person")
		c.Check(person == "Alice", "explicit revert overrides new default: %q", person)
		city, _ := form.Value("city")
		c.Check(city == "NewCity", "untracked slice edit and old default must yield the new default: %q", city)
		token, _ = form.Value("token")
		c.Check(token == "token-2", "untouched hidden token must refresh: %q", token)
		checked, err := form.IsChecked("agree")
		c.Check(err == nil && !checked, "uncheck intent must suppress the fresh checked default")
		grade, _ := form.Value("grade")
		c.Check(grade == "a", "radio intent migrated: %q", grade)
		planVals, _ = form.SelectValues("plan")
		c.Check(strSliceEqual(planVals, []string{"p1"}), "select value migrated: %v", planVals)
		planLabels, err := form.SelectLabels("plan")
		c.Check(err == nil && strSliceEqual(planLabels, []string{"Plan Uno"}), "select keeps value, adopts new labels: %v", planLabels)
		tags, _ := form.SelectValues("tags")
		c.Check(strSliceEqual(tags, []string{"x", "z"}), "final state migrated by name; removed y stays gone despite the fresh default: %v", tags)
		note, _ := form.Value("note")
		c.Check(note == "hello", "textarea revert migrated: %q", note)
		fresh, err := form.Value("fresh")
		c.Check(err == nil && fresh == "f2", "new unedited field appears with its default: %q %v", fresh, err)
		_, oldButton := form.buttons["save"]
		_, newButton := form.buttons["finish"]
		c.Check(!oldButton && newButton, "buttons come from the fresh form")
		c.Check(form.files["doc"] != nil && form.files["doc"].fileName == "a.txt", "file name migrated")
		c.Check(form.files["doc"].data == io.Reader(reader), "file reader reference migrated untouched")

		// Consecutive rebase: defaults adopted on the first rebase are still
		// not user edits.
		if err := bow.Open(ts.URL + "/v3"); err != nil {
			c.Check(false, "open v3: %v", err)
			return
		}
		c.Check(form.Rebase("#apply") == nil, "rebase v3")
		c.Check(reader.reads == 0, "second Rebase must not read the attachment, reads=%d", reader.reads)
		token, _ = form.Value("token")
		c.Check(token == "token-3", "second rebase refreshes token: %q", token)
		person, _ = form.Value("person")
		c.Check(person == "Alice", "recorded edit survives the second rebase: %q", person)
		city, _ = form.Value("city")
		c.Check(city == "ThirdCity", "adopted default is not an edit and follows the new page: %q", city)
		tags, _ = form.SelectValues("tags")
		c.Check(strSliceEqual(tags, []string{"x", "z"}), "removal intent survives the second rebase: %v", tags)
		planLabels, _ = form.SelectLabels("plan")
		c.Check(strSliceEqual(planLabels, []string{"Plan Ein"}), "option descriptions refreshed again: %v", planLabels)

		// One final submit sends the new token and the user's intent exactly
		// once, reading the attachment only now.
		c.Check(form.Click("finish") == nil, "click finish")
		c.Check(posts == 1, "exactly one post: %d", posts)
		c.Check(reader.reads > 0, "attachment read only at submit time, reads=%d", reader.reads)
		c.Check(got.values.Get("token") == "token-3", "submitted token: %q", got.values.Get("token"))
		c.Check(got.values.Get("person") == "Alice", "submitted person: %q", got.values.Get("person"))
		c.Check(got.values.Get("city") == "ThirdCity", "submitted city: %q", got.values.Get("city"))
		c.Check(got.values.Get("grade") == "a", "submitted grade: %q", got.values.Get("grade"))
		c.Check(got.values.Get("plan") == "p1", "submitted plan: %q", got.values.Get("plan"))
		c.Check(strSliceEqual(got.values["tags"], []string{"x", "z"}), "submitted tags: %v", got.values["tags"])
		c.Check(got.values.Get("note") == "hello", "submitted note: %q", got.values.Get("note"))
		_, agreeSent := got.values["agree"]
		c.Check(!agreeSent, "unchecked box must not be sent")
		c.Check(got.filenames["doc"] == "a.txt", "submitted file name: %q", got.filenames["doc"])
		c.Check(bytes.Equal(got.files["doc"], attachment), "submitted attachment bytes: %q", got.files["doc"])
		c.Check(got.values.Get("finish") == "done", "submitted button: %q", got.values.Get("finish"))
	})
}

// conflictPage renders a good v1-style form with one structural change.
func conflictPage(kind string) string {
	head := `<!doctype html><html><body><form id="apply" method="post" action="/submit" enctype="multipart/form-data">`
	tail := `<input type="submit" name="finish" value="done"></form></body></html>`
	hidden := `<input type="hidden" name="token" value="token-2">`
	switch kind {
	case "text-to-select":
		return head + hidden + `<select name="person"><option value="Ada">Ada</option></select>
<input type="checkbox" name="agree" value="yes" checked="checked">
<select name="plan"><option value="p1">P1</option></select>
<select name="tags" multiple><option value="x">X</option></select>
<input type="file" name="doc">` + tail
	case "check-to-text":
		return head + hidden + `<input type="text" name="person" value="Ada">
<input type="text" name="agree" value="yes">
<select name="plan"><option value="p1">P1</option></select>
<select name="tags" multiple><option value="x">X</option></select>
<input type="file" name="doc">` + tail
	case "select-to-multiple":
		return head + hidden + `<input type="text" name="person" value="Ada">
<input type="checkbox" name="agree" value="yes" checked="checked">
<select name="plan" multiple><option value="p1">P1</option></select>
<select name="tags" multiple><option value="x">X</option></select>
<input type="file" name="doc">` + tail
	case "file-to-text":
		return head + hidden + `<input type="text" name="person" value="Ada">
<input type="checkbox" name="agree" value="yes" checked="checked">
<select name="plan"><option value="p1">P1</option></select>
<select name="tags" multiple><option value="x">X</option></select>
<input type="text" name="doc" value="">` + tail
	case "plan-value-gone":
		return head + hidden + `<input type="text" name="person" value="Ada">
<input type="checkbox" name="agree" value="yes" checked="checked">
<select name="plan"><option value="p9">P9</option></select>
<select name="tags" multiple><option value="x">X</option></select>
<input type="file" name="doc">` + tail
	case "grade-value-gone":
		return head + hidden + `<input type="text" name="person" value="Ada">
<input type="checkbox" name="agree" value="yes" checked="checked">
<input type="radio" name="grade" value="c"><input type="radio" name="grade" value="d">
<select name="plan"><option value="p1">P1</option></select>
<select name="tags" multiple><option value="x">X</option></select>
<input type="file" name="doc">` + tail
	case "synthetic-collision":
		return head + hidden + `<input type="text" name="person" value="Ada">
<input type="text" name="synth" value="native">
<input type="file" name="up">
<input type="checkbox" name="agree" value="yes" checked="checked">
<select name="plan"><option value="p1">P1</option></select>
<select name="tags" multiple><option value="x">X</option></select>
<input type="file" name="doc">` + tail
	case "two-forms":
		return head + `</form><form id="other"></form>`
	case "not-form":
		return `<!doctype html><html><body><div id="apply"></div></body></html>`
	case "good":
		return head + hidden + `<input type="text" name="person" value="Server">
<input type="checkbox" name="agree" value="yes" checked="checked">
<input type="radio" name="grade" value="a"><input type="radio" name="grade" value="b">
<select name="plan"><option value="p1" selected>P1</option><option value="p2">P2</option></select>
<select name="tags" multiple><option value="x" selected>X</option><option value="y">Y</option><option value="z">Z</option></select>
<input type="file" name="doc">` + tail
	default:
		return head + tail
	}
}

// TestFormRebaseConflict covers selector failures, origin checks, category and
// value conflicts, synthetic field collisions and the atomicity guarantee:
// after every failure the old form is fully intact, no post is sent, and a
// corrected page lets the same Rebase succeed.
func TestFormRebaseConflict(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		got := &submission{}
		posts := 0
		ts := newRebaseIntentServer(t, got, &posts)
		defer ts.Close()
		other := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			io.WriteString(w, conflictPage("good"))
		}))
		defer other.Close()

		bow := newBrowser()
		if err := bow.Open(ts.URL + "/v1"); err != nil {
			c.Check(false, "open v1: %v", err)
			return
		}
		f, err := bow.Form("#apply")
		c.Check(err == nil, "form: %v", err)
		if err != nil {
			return
		}
		form := f.(*Form)

		// Build a rich set of recorded edits, including synthetic fields.
		c.Check(form.Input("person", "Ada") == nil, "input person")
		c.Check(form.Check("agree") == nil, "check agree")
		c.Check(form.Set("grade", "a") == nil, "set grade")
		c.Check(form.SelectByOptionValue("plan", "p1") == nil, "select plan")
		c.Check(form.RemoveValue("tags", "y") == nil, "remove tag")
		c.Check(form.Set("synth", "made-up") == nil, "synthetic set")
		form.SetFile("up", "b.bin", bytes.NewReader([]byte("bin")))
		c.Check(form.files["up"] != nil && form.files["up"].fileName == "b.bin", "synthetic file added")
		c.Check(form.File("doc", "doc.txt", bytes.NewReader([]byte("doc-bytes"))) == nil, "native file")

		oldAction := form.Action()
		oldMethod := form.Method()
		oldLabels, _ := form.SelectLabels("plan")
		oldEdited := len(form.edited)
		oldFiles := len(form.files)

		// Selector failures.
		c.Check(form.Rebase(":::not-a-selector") != nil, "invalid selector must error")
		if err := bow.Open(ts.URL + "/v2"); err != nil {
			c.Check(false, "reopen v2: %v", err)
			return
		}
		c.Check(form.Rebase("#does-not-exist") != nil, "no match must error")
		bow.state.Dom = mustParseDoc(t, conflictPage("two-forms"))
		c.Check(form.Rebase("form") != nil, "multiple matches must error")
		bow.state.Dom = mustParseDoc(t, conflictPage("not-form"))
		c.Check(form.Rebase("#apply") != nil, "non-form match must error")

		// Origin failure.
		c.Check(bow.Open(other.URL) == nil, "open other origin")
		c.Check(form.Rebase("#apply") != nil, "cross-origin rebase must error")
		c.Check(bow.Open(ts.URL+"/v2") == nil, "return to the form origin")

		// Structural and value conflicts. Each attempt must leave the whole
		// old form untouched.
		conflicts := []string{
			"text-to-select", "check-to-text", "select-to-multiple",
			"file-to-text", "plan-value-gone", "grade-value-gone",
			"synthetic-collision",
		}
		for _, kind := range conflicts {
			bow.state.Dom = mustParseDoc(t, conflictPage(kind))
			c.Check(form.Rebase("#apply") != nil, "%s: expected conflict", kind)
			c.Check(form.Action() == oldAction, "%s: action mutated", kind)
			c.Check(form.Method() == oldMethod, "%s: method mutated", kind)
			token, _ := form.Value("token")
			c.Check(token == "token-1", "%s: token mutated: %q", kind, token)
			person, _ := form.Value("person")
			c.Check(person == "Ada", "%s: values mutated: %q", kind, person)
			labels, _ := form.SelectLabels("plan")
			c.Check(strSliceEqual(labels, oldLabels), "%s: select descriptions mutated: %v", kind, labels)
			c.Check(len(form.edited) == oldEdited, "%s: edit record mutated", kind)
			c.Check(len(form.files) == oldFiles, "%s: files mutated", kind)
			c.Check(form.files["doc"].fileName == "doc.txt", "%s: native file mutated", kind)
			c.Check(form.files["up"].fileName == "b.bin", "%s: synthetic file mutated", kind)
			c.Check(form.selection != nil, "%s: dom selection lost", kind)
		}
		c.Check(posts == 0, "failed rebases must never submit, posts=%d", posts)

		// After fixing the page, the same form rebases and then submits.
		bow.state.Dom = mustParseDoc(t, conflictPage("good"))
		c.Check(form.Rebase("#apply") == nil, "retry after repair must succeed")
		token, _ := form.Value("token")
		c.Check(token == "token-2", "fresh token after repair: %q", token)
		person, _ := form.Value("person")
		c.Check(person == "Ada", "edit carried across repair: %q", person)
		tags, _ := form.SelectValues("tags")
		c.Check(strSliceEqual(tags, []string{"x", "z"}), "removal carried across repair: %v", tags)
		c.Check(form.files["doc"].fileName == "doc.txt", "native file carried across repair")
		c.Check(form.files["up"].fileName == "b.bin", "synthetic file kept when no native rival exists")

		// Old button choice was not inherited.
		c.Check(form.Click("save") != nil, "old button must be gone")
		c.Check(posts == 0, "failed click must not submit, posts=%d", posts)
		c.Check(form.Click("finish") == nil, "new button submits")
		c.Check(posts == 1, "exactly one post after repair: %d", posts)
		c.Check(got.values.Get("token") == "token-2", "post uses fresh token: %q", got.values.Get("token"))
		c.Check(got.values.Get("person") == "Ada", "post carries user value: %q", got.values.Get("person"))
	})
}

// TestFormRebaseWorkflow drives the end-to-end reopen/rebase/submit flow,
// including independent server-side verification and dual-tab document
// ownership.
func TestFormRebaseWorkflow(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var got submission
		posts := 0
		var seq int
		ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.Method == http.MethodPost {
				posts++
				if err := r.ParseMultipartForm(1 << 20); err != nil {
					t.Errorf("parse multipart: %v", err)
				}
				got.values = make(url.Values)
				for k, vs := range r.MultipartForm.Value {
					got.values[k] = append([]string(nil), vs...)
				}
				got.files = make(map[string][]byte)
				got.filenames = make(map[string]string)
				for k, fhs := range r.MultipartForm.File {
					for _, fh := range fhs {
						fh2, _ := fh.Open()
						b, _ := io.ReadAll(fh2)
						fh2.Close()
						got.files[k] = b
						got.filenames[k] = fh.Filename
					}
				}
				io.WriteString(w, "<title>receipt</title>")
				return
			}
			seq++
			token := "flow-token-" + string(rune('0'+seq))
			label := "Apply Now"
			if seq > 1 {
				label = "Apply Today"
			}
			fmt.Fprintf(w, `<!doctype html><html><body>
<form id="apply" method="post" action="/apply" enctype="multipart/form-data">
  <input type="hidden" name="token" value="%s">
  <input type="text" name="person" value="">
  <select name="plan"><option value="p1">%s</option><option value="p2">Second</option></select>
  <input type="checkbox" name="agree" value="yes">
  <input type="file" name="doc">
  <input type="submit" name="go" value="send">
</form></body></html>`, token, label)
		}))
		defer ts.Close()

		bow := newBrowser()
		c.Check(bow.Open(ts.URL+"/apply") == nil, "open step 1")
		f, err := bow.Form("#apply")
		c.Check(err == nil, "form step 1: %v", err)
		if err != nil {
			return
		}
		form := f.(*Form)
		firstToken, _ := form.Value("token")

		// User fills the multi-step form.
		c.Check(form.Input("person", "Ada") == nil, "fill person")
		c.Check(form.SelectByOptionLabel("plan", "Apply Now") == nil, "pick plan by label")
		c.Check(form.Check("agree") == nil, "check agree")
		attachment := []byte("workflow-attachment")
		c.Check(form.File("doc", "cv.bin", bytes.NewReader(attachment)) == nil, "attach doc")

		// The site reopens with a new token and rotated option labels.
		c.Check(bow.Open(ts.URL+"/apply") == nil, "reopen step 1")
		c.Check(form.Rebase("#apply") == nil, "workflow rebase")
		secondToken, _ := form.Value("token")
		c.Check(secondToken != firstToken && secondToken == "flow-token-2", "fresh token adopted: %q vs %q", secondToken, firstToken)
		labels, _ := form.SelectLabels("plan")
		c.Check(strSliceEqual(labels, []string{"Apply Today"}), "new option description adopted: %v", labels)
		plan, _ := form.SelectValues("plan")
		c.Check(strSliceEqual(plan, []string{"p1"}), "selected value kept by value: %v", plan)

		// A conflict midway must not send anything; after repair the same form
		// submits once with the fresh token.
		c.Check(posts == 0, "no posts before submit: %d", posts)
		bow.state.Dom = mustParseDoc(t, conflictPage("plan-value-gone"))
		c.Check(form.Rebase("#apply") != nil, "conflict during workflow must error")
		c.Check(posts == 0, "conflict must not submit: %d", posts)
		c.Check(bow.Open(ts.URL+"/apply") == nil, "reopen repaired page")
		c.Check(form.Rebase("#apply") == nil, "rebase after repair")
		c.Check(form.Click("go") == nil, "workflow submit")
		c.Check(posts == 1, "exactly one workflow post: %d", posts)

		// Independent server-side verification.
		c.Check(got.values.Get("token") != firstToken, "server must not receive the stale token")
		c.Check(got.values.Get("person") == "Ada", "server receives user value: %q", got.values.Get("person"))
		c.Check(got.values.Get("plan") == "p1", "server receives selected value: %q", got.values.Get("plan"))
		c.Check(got.values.Get("agree") == "yes", "server receives checked box: %q", got.values.Get("agree"))
		c.Check(got.values.Get("go") == "send", "server receives button: %q", got.values.Get("go"))
		c.Check(got.filenames["doc"] == "cv.bin", "server receives file name: %q", got.filenames["doc"])
		c.Check(bytes.Equal(got.files["doc"], attachment), "server receives exact attachment bytes")

		// Dual-tab ownership: a form stays bound to its own browser document.
		tabA := newBrowser()
		tabB := tabA.NewTab()
		c.Check(tabA != tabB, "NewTab returns a separate browser")
		c.Check(tabA.Open(ts.URL+"/tab") == nil, "tab A open")
		fa, err := tabA.Form("#apply")
		c.Check(err == nil, "tab A form: %v", err)
		formA := fa.(*Form)
		tokenA, _ := formA.Value("token")

		c.Check(tabB.Open(ts.URL+"/tab") == nil, "tab B open")
		fb, err := tabB.Form("#apply")
		c.Check(err == nil, "tab B form: %v", err)
		formB := fb.(*Form)
		tokenB, _ := formB.Value("token")
		c.Check(tokenA != tokenB, "tabs hold different documents: %q vs %q", tokenA, tokenB)

		// Navigating tab B must not choose a new document for tab A's form.
		c.Check(tabA.Open(ts.URL+"/tab") == nil, "tab A navigate")
		c.Check(formA.Rebase("#apply") == nil, "tab A rebase")
		tokenA2, _ := formA.Value("token")
		c.Check(tokenA2 != tokenA, "tab A rebases against its own document: %q vs %q", tokenA2, tokenA)
		c.Check(formB.Rebase("#apply") == nil, "tab B rebase against its own untouched document")
		tokenBAfter, _ := formB.Value("token")
		c.Check(tokenBAfter == tokenB, "tab B keeps its own document: %q vs %q", tokenBAfter, tokenB)

		// A cross-origin navigation in tab B cannot be used by tab A's form.
		foreign := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			io.WriteString(w, `<!doctype html><html><body><form id="apply"></form></body></html>`)
		}))
		defer foreign.Close()
		c.Check(tabB.Open(foreign.URL) == nil, "tab B cross-origin navigation")
		c.Check(formA.Rebase("#apply") == nil, "tab A unaffected by the other tab")
		c.Check(formB.Rebase("#apply") != nil, "tab B rejects the foreign document")
	})
}
