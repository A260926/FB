package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"

	"github.com/sclevine/agouti"
)

func require(ok bool, message string) {
	if !ok {
		panic(message)
	}
}

func main() {
	dir, err := os.MkdirTemp("", "agouti-upload-baseline-")
	if err != nil {
		panic(err)
	}
	defer os.RemoveAll(dir)
	filename := filepath.Join(dir, "资料.txt")
	if err := os.WriteFile(filename, []byte("attachment\x00bytes\n"), 0600); err != nil {
		panic(err)
	}
	mode := "local"
	var writes []string
	var trace []string
	uploads := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		trace = append(trace, r.Method+" "+r.URL.Path)
		var value interface{}
		status := http.StatusOK
		switch {
		case r.URL.Path == "/session/s/elements" && r.Method == "POST":
			ids := []string{"first"}
			if mode == "mixed" {
				ids = append(ids, "second")
			}
			refs := []map[string]string{}
			for _, id := range ids {
				refs = append(refs, map[string]string{"ELEMENT": id})
			}
			value = refs
		case strings.HasSuffix(r.URL.Path, "/name"):
			value = "input"
		case strings.HasSuffix(r.URL.Path, "/attribute/type"):
			value = "file"
			if mode == "mixed" && strings.Contains(r.URL.Path, "/second/") {
				value = "text"
			}
		case strings.HasSuffix(r.URL.Path, "/value") && r.Method == "POST":
			var body struct {
				Value []string `json:"value"`
			}
			if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
				panic(err)
			}
			path := strings.Join(body.Value, "")
			require(path == filename, "old UploadFile changed the supplied absolute filename")
			writes = append(writes, path)
			if mode == "remote" {
				status = http.StatusBadRequest
				value = map[string]string{"message": "remote browser cannot see client path"}
			}
		case strings.HasSuffix(r.URL.Path, "/file"):
			uploads++
			panic("baseline unexpectedly transmitted attachment bytes")
		default:
			panic("unexpected endpoint: " + r.URL.Path)
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(status)
		if err := json.NewEncoder(w).Encode(map[string]interface{}{"value": value}); err != nil {
			panic(err)
		}
	}))
	defer server.Close()
	page := agouti.JoinPage(server.URL + "/session/s")
	err = page.Find("input[type=file]").UploadFile(filename)
	require(err == nil && len(writes) == 1, fmt.Sprintf("local upload failed: %v", err))
	fmt.Println("BASELINE local-existing-file: one value request with original absolute path")
	mode, writes, trace = "remote", nil, nil
	err = page.Find("input[type=file]").UploadFile(filename)
	require(err != nil && strings.Contains(err.Error(), "remote browser cannot see client path") && len(writes) == 1 && uploads == 0, "remote limitation not observed")
	fmt.Println("BASELINE separate-host: local path sent, no file transfer, remote rejected")
	mode, writes, trace = "mixed", nil, nil
	err = page.All(".attachments").UploadFile(filename)
	require(err != nil && strings.Contains(err.Error(), "not a file uploader") && len(writes) == 1, "mixed target prefix not observed")
	firstWrite, laterCheck := -1, -1
	for i, call := range trace {
		if call == "POST /session/s/element/first/value" {
			firstWrite = i
		}
		if call == "GET /session/s/element/second/attribute/type" {
			laterCheck = i
		}
	}
	require(firstWrite >= 0 && laterCheck > firstWrite, "missing ordering witness")
	fmt.Println("BASELINE mixed-targets: first written before second rejected")
}
