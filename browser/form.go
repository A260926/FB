package browser

import (
	"io"
	"net/url"
	"strings"

	"github.com/PuerkitoBio/goquery"
	"github.com/headzoo/surf/errors"
	"golang.org/x/net/html"
)

// Submittable represents an element that may be submitted, such as a form.
type Submittable interface {
	Method() string
	Action() string
	Input(name, value string) error
	Set(name, value string) error

	// Remove will remove the input completely from the form.
	Remove(name string)

	// RemoveValue will remove a single instance of a form value whose name and value match.
	// This is valuable for removing a single value from a select multiple.
	RemoveValue(name, value string) error

	// Value returns the current value of a form element whose name matches.  If name is not
	// found, error is returned.  For multiple value form element such as select multiple,
	// the first value is returned.
	Value(name string) (string, error)

	// Check will set a checkbox to its active state.  This is done by adding it to
	// the form and setting its value to the value attribute defined in the form.
	Check(name string) error

	// UnCheck will set a checkbox to its inactive state.  This is done by removing
	// it from the form.
	UnCheck(name string) error

	// IsChecked returns a boolean indicating if the checkbox is active or inactive.
	IsChecked(name string) (bool, error)

	// SelectByOptionLabel sets the current value of a select form element acording to the
	// options label.  If the element is a select multiple, multiple options may be selected.
	SelectByOptionLabel(name string, optionLabel ...string) error

	// SelectByOptionValue sets the current value of a select form element acording to the
	// options value.  If the element is a select multiple, multiple options may be selected.
	SelectByOptionValue(name string, optionValue ...string) error

	// SelectValues returns the current values of a form element whose name matches.  If name is not
	// found, error is returned.  For select multiple elements, all values are returned.
	SelectValues(name string) ([]string, error)

	// SelectLabels returns the labels for the selected options for a select form element whose name
	// matches.  If name is not found, error is returned.
	SelectLabels(name string) ([]string, error)

	// File sets the value for an form input type file,
	// it returns an ElementNotFound error if the field does not exists
	File(name string, fileName string, data io.Reader) error

	// SetFile sets the value for a form input type file.
	// It will add the field to the form if necessary
	SetFile(name string, fileName string, data io.Reader)

	Click(button string) error
	ClickByValue(name, value string) error
	Submit() error
	Dom() *goquery.Selection

	// Entries returns an independent copy of the form entries in the order in
	// which they will be sent.  For file entries the value is the file name.
	// Submit buttons are excluded until one of them is clicked.  Mutating the
	// returned slice never affects the form.
	Entries() []FormEntry
}

// FormEntry is a single name/value pair collected from a successful form
// control, kept in the document order in which it will be sent.
type FormEntry struct {
	Name   string
	Value  string
	IsFile bool
}

// controlKind distinguishes the kinds of controls tracked by a form.
type controlKind int

const (
	controlField controlKind = iota
	controlCheckbox
	controlRadio
	controlSelect
	controlFile
	controlButton
)

// formSlot is one control contribution with its active state.  Disabled
// slots keep their position so the control can be reactivated in place.
type formSlot struct {
	value    string
	disabled bool
}

// optionMeta describes one enabled option of a select control.
type optionMeta struct {
	value string
	label string
}

// control is the snapshot of one form control taken when the form is created.
// One control per DOM element, so the controls slice itself is the document
// order used on the wire; later DOM mutations are never observed.
type control struct {
	name string
	kind controlKind

	// disabled records a control that was disabled in the snapshot, either
	// directly or through a disabled fieldset ancestor.  Such controls never
	// send and are ignored by the name-level mutation APIs.
	disabled bool

	// original is the control's markup-defined value, retained so Check can
	// restore checkboxes after an ordinary Set/Input overwrote them.
	original string

	// field/checkbox/radio/button/select contributions.  Disabled slots keep
	// their position but are skipped when entries are read.
	slots []formSlot

	// select metadata, kept in option order.
	multiple   bool
	options    []optionMeta
	optionVals map[string]bool
	labelToVal map[string]string

	// file metadata, nil until a file is chosen.
	file *File
}

// Form is the default form element.
type Form struct {
	bow       Browsable
	selection *goquery.Selection
	method    string
	action    string

	// controls holds one snapshot per DOM control in document tree order.
	// This is the single ordered dataset used by every mutation and
	// encoding path.
	controls []*control

	// byName indexes the snapshots by name, each group in document order.
	byName map[string][]*control
}

// NewForm creates and returns a *Form type.
func NewForm(bow Browsable, s *goquery.Selection) *Form {
	f := &Form{
		bow:       bow,
		selection: s,
		byName:    make(map[string][]*control),
	}
	f.method, f.action = formAttributes(bow, s)
	f.snapshot()
	return f
}

// Method returns the form method, eg "GET" or "POST".
func (f *Form) Method() string {
	return f.method
}

// Action returns the form action URL.
// The URL will always be absolute.
func (f *Form) Action() string {
	return f.action
}

// Entries returns an independent copy of the entries that would be sent right
// now, in document order.  Submit buttons are not included until clicked.
func (f *Form) Entries() []FormEntry {
	entries := make([]FormEntry, 0, len(f.controls))
	for _, c := range f.controls {
		if c.kind == controlButton {
			continue
		}
		entries = append(entries, f.activeEntries(c)...)
	}
	return entries
}

// activeEntries returns the currently sending entries of one control.  A
// control without an active contribution returns nil.
func (f *Form) activeEntries(c *control) []FormEntry {
	if c.kind == controlFile {
		if c.file == nil || c.file.fileName == "" {
			return nil
		}
		return []FormEntry{{Name: c.name, Value: c.file.fileName, IsFile: true}}
	}
	var entries []FormEntry
	for _, slot := range c.slots {
		if !slot.disabled {
			entries = append(entries, FormEntry{Name: c.name, Value: slot.value})
		}
	}
	return entries
}

// valueControls returns the controls sharing a name that participate in
// ordinary name/value APIs, in document order.  Buttons, file inputs and
// disabled controls are excluded.  Checkboxes, radios and selects are
// included so Input/Set reuse the very same controls Check and the select
// APIs manage, rather than maintaining a shadow list.
func (f *Form) valueControls(name string) []*control {
	var matches []*control
	for _, c := range f.byName[name] {
		if c.kind != controlButton && c.kind != controlFile && !c.disabled {
			matches = append(matches, c)
		}
	}
	return matches
}

// Input sets the value of a form field.
// it returns an ElementNotFound error if the field does not exist
func (f *Form) Input(name, value string) error {
	matches := f.valueControls(name)
	if len(matches) == 0 {
		return errors.NewElementNotFound("No input found with name '%s'.", name)
	}
	f.replaceValueGroup(matches, value)
	return nil
}

// replaceValueGroup swaps the whole ordinary value group of a name for one
// value, anchored at the position of the group's first control.
func (f *Form) replaceValueGroup(matches []*control, value string) {
	for i, c := range matches {
		if i == 0 {
			c.slots = []formSlot{{value: value}}
			continue
		}
		c.slots = nil
	}
}

// Set will set the value of a form field if it exists,
// or create and set it if it does not.
func (f *Form) Set(name, value string) error {
	if matches := f.valueControls(name); len(matches) > 0 {
		f.replaceValueGroup(matches, value)
		return nil
	}
	c := &control{
		name:  name,
		kind:  controlField,
		slots: []formSlot{{value: value}},
	}
	f.appendControl(c)
	return nil
}

// Get will return the value of a form field and `ok` - whether the field exists or not
func (f *Form) Get(name string) []string {
	var values []string
	for _, c := range f.byName[name] {
		if c.kind == controlButton {
			continue
		}
		for _, entry := range f.activeEntries(c) {
			values = append(values, entry.Value)
		}
	}
	return values
}

// Check sets the checkbox to its active state, restoring every checkbox
// control with this name in its original document position.
func (f *Form) Check(name string) error {
	matches := f.controlsOfKind(name, controlCheckbox)
	if len(matches) == 0 {
		return errors.NewElementNotFound("No checkbox found with name '%s'.", name)
	}
	for _, c := range matches {
		c.slots = []formSlot{{value: c.original}}
	}
	return nil
}

// UnCheck sets the checkbox value to inactive state.
func (f *Form) UnCheck(name string) error {
	matches := f.controlsOfKind(name, controlCheckbox)
	if len(matches) == 0 {
		return errors.NewElementNotFound("No checkbox found with name '%s'.", name)
	}
	for _, c := range matches {
		c.slots = []formSlot{{value: c.original, disabled: true}}
	}
	return nil
}

// IsChecked returns the current state of the checkbox
func (f *Form) IsChecked(name string) (bool, error) {
	matches := f.controlsOfKind(name, controlCheckbox)
	if len(matches) == 0 {
		return false, errors.NewElementNotFound("No checkbox found with name '%s'.", name)
	}
	for _, c := range matches {
		for _, slot := range c.slots {
			if !slot.disabled {
				return true, nil
			}
		}
	}
	return false, nil
}

// controlsOfKind returns the enabled controls of one kind sharing a name.
func (f *Form) controlsOfKind(name string, kind controlKind) []*control {
	var matches []*control
	for _, c := range f.byName[name] {
		if c.kind == kind && !c.disabled {
			matches = append(matches, c)
		}
	}
	return matches
}

// Remove will remove the form field if it exists.
func (f *Form) Remove(name string) {
	for _, c := range f.byName[name] {
		switch c.kind {
		case controlButton:
			continue
		case controlCheckbox, controlRadio:
			for i := range c.slots {
				c.slots[i].disabled = true
			}
		case controlFile:
			c.file = nil
		default:
			c.slots = nil
		}
	}
}

// Value returns the current value of a form element whose name matches.  If name is not
// found, error is returned.  For multiple value form element such as select multiple,
// the first value is returned.
func (f *Form) Value(name string) (string, error) {
	for _, c := range f.byName[name] {
		if c.kind == controlButton {
			continue
		}
		for _, slot := range c.slots {
			if !slot.disabled {
				return slot.value, nil
			}
		}
	}
	return "", errors.NewElementNotFound("No input found with name '%s'.", name)
}

// RemoveValue will remove a single instance of a form value whose name and value match.
// This is valuable for removing a single value from a select multiple.
// An absent name is an error; a present name without a matching value is a
// no-op, matching the historical name-level convention.
func (f *Form) RemoveValue(name, val string) error {
	matches := f.valueControls(name)
	if len(matches) == 0 {
		return errors.NewElementNotFound("No input found with name '%s'.", name)
	}
	for _, c := range matches {
		for i, slot := range c.slots {
			if !slot.disabled && slot.value == val {
				c.slots = append(c.slots[:i], c.slots[i+1:]...)
				return nil
			}
		}
	}
	return nil
}

// selectControl returns the first enabled select control sharing a name.
func (f *Form) selectControl(name string) (*control, error) {
	for _, c := range f.byName[name] {
		if c.kind == controlSelect && !c.disabled {
			return c, nil
		}
	}
	return nil, errors.NewElementNotFound("No select element found with name '%s'.", name)
}

// SelectByOptionLabel sets the current value of a select form element acording to the
// options label.  If the element is a select multiple, multiple options may be selected.
func (f *Form) SelectByOptionLabel(name string, optionLabel ...string) error {
	c, err := f.selectControl(name)
	if err != nil {
		return err
	}
	if len(optionLabel) > 1 && !c.multiple {
		return errors.NewElementNotFound("The select element with name '%s' is not a select miltiple.", name)
	}
	values := make([]string, 0, len(optionLabel))
	for _, label := range optionLabel {
		value, ok := c.labelToVal[label]
		if !ok {
			return errors.NewElementNotFound("The select element with name %q does not have an option with label %q", name, label)
		}
		values = append(values, value)
	}
	f.applySelectValues(c, values)
	return nil
}

// SelectByOptionValue sets the current value of a select form element acording to the
// options value.  If the element is a select multiple, multiple options may be selected.
func (f *Form) SelectByOptionValue(name string, optionValue ...string) error {
	c, err := f.selectControl(name)
	if err != nil {
		return err
	}
	if len(optionValue) > 1 && !c.multiple {
		return errors.NewElementNotFound("The select element with name '%s' is not a select miltiple.", name)
	}
	for _, value := range optionValue {
		if !c.optionVals[value] {
			return errors.NewElementNotFound("The select element with name %q does not have an option with value %q", name, value)
		}
	}
	f.applySelectValues(c, optionValue)
	return nil
}

// applySelectValues rebuilds the select slots from the chosen values,
// emitting them in the original option order rather than argument order.
func (f *Form) applySelectValues(c *control, values []string) {
	chosen := make(map[string]bool, len(values))
	for _, value := range values {
		chosen[value] = true
	}
	c.slots = c.slots[:0]
	for _, option := range c.options {
		if chosen[option.value] {
			c.slots = append(c.slots, formSlot{value: option.value})
		}
	}
}

// SelectValues returns the current values of a form element whose name matches.  If name is not
// found, error is returned.  For select multiple elements, all values are returned.
func (f *Form) SelectValues(name string) ([]string, error) {
	c, err := f.selectControl(name)
	if err != nil {
		return nil, err
	}
	values := make([]string, 0, len(c.slots))
	for _, slot := range c.slots {
		values = append(values, slot.value)
	}
	return values, nil
}

// SelectLabels returns the labels for the selected options for a select form element whose name
// matches.  If name is not found, error is returned.
func (f *Form) SelectLabels(name string) ([]string, error) {
	c, err := f.selectControl(name)
	if err != nil {
		return nil, err
	}
	labels := make([]string, 0, len(c.slots))
	for _, slot := range c.slots {
		for _, option := range c.options {
			if option.value == slot.value {
				labels = append(labels, option.label)
				break
			}
		}
	}
	return labels, nil
}

// File sets the value for an form input type file,
// it returns an ElementNotFound error if the field does not exists
func (f *Form) File(name string, fileName string, data io.Reader) error {
	for _, c := range f.byName[name] {
		if c.kind == controlFile && !c.disabled {
			c.file = &File{fileName: fileName, data: data}
			return nil
		}
	}
	return errors.NewElementNotFound(
		"No input type 'file' found with name '%s'.", name)
}

// SetFile sets the value for a form input type file.
// It will add the field to the form if necessary
func (f *Form) SetFile(name string, fileName string, data io.Reader) {
	for _, c := range f.byName[name] {
		if c.kind == controlFile && !c.disabled {
			c.file = &File{fileName: fileName, data: data}
			return
		}
	}
	f.appendControl(&control{
		name: name,
		kind: controlFile,
		file: &File{fileName: fileName, data: data},
	})
}

// appendControl registers a new control at the end of document order.
func (f *Form) appendControl(c *control) {
	f.controls = append(f.controls, c)
	f.byName[c.name] = append(f.byName[c.name], c)
}

// Submit submits the form.
// It clicks the first available submit button in document order, or submits
// the form without a button when the form contains none.
func (f *Form) Submit() error {
	for _, c := range f.controls {
		if c.kind == controlButton && len(c.slots) > 0 {
			return f.send(c.name, c.slots[0].value)
		}
	}
	return f.send("", "")
}

// Click submits the form by clicking the first button with the given name.
func (f *Form) Click(button string) error {
	for _, c := range f.controls {
		if c.kind == controlButton && c.name == button && len(c.slots) > 0 {
			return f.send(button, c.slots[0].value)
		}
	}
	return errors.NewInvalidFormValue(
		"Form does not contain a button with the name '%s'.", button)
}

// ClickByValue submits the form by clicking the first button matching the
// given name and value in document order.
func (f *Form) ClickByValue(name, value string) error {
	for _, c := range f.controls {
		if c.kind != controlButton || c.name != name {
			continue
		}
		for _, slot := range c.slots {
			if slot.value == value {
				return f.send(name, value)
			}
		}
	}
	return errors.NewInvalidFormValue(
		"Form does not contain a button with the name '%s' and value '%s'.", name, value)
}

// Dom returns the inner *goquery.Selection.
func (f *Form) Dom() *goquery.Selection {
	return f.selection
}

// send builds the request straight from the ordered controls, so document
// order survives all the way to the HTTP request.  When a submit button is
// clicked only that button contributes, at its original control position.
func (f *Form) send(buttonName, buttonValue string) error {
	method := f.method
	if method == "" {
		method = "GET"
	}
	aurl, err := url.Parse(f.action)
	if err != nil {
		return err
	}
	aurl = f.bow.ResolveUrl(aurl)

	entries := make([]FormEntry, 0, len(f.controls))
	buttonAdded := false
	for _, c := range f.controls {
		if c.kind == controlButton {
			// Duplicate buttons sharing a name and value contribute once:
			// the first matching control in document order.
			if !buttonAdded && c.name == buttonName && len(c.slots) > 0 && c.slots[0].value == buttonValue {
				entries = append(entries, FormEntry{Name: buttonName, Value: buttonValue})
				buttonAdded = true
			}
			continue
		}
		entries = append(entries, f.activeEntries(c)...)
	}

	if method == "GET" {
		// The form data replaces any query string already on the action.
		aurl.RawQuery = encodeEntries(entries)
		return f.bow.Open(aurl.String())
	}

	enctype, _ := f.selection.Attr("enctype")
	if strings.ToLower(enctype) == "multipart/form-data" {
		return f.bow.PostMultipartEntries(aurl.String(), entries, f.filesFor(entries))
	}
	return f.bow.PostFormEntries(aurl.String(), entries)
}

// filesFor pairs the ordered file entries with their stored readers without
// consuming them; the readers remain owned by the caller.
func (f *Form) filesFor(entries []FormEntry) []*File {
	var files []*File
	for _, entry := range entries {
		if !entry.IsFile {
			continue
		}
		found := false
		for _, c := range f.byName[entry.Name] {
			if c.kind == controlFile && c.file != nil {
				files = append(files, c.file)
				found = true
				break
			}
		}
		if !found {
			files = append(files, &File{fileName: entry.Value})
		}
	}
	return files
}

// encodeEntries builds an application/x-www-form-urlencoded query or body
// without sorting the names.  Field line breaks are normalized to CRLF.
func encodeEntries(entries []FormEntry) string {
	var builder strings.Builder
	for i, entry := range entries {
		if i > 0 {
			builder.WriteByte('&')
		}
		builder.WriteString(url.QueryEscape(entry.Name))
		builder.WriteByte('=')
		builder.WriteString(url.QueryEscape(normalizeNewlines(entry.Value)))
	}
	return builder.String()
}

// normalizeNewlines converts bare CR and LF line breaks to CRLF.  Escaping
// happens afterwards, so the CR and LF bytes reach the wire unchanged.
func normalizeNewlines(value string) string {
	if !strings.ContainsAny(value, "\r\n") {
		return value
	}
	var builder strings.Builder
	builder.Grow(len(value) + len(value)/2)
	for i := 0; i < len(value); i++ {
		switch value[i] {
		case '\r':
			builder.WriteString("\r\n")
			if i+1 < len(value) && value[i+1] == '\n' {
				i++
			}
		case '\n':
			if i > 0 && value[i-1] == '\r' {
				continue
			}
			builder.WriteString("\r\n")
		default:
			builder.WriteByte(value[i])
		}
	}
	return builder.String()
}

// snapshot collects the controls owned by the form in document tree order.
// The snapshot is fixed for the life of the form; later DOM mutations are
// ignored.
func (f *Form) snapshot() {
	formNode := f.selection.Nodes[0]
	doc := rootNode(formNode)
	if doc == nil {
		return
	}

	// The form attribute associates with the first element carrying the id,
	// and that element must itself be a form.
	firstByID := make(map[string]*html.Node)
	walkNodes(doc, func(node *html.Node) bool {
		if node.Type == html.ElementNode {
			if id, ok := attrValue(node, "id"); ok && id != "" {
				if _, seen := firstByID[id]; !seen {
					firstByID[id] = node
				}
			}
		}
		return true
	})

	walkNodes(doc, func(node *html.Node) bool {
		if node.Type != html.ElementNode || !isListedElement(node.Data) {
			return true
		}
		if owningForm(node, formNode, firstByID) != formNode {
			return true
		}
		f.collectNode(node)
		return true
	})
}

// collectNode turns an owned element node into a control snapshot.
func (f *Form) collectNode(node *html.Node) {
	name, hasName := attrValue(node, "name")
	if !hasName || name == "" {
		// Controls with no name (or an empty one) are never successful.
		return
	}
	disabled := hasAttr(node, "disabled") || disabledByAncestor(node)

	switch node.Data {
	case "input":
		f.collectInput(node, name, disabled)
	case "button":
		buttonType := strings.ToLower(nodeAttrOr(node, "type", "submit"))
		if buttonType == "submit" && !disabled {
			value, _ := attrValue(node, "value")
			f.appendControl(&control{
				name:     name,
				kind:     controlButton,
				original: value,
				slots:    []formSlot{{value: value}},
			})
		}
	case "textarea":
		if disabled {
			return
		}
		value := textContent(node)
		f.appendControl(&control{
			name:     name,
			kind:     controlField,
			original: value,
			slots:    []formSlot{{value: value}},
		})
	case "select":
		f.collectSelect(node, name, disabled)
	}
}

// collectInput handles the input element types supported by the form model.
func (f *Form) collectInput(node *html.Node, name string, disabled bool) {
	inputType := strings.ToLower(nodeAttrOr(node, "type", "text"))
	switch inputType {
	case "submit":
		if disabled {
			return
		}
		value, _ := attrValue(node, "value")
		f.appendControl(&control{
			name:     name,
			kind:     controlButton,
			original: value,
			slots:    []formSlot{{value: value}},
		})
	case "reset", "button", "image":
		// Reset and plain buttons never contribute; image inputs are not
		// supported in this revision.
		return
	case "checkbox", "radio":
		kind := controlCheckbox
		if inputType == "radio" {
			kind = controlRadio
		}
		value, _ := attrValue(node, "value")
		if value == "" {
			// A checked box without a value attribute submits "on".
			value = "on"
		}
		f.appendControl(&control{
			name:     name,
			kind:     kind,
			disabled: disabled,
			original: value,
			slots: []formSlot{{
				value:    value,
				disabled: disabled || !hasAttr(node, "checked"),
			}},
		})
	case "file":
		f.appendControl(&control{name: name, kind: controlFile, disabled: disabled})
	default:
		if disabled {
			return
		}
		value, _ := attrValue(node, "value")
		f.appendControl(&control{
			name:     name,
			kind:     controlField,
			original: value,
			slots:    []formSlot{{value: value}},
		})
	}
}

// collectSelect snapshots a select and its enabled options in option order.
func (f *Form) collectSelect(node *html.Node, name string, disabled bool) {
	c := &control{
		name:       name,
		kind:       controlSelect,
		disabled:   disabled,
		multiple:   hasAttr(node, "multiple"),
		optionVals: make(map[string]bool),
		labelToVal: make(map[string]string),
	}

	type chosenOption struct {
		value    string
		selected bool
	}
	var options []chosenOption
	walkNodes(node, func(child *html.Node) bool {
		if child.Type != html.ElementNode || child.Data != "option" {
			return true
		}
		if optionDisabled(child) {
			return false
		}
		value, ok := attrValue(child, "value")
		if !ok {
			value = textContent(child)
		}
		label := strings.TrimSpace(textContent(child))
		c.options = append(c.options, optionMeta{value: value, label: label})
		c.optionVals[value] = true
		if _, exists := c.labelToVal[label]; !exists {
			c.labelToVal[label] = value
		}
		options = append(options, chosenOption{value: value, selected: hasAttr(child, "selected")})
		return true
	})

	if !disabled {
		hasExplicit := false
		for _, option := range options {
			if option.selected {
				hasExplicit = true
				break
			}
		}
		if hasExplicit {
			for _, option := range options {
				if option.selected {
					c.slots = append(c.slots, formSlot{value: option.value})
				}
			}
		} else if !c.multiple && len(c.options) > 0 {
			// A single select with no explicit selection defaults to its
			// first enabled option; a multiple select may submit nothing.
			c.slots = append(c.slots, formSlot{value: c.options[0].value})
		}
	}
	f.appendControl(c)
}

// owningForm resolves the form owner of a listed element following the HTML
// rules: the form attribute only associates with the first element sharing
// the id, which must be a form, and never falls back to an ancestor; without
// the attribute the nearest ancestor form wins.
func owningForm(node, formNode *html.Node, firstByID map[string]*html.Node) *html.Node {
	if formID, ok := attrValue(node, "form"); ok {
		target, found := firstByID[formID]
		if !found || target.Type != html.ElementNode || target.Data != "form" {
			return nil
		}
		return target
	}
	for ancestor := node.Parent; ancestor != nil; ancestor = ancestor.Parent {
		if ancestor == formNode {
			return formNode
		}
		if ancestor.Type == html.ElementNode && ancestor.Data == "form" {
			return ancestor
		}
	}
	return nil
}

// isListedElement reports whether the tag participates in form ownership.
func isListedElement(tag string) bool {
	switch tag {
	case "input", "button", "select", "textarea":
		return true
	}
	return false
}

// optionDisabled reports whether an option is disabled directly or through a
// disabled optgroup ancestor.
func optionDisabled(option *html.Node) bool {
	if hasAttr(option, "disabled") {
		return true
	}
	for ancestor := option.Parent; ancestor != nil; ancestor = ancestor.Parent {
		if ancestor.Type == html.ElementNode && ancestor.Data == "select" {
			return false
		}
		if ancestor.Type == html.ElementNode && ancestor.Data == "optgroup" && hasAttr(ancestor, "disabled") {
			return true
		}
	}
	return false
}

// disabledByAncestor implements fieldset disabled inheritance: a disabled
// fieldset disables its descendants except those inside its first direct
// legend child.  Any other disabled ancestor on the path still applies.
func disabledByAncestor(node *html.Node) bool {
	for ancestor := node.Parent; ancestor != nil; ancestor = ancestor.Parent {
		if ancestor.Type == html.ElementNode && ancestor.Data == "fieldset" &&
			hasAttr(ancestor, "disabled") && !insideFirstLegend(node, ancestor) {
			return true
		}
	}
	return false
}

// insideFirstLegend reports whether node is a descendant of the fieldset's
// first direct legend child.
func insideFirstLegend(node, fieldset *html.Node) bool {
	var legend *html.Node
	for child := fieldset.FirstChild; child != nil; child = child.NextSibling {
		if child.Type == html.ElementNode && child.Data == "legend" {
			legend = child
			break
		}
	}
	if legend == nil {
		return false
	}
	for ancestor := node.Parent; ancestor != nil; ancestor = ancestor.Parent {
		if ancestor == legend {
			return true
		}
		if ancestor == fieldset {
			return false
		}
	}
	return false
}

// attrValue returns an attribute value and whether the attribute exists.
func attrValue(node *html.Node, key string) (string, bool) {
	for _, attr := range node.Attr {
		if attr.Key == key {
			return attr.Val, true
		}
	}
	return "", false
}

// hasAttr reports whether the element carries an attribute.  Disabled,
// checked and selected are boolean attributes: presence alone counts.
func hasAttr(node *html.Node, key string) bool {
	_, ok := attrValue(node, key)
	return ok
}

// nodeAttrOr returns an attribute value or a default.
func nodeAttrOr(node *html.Node, key, def string) string {
	if value, ok := attrValue(node, key); ok {
		return value
	}
	return def
}

// textContent returns the concatenated text of a node, the value source for
// textarea elements and value-less options.
func textContent(node *html.Node) string {
	var builder strings.Builder
	var walk func(*html.Node)
	walk = func(n *html.Node) {
		if n.Type == html.TextNode {
			builder.WriteString(n.Data)
		}
		for child := n.FirstChild; child != nil; child = child.NextSibling {
			walk(child)
		}
	}
	walk(node)
	return builder.String()
}

// rootNode walks up to the document node owning the given node.
func rootNode(node *html.Node) *html.Node {
	root := node
	for root.Parent != nil {
		root = root.Parent
	}
	return root
}

// walkNodes performs a depth-first traversal in document order.
func walkNodes(node *html.Node, visit func(*html.Node) bool) {
	if !visit(node) {
		return
	}
	for child := node.FirstChild; child != nil; child = child.NextSibling {
		walkNodes(child, visit)
	}
}

func formAttributes(bow Browsable, s *goquery.Selection) (string, string) {
	method, ok := s.Attr("method")
	if !ok {
		method = "GET"
	}
	action, ok := s.Attr("action")
	if !ok {
		action = bow.Url().String()
	}
	aurl, err := url.Parse(action)
	if err != nil {
		return "", ""
	}
	aurl = bow.ResolveUrl(aurl)

	return strings.ToUpper(method), aurl.String()
}
