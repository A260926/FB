package agouti_test

import (
	"archive/zip"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/sclevine/agouti"
)

// gridElement describes a fake DOM element served by gridServer.
type gridElement struct {
	tag   string
	attrs map[string]string
}

// gridUpload records one successful file transfer to the fake Grid.
type gridUpload struct {
	session string
	name    string
	data    []byte
	remote  string
}

// gridValue records one element value request received by the fake Grid.
type gridValue struct {
	session string
	element string
	joined  string
	text    *string
}

// gridServer is a local httptest WebDriver stand-in. It decodes the base64
// ZIP archives sent to the file upload endpoint, records the order and
// session ownership of every request, and serves configurable elements,
// find results, and failures.
type gridServer struct {
	*httptest.Server
	t *testing.T

	mu             sync.Mutex
	calls          []string
	elements       map[string]gridElement
	finds          map[string][]string
	uploads        []gridUpload
	values         []gridValue
	uploadAttempts int
	sessionsIssued int

	failUploadAt  int
	uploadMessage string
	badPathAt     int
	badPath       string
	failValueFor  map[string]string
}

func newGridServer(t *testing.T) *gridServer {
	t.Helper()
	g := &gridServer{
		t:            t,
		elements:     map[string]gridElement{},
		finds:        map[string][]string{},
		failUploadAt: -1,
		badPathAt:    -1,
		failValueFor: map[string]string{},
	}
	g.Server = httptest.NewServer(http.HandlerFunc(g.handle))
	t.Cleanup(g.Server.Close)
	return g
}

func (g *gridServer) addElement(id, tag string, attrs map[string]string) {
	g.mu.Lock()
	defer g.mu.Unlock()
	g.elements[id] = gridElement{tag: tag, attrs: attrs}
}

func (g *gridServer) addFileInput(id string, multiple bool) {
	attrs := map[string]string{"type": "file"}
	if multiple {
		attrs["multiple"] = ""
	}
	g.addElement(id, "input", attrs)
}

// addFind registers the element IDs returned for a selector. The parent is
// empty for page-level finds and an element ID for chained finds.
func (g *gridServer) addFind(parent, selector string, ids ...string) {
	g.mu.Lock()
	defer g.mu.Unlock()
	g.finds[parent+"\x1f"+selector] = ids
}

func (g *gridServer) snapshot() (calls []string, uploads []gridUpload, values []gridValue) {
	g.mu.Lock()
	defer g.mu.Unlock()
	return append([]string(nil), g.calls...), append([]gridUpload(nil), g.uploads...), append([]gridValue(nil), g.values...)
}

func (g *gridServer) writeValue(w http.ResponseWriter, status int, value interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(map[string]interface{}{"value": value}); err != nil {
		g.t.Errorf("failed to encode response: %s", err)
	}
}

func (g *gridServer) writeError(w http.ResponseWriter, status int, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(map[string]interface{}{"value": map[string]string{"message": message}}); err != nil {
		g.t.Errorf("failed to encode error response: %s", err)
	}
}

func (g *gridServer) handle(w http.ResponseWriter, r *http.Request) {
	g.mu.Lock()
	g.calls = append(g.calls, r.Method+" "+r.URL.Path)
	g.mu.Unlock()

	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/"), "/")
	switch {
	case r.Method == "GET" && r.URL.Path == "/status":
		g.writeValue(w, http.StatusOK, map[string]string{"status": "ok"})
	case r.Method == "POST" && r.URL.Path == "/session":
		g.mu.Lock()
		g.sessionsIssued++
		id := fmt.Sprintf("auto-%d", g.sessionsIssued)
		g.mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(w).Encode(map[string]interface{}{"sessionId": id, "status": 0, "value": map[string]string{}}); err != nil {
			g.t.Errorf("failed to encode session response: %s", err)
		}
	case r.Method == "DELETE" && len(parts) == 2 && parts[0] == "session":
		g.writeValue(w, http.StatusOK, nil)
	case r.Method == "POST" && len(parts) == 3 && parts[0] == "session" && parts[2] == "elements":
		g.handleFind(w, r, "")
	case r.Method == "POST" && len(parts) == 5 && parts[0] == "session" && parts[2] == "element" && parts[4] == "elements":
		g.handleFind(w, r, parts[3])
	case r.Method == "GET" && len(parts) == 5 && parts[0] == "session" && parts[2] == "element" && parts[4] == "name":
		g.mu.Lock()
		el, ok := g.elements[parts[3]]
		g.mu.Unlock()
		if !ok {
			g.writeError(w, http.StatusNotFound, "unknown element: "+parts[3])
			return
		}
		g.writeValue(w, http.StatusOK, el.tag)
	case r.Method == "GET" && len(parts) == 6 && parts[0] == "session" && parts[2] == "element" && parts[4] == "attribute":
		g.mu.Lock()
		el, ok := g.elements[parts[3]]
		g.mu.Unlock()
		if !ok {
			g.writeError(w, http.StatusNotFound, "unknown element: "+parts[3])
			return
		}
		if value, present := el.attrs[parts[5]]; present {
			g.writeValue(w, http.StatusOK, value)
		} else {
			g.writeValue(w, http.StatusOK, nil)
		}
	case r.Method == "POST" && len(parts) == 5 && parts[0] == "session" && parts[2] == "element" && parts[4] == "value":
		g.handleValue(w, r, parts[1], parts[3])
	case r.Method == "POST" && len(parts) == 4 && parts[0] == "session" && parts[2] == "se" && parts[3] == "file":
		g.handleUpload(w, r, parts[1])
	default:
		g.t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
		g.writeError(w, http.StatusNotFound, "unexpected request")
	}
}

func (g *gridServer) handleFind(w http.ResponseWriter, r *http.Request, parent string) {
	var selector struct {
		Using string `json:"using"`
		Value string `json:"value"`
	}
	if err := json.NewDecoder(r.Body).Decode(&selector); err != nil {
		g.t.Errorf("failed to decode find request: %s", err)
	}
	g.mu.Lock()
	ids := g.finds[parent+"\x1f"+selector.Value]
	g.mu.Unlock()
	refs := []map[string]string{}
	for _, id := range ids {
		refs = append(refs, map[string]string{"ELEMENT": id})
	}
	g.writeValue(w, http.StatusOK, refs)
}

func (g *gridServer) handleValue(w http.ResponseWriter, r *http.Request, session, element string) {
	var body struct {
		Value []string `json:"value"`
		Text  *string  `json:"text"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		g.t.Errorf("failed to decode value request: %s", err)
	}
	g.mu.Lock()
	g.values = append(g.values, gridValue{session: session, element: element, joined: strings.Join(body.Value, ""), text: body.Text})
	message, fail := g.failValueFor[element]
	g.mu.Unlock()
	if fail {
		g.writeError(w, http.StatusInternalServerError, message)
		return
	}
	g.writeValue(w, http.StatusOK, nil)
}

func (g *gridServer) handleUpload(w http.ResponseWriter, r *http.Request, session string) {
	var body struct {
		File string `json:"file"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		g.t.Errorf("failed to decode upload request: %s", err)
	}
	g.mu.Lock()
	attempt := g.uploadAttempts
	g.uploadAttempts++
	failAt, failMessage := g.failUploadAt, g.uploadMessage
	badAt, badPath := g.badPathAt, g.badPath
	g.mu.Unlock()
	if attempt == failAt {
		g.writeError(w, http.StatusInternalServerError, failMessage)
		return
	}
	raw, err := base64.StdEncoding.DecodeString(body.File)
	if err != nil {
		g.t.Errorf("upload %d is not valid base64: %s", attempt, err)
		g.writeError(w, http.StatusBadRequest, "invalid base64")
		return
	}
	archive, err := zip.NewReader(bytes.NewReader(raw), int64(len(raw)))
	if err != nil {
		g.t.Errorf("upload %d is not a ZIP archive: %s", attempt, err)
		g.writeError(w, http.StatusBadRequest, "invalid zip")
		return
	}
	if len(archive.File) != 1 {
		g.t.Errorf("upload %d contains %d ZIP entries, expected exactly one", attempt, len(archive.File))
		g.writeError(w, http.StatusBadRequest, "invalid entry count")
		return
	}
	entry := archive.File[0]
	reader, err := entry.Open()
	if err != nil {
		g.t.Errorf("failed to open ZIP entry of upload %d: %s", attempt, err)
		g.writeError(w, http.StatusBadRequest, "invalid entry")
		return
	}
	data, err := io.ReadAll(reader)
	reader.Close()
	if err != nil {
		g.t.Errorf("failed to read ZIP entry of upload %d: %s", attempt, err)
		g.writeError(w, http.StatusBadRequest, "invalid entry")
		return
	}
	remote := fmt.Sprintf("/remote/staged-%d/%s", attempt, entry.Name)
	if attempt == badAt {
		remote = badPath
	}
	g.mu.Lock()
	g.uploads = append(g.uploads, gridUpload{session: session, name: entry.Name, data: data, remote: remote})
	g.mu.Unlock()
	g.writeValue(w, http.StatusOK, remote)
}

func writeTempFile(t *testing.T, dir, name string, data []byte) string {
	t.Helper()
	path := filepath.Join(dir, name)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// callIndices returns the positions of every matching call in order.
func callIndices(calls []string, want string) []int {
	indices := []int{}
	for i, call := range calls {
		if call == want {
			indices = append(indices, i)
		}
	}
	return indices
}

func requireCallBefore(t *testing.T, calls []string, earlier, later string) {
	t.Helper()
	earlierIndices, laterIndices := callIndices(calls, earlier), callIndices(calls, later)
	if len(earlierIndices) == 0 || len(laterIndices) == 0 {
		t.Fatalf("expected calls %q before %q, got %v", earlier, later, calls)
	}
	if earlierIndices[len(earlierIndices)-1] > laterIndices[0] {
		t.Fatalf("expected %q before %q, got %v", earlier, later, calls)
	}
}

func TestRemoteUploadWire(t *testing.T) {
	t.Run("same-name files from two directories and binary content", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", true)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		dataA := []byte{0x00, 0x01, 0x02, 0xff, 'a', '\r', '\n'}
		dataB := []byte("报告内容\nsecond line\x00end")
		fileA := writeTempFile(t, filepath.Join(dir, "alpha"), "报告.bin", dataA)
		fileB := writeTempFile(t, filepath.Join(dir, "beta"), "报告.bin", dataB)

		page := agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
		if err := page.Find("input[type=file]").UploadFiles(fileA, fileB); err != nil {
			t.Fatalf("upload failed: %s", err)
		}

		calls, uploads, values := server.snapshot()
		if len(uploads) != 2 {
			t.Fatalf("expected 2 file transfers, got %d", len(uploads))
		}
		for i, upload := range uploads {
			if upload.session != "s1" {
				t.Errorf("upload %d reached session %q, expected s1", i, upload.session)
			}
			if upload.name != "报告.bin" {
				t.Errorf("upload %d ZIP entry is %q, expected the client basename", i, upload.name)
			}
		}
		if !bytes.Equal(uploads[0].data, dataA) {
			t.Errorf("upload 0 bytes %v do not match the first directory's file", uploads[0].data)
		}
		if !bytes.Equal(uploads[1].data, dataB) {
			t.Errorf("upload 1 bytes %v do not match the second directory's file", uploads[1].data)
		}
		if uploads[0].remote == uploads[1].remote {
			t.Errorf("same-name files from two directories were confused: %q", uploads[0].remote)
		}

		if len(values) != 1 {
			t.Fatalf("expected 1 value request, got %d", len(values))
		}
		want := uploads[0].remote + "\n" + uploads[1].remote
		if values[0].element != "f1" || values[0].session != "s1" {
			t.Errorf("value request went to %s/%s, expected s1/f1", values[0].session, values[0].element)
		}
		if values[0].joined != want {
			t.Errorf("value character array joins to %q, expected %q", values[0].joined, want)
		}
		if values[0].text == nil || *values[0].text != want {
			t.Errorf("value text field is %v, expected %q for W3C servers", values[0].text, want)
		}

		requireCallBefore(t, calls, "POST /session/s1/elements", "POST /session/s1/se/file")
		requireCallBefore(t, calls, "GET /session/s1/element/f1/attribute/multiple", "POST /session/s1/se/file")
		requireCallBefore(t, calls, "POST /session/s1/se/file", "POST /session/s1/element/f1/value")
	})

	t.Run("multiple controls and chained selection", func(t *testing.T) {
		server := newGridServer(t)
		server.addElement("panel", "div", nil)
		server.addFileInput("f1", true)
		server.addFileInput("f2", true)
		server.addFind("", "#panel", "panel")
		server.addFind("panel", "input[type=file]", "f1", "f2")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))
		fileB := writeTempFile(t, dir, "b.txt", []byte("second"))

		page := agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
		if err := page.Find("#panel").All("input[type=file]").UploadFiles(fileA, fileB); err != nil {
			t.Fatalf("upload failed: %s", err)
		}

		calls, uploads, values := server.snapshot()
		if len(uploads) != 2 {
			t.Fatalf("expected 2 file transfers shared by both controls, got %d", len(uploads))
		}
		if len(values) != 2 {
			t.Fatalf("expected 2 value requests, got %d", len(values))
		}
		want := uploads[0].remote + "\n" + uploads[1].remote
		for i, element := range []string{"f1", "f2"} {
			if values[i].element != element {
				t.Fatalf("value request %d went to element %q, expected %q (control order)", i, values[i].element, element)
			}
			if values[i].joined != want {
				t.Errorf("value request %d joins to %q, expected %q", i, values[i].joined, want)
			}
			if values[i].text == nil || *values[i].text != want {
				t.Errorf("value request %d text field is %v, expected %q", i, values[i].text, want)
			}
		}

		requireCallBefore(t, calls, "POST /session/s1/element/panel/elements", "POST /session/s1/se/file")
		requireCallBefore(t, calls, "POST /session/s1/se/file", "POST /session/s1/element/f1/value")
		requireCallBefore(t, calls, "POST /session/s1/element/f1/value", "POST /session/s1/element/f2/value")
	})

	t.Run("indexed selection from a multi selection", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("g1", false)
		server.addFileInput("g2", false)
		server.addFind("", ".attach", "g1", "g2")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("only"))

		page := agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
		if err := page.All(".attach").At(1).UploadFiles(fileA); err != nil {
			t.Fatalf("upload failed: %s", err)
		}

		_, uploads, values := server.snapshot()
		if len(uploads) != 1 {
			t.Fatalf("expected 1 file transfer, got %d", len(uploads))
		}
		if len(values) != 1 || values[0].element != "g2" {
			t.Fatalf("expected exactly one value request on g2, got %+v", values)
		}
		if values[0].joined != uploads[0].remote {
			t.Errorf("value joins to %q, expected remote path %q", values[0].joined, uploads[0].remote)
		}
	})

	t.Run("duplicate list entries are transferred separately", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", true)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("same file twice"))

		page := agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
		if err := page.Find("input[type=file]").UploadFiles(fileA, fileA); err != nil {
			t.Fatalf("upload failed: %s", err)
		}

		_, uploads, values := server.snapshot()
		if len(uploads) != 2 {
			t.Fatalf("expected the duplicate entry to be transferred twice, got %d uploads", len(uploads))
		}
		if uploads[0].remote == uploads[1].remote {
			t.Errorf("duplicate entries were collapsed into one staged file: %q", uploads[0].remote)
		}
		want := uploads[0].remote + "\n" + uploads[1].remote
		if len(values) != 1 || values[0].joined != want {
			t.Fatalf("expected one write of %q, got %+v", want, values)
		}
	})
}

func TestRemoteUploadPrepare(t *testing.T) {
	newRemotePage := func(server *gridServer) *agouti.Page {
		return agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
	}

	t.Run("empty file list", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		err := newRemotePage(server).Find("input[type=file]").UploadFiles()
		if err == nil || !strings.Contains(err.Error(), "no files") {
			t.Fatalf("expected an empty file list error, got %v", err)
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 0 || len(values) != 0 {
			t.Errorf("expected no transfers and no writes, got %d uploads and %d values", len(uploads), len(values))
		}
	})

	t.Run("invalid filename characters", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")
		page := newRemotePage(server)

		for _, name := range []string{"", "a\rb", "a\nb", "a\x00b"} {
			if err := page.Find("input[type=file]").UploadFiles(name); err == nil {
				t.Errorf("expected an error for filename %q", name)
			}
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 0 || len(values) != 0 {
			t.Errorf("expected no transfers and no writes, got %d uploads and %d values", len(uploads), len(values))
		}
	})

	t.Run("missing second file leaves earlier controls untouched", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", true)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))
		missing := filepath.Join(dir, "missing.txt")

		err := newRemotePage(server).Find("input[type=file]").UploadFiles(fileA, missing)
		if err == nil || !strings.Contains(err.Error(), "missing.txt") {
			t.Fatalf("expected a missing file error, got %v", err)
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 0 || len(values) != 0 {
			t.Errorf("expected no transfers and no writes, got %d uploads and %d values", len(uploads), len(values))
		}
	})

	t.Run("directory rejected", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		err := newRemotePage(server).Find("input[type=file]").UploadFiles(t.TempDir())
		if err == nil || !strings.Contains(err.Error(), "not a regular file") {
			t.Fatalf("expected a regular file error, got %v", err)
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 0 || len(values) != 0 {
			t.Errorf("expected no transfers and no writes, got %d uploads and %d values", len(uploads), len(values))
		}
	})

	t.Run("symlink to regular file accepted", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		target := writeTempFile(t, dir, "target.txt", []byte("via symlink"))
		link := filepath.Join(dir, "link.txt")
		if err := os.Symlink(target, link); err != nil {
			t.Fatal(err)
		}

		if err := newRemotePage(server).Find("input[type=file]").UploadFiles(link); err != nil {
			t.Fatalf("upload through symlink failed: %s", err)
		}
		_, uploads, _ := server.snapshot()
		if len(uploads) != 1 || uploads[0].name != "link.txt" || !bytes.Equal(uploads[0].data, []byte("via symlink")) {
			t.Fatalf("expected one transfer of link.txt with target bytes, got %+v", uploads)
		}
	})

	t.Run("trailing non-file control prevents any write", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addElement("f2", "input", map[string]string{"type": "text"})
		server.addFind("", ".attach", "f1", "f2")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		err := newRemotePage(server).All(".attach").UploadFiles(fileA)
		if err == nil || !strings.Contains(err.Error(), "not a file uploader") {
			t.Fatalf("expected a file uploader error, got %v", err)
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 0 || len(values) != 0 {
			t.Errorf("expected the invalid trailing control to block all writes, got %d uploads and %d values", len(uploads), len(values))
		}
	})

	t.Run("multiple files require the multiple attribute", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))
		fileB := writeTempFile(t, dir, "b.txt", []byte("second"))

		err := newRemotePage(server).Find("input[type=file]").UploadFiles(fileA, fileB)
		if err == nil || !strings.Contains(err.Error(), "multiple") {
			t.Fatalf("expected a multiple attribute error, got %v", err)
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 0 || len(values) != 0 {
			t.Errorf("expected no transfers and no writes, got %d uploads and %d values", len(uploads), len(values))
		}
	})

	t.Run("empty multiple attribute is accepted", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", true) // multiple=""
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))
		fileB := writeTempFile(t, dir, "b.txt", []byte("second"))

		if err := newRemotePage(server).Find("input[type=file]").UploadFiles(fileA, fileB); err != nil {
			t.Fatalf("upload with multiple=\"\" failed: %s", err)
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 2 || len(values) != 1 {
			t.Fatalf("expected 2 transfers and 1 write, got %d uploads and %d values", len(uploads), len(values))
		}
	})

	t.Run("empty selection", func(t *testing.T) {
		server := newGridServer(t)
		server.addFind("", "input[type=file]")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		err := newRemotePage(server).Find("input[type=file]").UploadFiles(fileA)
		if err == nil || !strings.Contains(err.Error(), "failed to select elements") {
			t.Fatalf("expected a selection error, got %v", err)
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 0 || len(values) != 0 {
			t.Errorf("expected no transfers and no writes, got %d uploads and %d values", len(uploads), len(values))
		}
	})

	t.Run("local mode fills the client absolute path", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		page := agouti.JoinPage(server.URL + "/session/s1")
		if err := page.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("local upload failed: %s", err)
		}
		calls, _, values := server.snapshot()
		if len(values) != 1 || values[0].joined != fileA {
			t.Fatalf("expected one write of the client absolute path %q, got %+v", fileA, values)
		}
		if values[0].text != nil {
			t.Errorf("local mode should use the legacy character-array-only request, got text %q", *values[0].text)
		}
		if len(callIndices(calls, "POST /session/s1/se/file")) != 0 {
			t.Errorf("local mode unexpectedly transmitted file bytes")
		}
	})

	t.Run("local mode resolves relative paths to absolute ones", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		writeTempFile(t, dir, "rel.txt", []byte("relative"))
		t.Chdir(dir)

		page := agouti.JoinPage(server.URL + "/session/s1")
		if err := page.Find("input[type=file]").UploadFiles("rel.txt"); err != nil {
			t.Fatalf("local upload failed: %s", err)
		}
		_, _, values := server.snapshot()
		want := filepath.Join(dir, "rel.txt")
		if len(values) != 1 || values[0].joined != want {
			t.Fatalf("expected one write of the absolute path %q, got %+v", want, values)
		}
	})

	t.Run("legacy UploadFile is unchanged when remote upload is disabled", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		missing := filepath.Join(t.TempDir(), "not-on-client.txt")
		page := agouti.JoinPage(server.URL + "/session/s1")
		if err := page.Find("input[type=file]").UploadFile(missing); err != nil {
			t.Fatalf("legacy UploadFile must not reject missing client files, got: %s", err)
		}
		calls, uploads, values := server.snapshot()
		if len(values) != 1 || values[0].joined != missing {
			t.Fatalf("expected one write of the original absolute path %q, got %+v", missing, values)
		}
		if len(uploads) != 0 || len(callIndices(calls, "POST /session/s1/se/file")) != 0 {
			t.Errorf("legacy UploadFile unexpectedly transmitted file bytes")
		}
	})

	t.Run("legacy UploadFile shares the remote flow when enabled", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		if err := newRemotePage(server).Find("input[type=file]").UploadFile(fileA); err != nil {
			t.Fatalf("remote UploadFile failed: %s", err)
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 1 || len(values) != 1 {
			t.Fatalf("expected 1 transfer and 1 write, got %d uploads and %d values", len(uploads), len(values))
		}
		if values[0].joined != uploads[0].remote {
			t.Errorf("value joins to %q, expected remote path %q", values[0].joined, uploads[0].remote)
		}
	})
}

func TestRemoteUploadFailure(t *testing.T) {
	t.Run("transfer failure stops before any write", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", true)
		server.addFind("", "input[type=file]", "f1")
		server.failUploadAt = 1
		server.uploadMessage = "grid staging exploded"

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))
		fileB := writeTempFile(t, dir, "b.txt", []byte("second"))

		page := agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
		err := page.Find("input[type=file]").UploadFiles(fileA, fileB)
		if err == nil {
			t.Fatal("expected a transfer failure")
		}
		if !strings.Contains(err.Error(), "file 1") || !strings.Contains(err.Error(), "grid staging exploded") {
			t.Errorf("error should identify the file position and the server error, got: %s", err)
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 1 {
			t.Errorf("expected the first file to remain staged, got %d uploads", len(uploads))
		}
		if len(values) != 0 {
			t.Errorf("expected no control to be filled, got %d writes", len(values))
		}
	})

	t.Run("empty remote path rejected", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")
		server.badPathAt = 0
		server.badPath = ""

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		page := agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
		err := page.Find("input[type=file]").UploadFiles(fileA)
		if err == nil || !strings.Contains(err.Error(), "invalid remote path") {
			t.Fatalf("expected an invalid remote path error, got %v", err)
		}
		_, _, values := server.snapshot()
		if len(values) != 0 {
			t.Errorf("expected no control to be filled, got %d writes", len(values))
		}
	})

	t.Run("remote path containing a newline rejected", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")
		server.badPathAt = 0
		server.badPath = "/remote/evil\npath"

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		page := agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
		err := page.Find("input[type=file]").UploadFiles(fileA)
		if err == nil || !strings.Contains(err.Error(), "invalid remote path") {
			t.Fatalf("expected an invalid remote path error, got %v", err)
		}
		_, _, values := server.snapshot()
		if len(values) != 0 {
			t.Errorf("expected no control to be filled, got %d writes", len(values))
		}
	})

	t.Run("value failure keeps the completed prefix without retrying", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFileInput("f2", false)
		server.addFind("", ".attach", "f1", "f2")
		server.failValueFor["f2"] = "element not interactable"

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		page := agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
		err := page.All(".attach").UploadFiles(fileA)
		if err == nil {
			t.Fatal("expected a value failure")
		}
		if !strings.Contains(err.Error(), "element 1") || !strings.Contains(err.Error(), "element not interactable") {
			t.Errorf("error should identify the control position and the server error, got: %s", err)
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 1 {
			t.Errorf("expected the file to be staged exactly once, got %d uploads", len(uploads))
		}
		if len(values) != 2 {
			t.Fatalf("expected one completed write and one failed write, got %+v", values)
		}
		if values[0].element != "f1" || values[1].element != "f2" {
			t.Errorf("expected writes on f1 then f2, got %+v", values)
		}
		if values[0].joined != uploads[0].remote {
			t.Errorf("the completed prefix was not preserved: f1 has %q, expected %q", values[0].joined, uploads[0].remote)
		}
	})
}

func TestRemoteUploadIsolation(t *testing.T) {
	t.Run("consecutive calls retransmit without caching", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		page := agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
		for i := 0; i < 2; i++ {
			if err := page.Find("input[type=file]").UploadFiles(fileA); err != nil {
				t.Fatalf("upload %d failed: %s", i, err)
			}
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 2 {
			t.Fatalf("expected the same file to be retransmitted, got %d uploads", len(uploads))
		}
		if uploads[0].remote == uploads[1].remote {
			t.Errorf("remote paths were cached across calls: %q", uploads[0].remote)
		}
		if len(values) != 2 || values[0].joined != uploads[0].remote || values[1].joined != uploads[1].remote {
			t.Errorf("each call should fill with its own remote path, got %+v and %+v", values, uploads)
		}
	})

	t.Run("two sessions stay separate", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		pageOne := agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
		pageTwo := agouti.JoinPage(server.URL+"/session/s2", agouti.RemoteFileUpload(true))
		if err := pageOne.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("first session upload failed: %s", err)
		}
		if err := pageTwo.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("second session upload failed: %s", err)
		}
		_, uploads, values := server.snapshot()
		if len(uploads) != 2 || uploads[0].session != "s1" || uploads[1].session != "s2" {
			t.Fatalf("expected one transfer per session, got %+v", uploads)
		}
		if len(values) != 2 || values[0].session != "s1" || values[1].session != "s2" {
			t.Fatalf("expected one write per session, got %+v", values)
		}
	})

	t.Run("NewPage and JoinPage option handling", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		newRemote, err := agouti.NewPage(server.URL, agouti.RemoteFileUpload(true))
		if err != nil {
			t.Fatalf("failed to open remote page: %s", err)
		}
		if err := newRemote.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("NewPage remote upload failed: %s", err)
		}

		newLocal, err := agouti.NewPage(server.URL)
		if err != nil {
			t.Fatalf("failed to open local page: %s", err)
		}
		if err := newLocal.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("NewPage local upload failed: %s", err)
		}

		joinRemote := agouti.JoinPage(server.URL+"/session/s1", agouti.RemoteFileUpload(true))
		if err := joinRemote.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("JoinPage remote upload failed: %s", err)
		}

		joinLocal := agouti.JoinPage(server.URL + "/session/s2")
		if err := joinLocal.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("JoinPage local upload failed: %s", err)
		}

		_, uploads, values := server.snapshot()
		if len(uploads) != 2 {
			t.Fatalf("expected transfers only from the two remote-enabled pages, got %+v", uploads)
		}
		if uploads[0].session != "auto-1" || uploads[1].session != "s1" {
			t.Errorf("transfers reached the wrong sessions: %+v", uploads)
		}
		if len(values) != 4 {
			t.Fatalf("expected 4 writes, got %+v", values)
		}
		if values[0].joined != uploads[0].remote || values[2].joined != uploads[1].remote {
			t.Errorf("remote-enabled pages should fill remote paths, got %+v", values)
		}
		if values[1].joined != fileA || values[3].joined != fileA {
			t.Errorf("local pages should fill the client absolute path, got %+v", values)
		}
	})

	t.Run("WebDriver defaults are inherited and overridable per page", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		driver := agouti.NewWebDriver(server.URL, []string{"sleep", "60"}, agouti.RemoteFileUpload(true))
		if err := driver.Start(); err != nil {
			t.Fatalf("failed to start WebDriver: %s", err)
		}
		defer driver.Stop()

		inherited, err := driver.NewPage()
		if err != nil {
			t.Fatalf("failed to open page: %s", err)
		}
		if err := inherited.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("inherited remote upload failed: %s", err)
		}

		overridden, err := driver.NewPage(agouti.RemoteFileUpload(false))
		if err != nil {
			t.Fatalf("failed to open page: %s", err)
		}
		if err := overridden.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("overridden local upload failed: %s", err)
		}

		stillDefault, err := driver.NewPage()
		if err != nil {
			t.Fatalf("failed to open page: %s", err)
		}
		if err := stillDefault.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("default remote upload after override failed: %s", err)
		}

		_, uploads, values := server.snapshot()
		if len(uploads) != 2 {
			t.Fatalf("expected transfers from the two remote pages only, got %+v", uploads)
		}
		if len(values) != 3 {
			t.Fatalf("expected 3 writes, got %+v", values)
		}
		if values[0].joined != uploads[0].remote || values[2].joined != uploads[1].remote {
			t.Errorf("defaulted pages should fill remote paths, got %+v", values)
		}
		if values[1].joined != fileA {
			t.Errorf("overridden page should fill the client absolute path, got %+v", values[1])
		}
		if uploads[0].session == uploads[1].session {
			t.Errorf("remote paths leaked across pages: both transfers hit %q", uploads[0].session)
		}
	})

	t.Run("WebDriver default off can be enabled per page", func(t *testing.T) {
		server := newGridServer(t)
		server.addFileInput("f1", false)
		server.addFind("", "input[type=file]", "f1")

		dir := t.TempDir()
		fileA := writeTempFile(t, dir, "a.txt", []byte("first"))

		driver := agouti.NewWebDriver(server.URL, []string{"sleep", "60"})
		if err := driver.Start(); err != nil {
			t.Fatalf("failed to start WebDriver: %s", err)
		}
		defer driver.Stop()

		enabled, err := driver.NewPage(agouti.RemoteFileUpload(true))
		if err != nil {
			t.Fatalf("failed to open page: %s", err)
		}
		if err := enabled.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("per-page remote upload failed: %s", err)
		}

		defaulted, err := driver.NewPage()
		if err != nil {
			t.Fatalf("failed to open page: %s", err)
		}
		if err := defaulted.Find("input[type=file]").UploadFiles(fileA); err != nil {
			t.Fatalf("default local upload failed: %s", err)
		}

		_, uploads, values := server.snapshot()
		if len(uploads) != 1 {
			t.Fatalf("expected a transfer from the enabled page only, got %+v", uploads)
		}
		if len(values) != 2 || values[0].joined != uploads[0].remote || values[1].joined != fileA {
			t.Fatalf("expected one remote and one local write, got %+v", values)
		}
	})
}
