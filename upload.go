package agouti

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/sclevine/agouti/api"
	"github.com/sclevine/agouti/internal/element"
)

// UploadFiles uploads one or more files to all selected
// <input type="file" /> elements. The provided filenames may be relative or
// absolute paths. Their order and any duplicates are preserved.
//
// The behavior depends on the RemoteFileUpload Option of the Page that owns
// the selection:
//
// When remote file upload is disabled (the default), the client-side
// absolute path of each file is sent to the browser directly. This requires
// the browser to run on the same machine as the client.
//
// When remote file upload is enabled (for example, when driving a remote
// Selenium Grid), every file is first transmitted to the WebDriver server
// (POST /session/<id>/se/file, one base64-encoded single-entry ZIP archive
// per list position), and each file input is filled with the newline-joined
// server-side paths returned for this call. Remote paths are never cached:
// every call re-transmits every file, even within the same session.
//
// Before anything is transmitted or any element is modified, the selection
// is resolved and every matched element is verified to be an
// <input type="file" /> (additionally requiring the multiple attribute, such
// as multiple="", when more than one file is provided), and every file is
// verified to be a readable regular file (symbolic links to regular files
// are accepted). If any of these checks fail, no element is cleared or
// filled and no file is transmitted.
//
// Failures are reported by stage. If transmitting a file fails, or the
// server returns an invalid remote path, no element is filled and the error
// identifies the file position; files already staged on the server are left
// in place. If filling an element fails after all files were staged, the
// elements filled so far keep their new values, the remaining elements are
// left untouched, and the error identifies the element position.
func (s *Selection) UploadFiles(filenames ...string) error {
	elements, err := s.elements.GetAtLeastOne()
	if err != nil {
		return fmt.Errorf("failed to select elements from %s: %s", s, err)
	}

	multiple := len(filenames) > 1
	for _, selectedElement := range elements {
		if err := s.checkFileInput(selectedElement, multiple); err != nil {
			return err
		}
	}

	if err := checkUploadFilenames(filenames); err != nil {
		return err
	}

	paths, err := s.stageUploadFiles(filenames)
	if err != nil {
		return err
	}

	value := strings.Join(paths, "\n")
	for position, selectedElement := range elements {
		if err := setUploadValue(selectedElement, value, s.remoteFileUpload); err != nil {
			return fmt.Errorf("failed to set files on element %d for %s: %s", position, s, err)
		}
	}
	return nil
}

func (s *Selection) checkFileInput(selectedElement element.Element, multiple bool) error {
	tagName, err := selectedElement.GetName()
	if err != nil {
		return fmt.Errorf("failed to determine tag name of %s: %s", s, err)
	}
	if tagName != "input" {
		return fmt.Errorf("element for %s is not an input element", s)
	}
	inputType, err := selectedElement.GetAttribute("type")
	if err != nil {
		return fmt.Errorf("failed to determine type attribute of %s: %s", s, err)
	}
	if inputType != "file" {
		return fmt.Errorf("element for %s is not a file uploader", s)
	}
	if multiple {
		apiElement, ok := selectedElement.(*api.Element)
		if !ok {
			return fmt.Errorf("element for %s does not support multiple attribute checks", s)
		}
		hasMultiple, err := apiElement.HasAttribute("multiple")
		if err != nil {
			return fmt.Errorf("failed to determine multiple attribute of %s: %s", s, err)
		}
		if !hasMultiple {
			return fmt.Errorf("element for %s does not accept multiple files", s)
		}
	}
	return nil
}

func checkUploadFilenames(filenames []string) error {
	if len(filenames) == 0 {
		return fmt.Errorf("no files provided for upload")
	}
	for _, filename := range filenames {
		if filename == "" {
			return fmt.Errorf("filename for upload may not be empty")
		}
		if strings.ContainsAny(filename, "\r\n\x00") {
			return fmt.Errorf("filename %q for upload may not contain CR, LF, or NUL characters", filename)
		}
	}
	for _, filename := range filenames {
		info, err := os.Stat(filename)
		if err != nil {
			return fmt.Errorf("failed to read file %q for upload: %s", filename, err)
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("file %q for upload is not a regular file", filename)
		}
		file, err := os.Open(filename)
		if err != nil {
			return fmt.Errorf("failed to read file %q for upload: %s", filename, err)
		}
		file.Close()
	}
	return nil
}

// stageUploadFiles resolves every list position to the path that the browser
// should receive. With remote file upload enabled, each position is
// transmitted to the WebDriver server individually (duplicates included) and
// the returned remote paths are used; otherwise the client-side absolute
// paths are used. No element is modified here.
func (s *Selection) stageUploadFiles(filenames []string) ([]string, error) {
	paths := make([]string, 0, len(filenames))
	for position, filename := range filenames {
		absFilePath, err := filepath.Abs(filename)
		if err != nil {
			return nil, fmt.Errorf("failed to find absolute path for filename: %s", err)
		}
		if !s.remoteFileUpload {
			paths = append(paths, absFilePath)
			continue
		}
		session, ok := s.session.(*api.Session)
		if !ok {
			return nil, fmt.Errorf("remote file upload for %s requires an *api.Session", s)
		}
		remotePath, err := session.UploadFile(absFilePath)
		if err != nil {
			return nil, fmt.Errorf("failed to upload file %d (%q) to remote WebDriver: %s", position, filename, err)
		}
		if remotePath == "" || strings.ContainsAny(remotePath, "\r\n\x00") {
			return nil, fmt.Errorf("invalid remote path %q returned for file %d (%q)", remotePath, position, filename)
		}
		paths = append(paths, remotePath)
	}
	return paths, nil
}

func setUploadValue(selectedElement element.Element, value string, remote bool) error {
	if remote {
		apiElement, ok := selectedElement.(*api.Element)
		if !ok {
			return fmt.Errorf("remote file upload requires *api.Element targets")
		}
		return apiElement.UploadValue(value)
	}
	return selectedElement.Value(value)
}
