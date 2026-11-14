package browser

import (
	"net/url"
	"strings"

	"github.com/PuerkitoBio/goquery"
	"github.com/andybalholm/cascadia"
	"github.com/headzoo/surf/errors"
)

// Control categories used when matching an edited native control against the
// fresh form snapshot. Same name but a different category set is a conflict.
const (
	categoryValue      = "value"          // text-like inputs, hidden inputs and textareas
	categoryCheckable  = "checkbox-radio" // checkbox and radio inputs
	categorySelectOne  = "select-one"
	categorySelectMany = "select-multiple"
	categoryFile       = "file"
)

// controlInfo describes every native control sharing one name inside a form.
// The traversal mirrors serializeForm exactly: the same element selectors, the
// same disabled handling and no external controls.
type controlInfo struct {
	categories map[string]bool
	// multiple is -1 when no select carries the name, 0 for a single select
	// and 1 for a select multiple.
	multiple int
	// offered holds the values a checkable control can send, or the option
	// values a select offers.
	offered []string
}

func newControlInfo() *controlInfo {
	return &controlInfo{categories: make(map[string]bool), multiple: -1}
}

// Rebase re-snapshots the form from the current document of the browser the
// form belongs to, without sending any request or reloading the page. The form
// in the current document matching expr must be unique.
//
// A fresh snapshot of the matched form is taken, and only the names the user
// explicitly edited through Input, Set, Check, UnCheck, SelectByOptionLabel,
// SelectByOptionValue, File, SetFile, Remove or RemoveValue are migrated onto
// it. Editing intent is recorded per name: changing a field back to its
// original value still counts as an edit and overrides the new page default,
// while names that were never edited - hidden tokens included - keep whatever
// the fresh snapshot provides. New buttons, action, method, enctype and select
// option descriptions all come from the fresh form; button choices are not
// carried over, so the next Click looks among the new buttons.
//
// Rebase is atomic: any conflict returns an error and leaves the form, its
// editing record and the current browser document untouched. The address the
// form was captured from and the current document must be same-origin (scheme,
// host and effective port). Edited native controls must still exist in the
// fresh form with the same name and control category, and every value that
// would still be sent must still be offered by the fresh control. Synthetic
// fields created with Set/SetFile that have no native counterpart in the
// original document are kept, unless the fresh page introduces a native control
// with the same name, which is a conflict. File attachments migrate by file
// name and io.Reader reference only; the readers are never read, closed or
// rewound. After the page has been fixed Rebase may be tried again.
func (f *Form) Rebase(expr string) error {
	if f.bow == nil || f.bow.Url() == nil {
		return errors.NewPageNotLoaded("Cannot rebase the form, no page is loaded.")
	}
	currentURL := f.bow.Url()
	if !sameOrigin(f.action, currentURL) {
		return errors.NewLocation(
			"Cannot rebase the form from a document on a different origin: form action %q vs page %q.",
			f.action, currentURL.String())
	}

	matcher, err := cascadia.Compile(expr)
	if err != nil {
		return errors.NewElementNotFound("Invalid form selector %q: %s.", expr, err.Error())
	}
	matches := f.bow.Dom().FindMatcher(matcher)
	switch matches.Length() {
	case 0:
		return errors.NewElementNotFound("Form not found matching expr '%s'.", expr)
	case 1:
	default:
		return errors.NewElementNotFound(
			"Expr '%s' matches %d forms, it must match exactly one.", expr, matches.Length())
	}
	if !matches.Is("form") {
		return errors.NewElementNotFound("Expr '%s' does not match a form tag.", expr)
	}

	// Build the complete replacement snapshot first. Nothing on f is touched
	// until every edited control has been validated, keeping the operation
	// atomic.
	snapshot := NewForm(f.bow, matches)
	if err := f.migrateEdits(snapshot); err != nil {
		return err
	}

	f.selection = snapshot.selection
	f.method = snapshot.method
	f.action = snapshot.action
	f.fields = snapshot.fields
	f.buttons = snapshot.buttons
	f.checkboxs = snapshot.checkboxs
	f.selects = snapshot.selects
	f.files = snapshot.files
	f.edited = snapshot.edited
	f.editedFiles = snapshot.editedFiles
	return nil
}

// migrateEdits validates and applies every recorded edit onto snapshot, which
// starts as an untouched serialization of the fresh form.
func (f *Form) migrateEdits(snapshot *Form) error {
	oldCatalog := catalogControls(f.selection)
	newCatalog := catalogControls(snapshot.selection)

	for name := range f.edited {
		oldInfo, oldNative := oldCatalog[name]
		newInfo, newNative := newCatalog[name]

		if !oldNative {
			// Synthetic field created with Set. It survives only while the
			// fresh page has not claimed the name for a real control.
			if newNative {
				return errors.NewInvalidFormValue(
					"Cannot rebase field '%s': the new page defines a native %s control with the same name.",
					name, describeControl(newInfo))
			}
			if vals, ok := f.fields[name]; ok {
				snapshot.fields[name] = append([]string(nil), vals...)
			} else {
				// A synthetic name whose final state is absent carries a
				// deletion intent.
				snapshot.fields.Del(name)
			}
			snapshot.edited[name] = true
			continue
		}

		if !newNative || !sameControlKind(oldInfo, newInfo) {
			return errors.NewInvalidFormValue(
				"Cannot rebase field '%s': control category changed from %s to %s.",
				name, describeControl(oldInfo), describeControlOrMissing(newInfo, newNative))
		}

		switch {
		case oldInfo.categories[categoryCheckable]:
			if err := migrateCheckable(f, snapshot, name, newInfo); err != nil {
				return err
			}
		case oldInfo.multiple >= 0:
			if err := migrateSelect(f, snapshot, name, newInfo); err != nil {
				return err
			}
		case oldInfo.categories[categoryFile]:
			if err := migrateFile(f, snapshot, name); err != nil {
				return err
			}
		default:
			migrateValues(f, snapshot, name)
		}
		snapshot.edited[name] = true
	}

	// File edits are recorded separately because attachments never live in
	// fields. A name already handled above is not migrated twice.
	for name := range f.editedFiles {
		if f.edited[name] {
			continue
		}
		oldInfo, oldNative := oldCatalog[name]
		newInfo, newNative := newCatalog[name]
		if !oldNative {
			// Synthetic attachment created with SetFile.
			if newNative {
				return errors.NewInvalidFormValue(
					"Cannot rebase file field '%s': the new page defines a native %s control with the same name.",
					name, describeControl(newInfo))
			}
		} else if !newNative || !sameControlKind(oldInfo, newInfo) {
			return errors.NewInvalidFormValue(
				"Cannot rebase file field '%s': control category changed from %s to %s.",
				name, describeControl(oldInfo), describeControlOrMissing(newInfo, newNative))
		}
		if err := migrateFile(f, snapshot, name); err != nil {
			return err
		}
		snapshot.editedFiles[name] = true
	}
	return nil
}

// catalogControls walks a form selection using the same rules as
// serializeForm and returns the native controls keyed by name.
func catalogControls(sel *goquery.Selection) map[string]*controlInfo {
	catalog := make(map[string]*controlInfo)
	sel.Find("input,button,textarea").Each(func(_ int, s *goquery.Selection) {
		if v, ok := s.Attr("disabled"); ok && strings.ToLower(v) == "disabled" {
			return
		}
		name, ok := s.Attr("name")
		if !ok {
			return
		}
		t, _ := s.Attr("type")
		t = strings.ToLower(t)
		if t == "submit" {
			return
		}
		info := catalog[name]
		if info == nil {
			info = newControlInfo()
			catalog[name] = info
		}
		switch {
		case t == "checkbox" || t == "radio":
			info.categories[categoryCheckable] = true
			if val, ok := s.Attr("value"); ok {
				info.offered = appendUnique(info.offered, val)
			}
		case t == "file":
			info.categories[categoryFile] = true
		default:
			info.categories[categoryValue] = true
		}
	})

	sel.Find("select").Each(func(_ int, s *goquery.Selection) {
		if v, ok := s.Attr("disabled"); ok && strings.ToLower(v) == "disabled" {
			return
		}
		name, ok := s.Attr("name")
		if !ok {
			return
		}
		info := catalog[name]
		if info == nil {
			info = newControlInfo()
			catalog[name] = info
		}
		if _, multiple := s.Attr("multiple"); multiple {
			info.multiple = 1
			info.categories[categorySelectMany] = true
		} else {
			info.multiple = 0
			info.categories[categorySelectOne] = true
		}
		s.Find("option").Each(func(_ int, opt *goquery.Selection) {
			val, _ := opt.Attr("value")
			info.offered = appendUnique(info.offered, val)
		})
	})
	return catalog
}

func sameControlKind(a, b *controlInfo) bool {
	if len(a.categories) != len(b.categories) || a.multiple != b.multiple {
		return false
	}
	for category := range a.categories {
		if !b.categories[category] {
			return false
		}
	}
	return true
}

func describeControl(info *controlInfo) string {
	var categories []string
	for category := range info.categories {
		categories = append(categories, category)
	}
	if len(categories) == 0 {
		return "unknown"
	}
	return strings.Join(categories, "+")
}

func describeControlOrMissing(info *controlInfo, native bool) string {
	if !native {
		return "missing"
	}
	return describeControl(info)
}

func appendUnique(values []string, value string) []string {
	for _, v := range values {
		if v == value {
			return values
		}
	}
	return append(values, value)
}

func migrateValues(f, snapshot *Form, name string) {
	vals, ok := f.fields[name]
	if !ok {
		// Removal intent: suppress the fresh default for this name.
		snapshot.fields.Del(name)
		return
	}
	snapshot.fields[name] = append([]string(nil), vals...)
}

func migrateCheckable(f, snapshot *Form, name string, newInfo *controlInfo) error {
	oldVals, sending := f.fields[name]
	if !sending {
		// Explicit uncheck: even a fresh default checked state is suppressed.
		snapshot.fields.Del(name)
		return nil
	}
	for _, v := range oldVals {
		if !contains(newInfo.offered, v) {
			return errors.NewInvalidFormValue(
				"Cannot rebase checkable '%s': value %q is no longer offered by the new form.", name, v)
		}
	}
	snapshot.fields[name] = append([]string(nil), oldVals...)
	return nil
}

func migrateSelect(f, snapshot *Form, name string, newInfo *controlInfo) error {
	chosen, ok := f.fields[name]
	if !ok {
		// Explicit clearing of the select suppresses the fresh default
		// selection.
		snapshot.fields.Del(name)
		return nil
	}
	for _, v := range chosen {
		if !contains(newInfo.offered, v) {
			return errors.NewInvalidFormValue(
				"Cannot rebase select '%s': selected value %q is no longer an option.", name, v)
		}
	}
	snapshot.fields[name] = append([]string(nil), chosen...)
	return nil
}

func migrateFile(f, snapshot *Form, name string) error {
	file, ok := f.files[name]
	if !ok {
		snapshot.files[name] = &File{}
		return nil
	}
	// Only the file name and Reader reference move; the Reader is never read,
	// closed or rewound.
	snapshot.files[name] = &File{fileName: file.fileName, data: file.data}
	return nil
}

func contains(values []string, value string) bool {
	for _, v := range values {
		if v == value {
			return true
		}
	}
	return false
}

// sameOrigin reports whether target (the form's absolute action URL) and the
// browser's current document URL share scheme, host and effective port.
func sameOrigin(target string, current *url.URL) bool {
	parsed, err := url.Parse(target)
	if err != nil || !parsed.IsAbs() {
		return false
	}
	if !strings.EqualFold(parsed.Scheme, current.Scheme) {
		return false
	}
	if !strings.EqualFold(parsed.Hostname(), current.Hostname()) {
		return false
	}
	return effectivePort(parsed) == effectivePort(current)
}

func effectivePort(u *url.URL) string {
	if p := u.Port(); p != "" {
		return p
	}
	switch strings.ToLower(u.Scheme) {
	case "https":
		return "443"
	case "http":
		return "80"
	default:
		return ""
	}
}
