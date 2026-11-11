package agouti

import (
	"github.com/sclevine/agouti/api"
	"github.com/sclevine/agouti/internal/element"
	"github.com/sclevine/agouti/internal/target"
)

type Selectors interface {
	String() string
}

type selectable struct {
	session          apiSession
	selectors        target.Selectors
	remoteFileUpload bool
}

type apiSession interface {
	element.Client
	Delete() error
	GetActiveElement() (*api.Element, error)
	GetWindow() (*api.Window, error)
	GetWindows() ([]*api.Window, error)
	SetWindow(window *api.Window) error
	SetWindowByName(name string) error
	DeleteWindow() error
	GetScreenshot() ([]byte, error)
	GetCookies() ([]*api.Cookie, error)
	SetCookie(cookie *api.Cookie) error
	DeleteCookie(name string) error
	DeleteCookies() error
	GetURL() (string, error)
	SetURL(url string) error
	GetTitle() (string, error)
	GetSource() (string, error)
	MoveTo(element *api.Element, point api.Offset) error
	Frame(frame *api.Element) error
	FrameParent() error
	Execute(body string, arguments []interface{}, result interface{}) error
	Forward() error
	Back() error
	Refresh() error
	GetAlertText() (string, error)
	SetAlertText(text string) error
	AcceptAlert() error
	DismissAlert() error
	NewLogs(logType string) ([]api.Log, error)
	GetLogTypes() ([]string, error)
	DoubleClick() error
	Click(button api.Button) error
	ButtonDown(button api.Button) error
	ButtonUp(button api.Button) error
	TouchDown(x, y int) error
	TouchUp(x, y int) error
	TouchMove(x, y int) error
	TouchClick(element *api.Element) error
	TouchDoubleClick(element *api.Element) error
	TouchLongClick(element *api.Element) error
	TouchFlick(element *api.Element, offset api.Offset, speed api.Speed) error
	TouchScroll(element *api.Element, offset api.Offset) error
	DeleteLocalStorage() error
	DeleteSessionStorage() error
	SetImplicitWait(timout int) error
	SetPageLoad(timout int) error
	SetScriptTimeout(timout int) error
}

// Find finds exactly one element by CSS selector.
func (s *selectable) Find(selector string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.CSS, selector).Single(), s.remoteFileUpload)
}

// FindByXPath finds exactly one element by XPath selector.
func (s *selectable) FindByXPath(selector string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.XPath, selector).Single(), s.remoteFileUpload)
}

// FindByLink finds exactly one anchor element by its text content.
func (s *selectable) FindByLink(text string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.Link, text).Single(), s.remoteFileUpload)
}

// FindByLabel finds exactly one element by associated label text.
func (s *selectable) FindByLabel(text string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.Label, text).Single(), s.remoteFileUpload)
}

// FindByButton finds exactly one button element with the provided text.
// Supports <button>, <input type="button">, and <input type="submit">.
func (s *selectable) FindByButton(text string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.Button, text).Single(), s.remoteFileUpload)
}

// FindByName finds exactly element with the provided name attribute.
func (s *selectable) FindByName(name string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.Name, name).Single(), s.remoteFileUpload)
}

// FindByClass finds exactly one element with a given CSS class.
func (s *selectable) FindByClass(text string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.Class, text).Single(), s.remoteFileUpload)
}

// FindByID finds exactly one element that has the given ID.
func (s *selectable) FindByID(id string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.ID, id).Single(), s.remoteFileUpload)
}

// First finds the first element by CSS selector.
func (s *selectable) First(selector string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.CSS, selector).At(0), s.remoteFileUpload)
}

// FirstByXPath finds the first element by XPath selector.
func (s *selectable) FirstByXPath(selector string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.XPath, selector).At(0), s.remoteFileUpload)
}

// FirstByLink finds the first anchor element by its text content.
func (s *selectable) FirstByLink(text string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.Link, text).At(0), s.remoteFileUpload)
}

// FirstByLabel finds the first element by associated label text.
func (s *selectable) FirstByLabel(text string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.Label, text).At(0), s.remoteFileUpload)
}

// FirstByButton finds the first button element with the provided text.
// Supports <button>, <input type="button">, and <input type="submit">.
func (s *selectable) FirstByButton(text string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.Button, text).At(0), s.remoteFileUpload)
}

// FirstByName finds the first element with the provided name attribute.
func (s *selectable) FirstByName(name string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.Name, name).At(0), s.remoteFileUpload)
}

// FirstByClass finds the first element with a given CSS class.
func (s *selectable) FirstByClass(text string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.Class, text).At(0), s.remoteFileUpload)
}

// All finds zero or more elements by CSS selector.
func (s *selectable) All(selector string) *MultiSelection {
	return newMultiSelection(s.session, s.selectors.Append(target.CSS, selector), s.remoteFileUpload)
}

// AllByXPath finds zero or more elements by XPath selector.
func (s *selectable) AllByXPath(selector string) *MultiSelection {
	return newMultiSelection(s.session, s.selectors.Append(target.XPath, selector), s.remoteFileUpload)
}

// AllByLink finds zero or more anchor elements by their text content.
func (s *selectable) AllByLink(text string) *MultiSelection {
	return newMultiSelection(s.session, s.selectors.Append(target.Link, text), s.remoteFileUpload)
}

// AllByLabel finds zero or more elements by associated label text.
func (s *selectable) AllByLabel(text string) *MultiSelection {
	return newMultiSelection(s.session, s.selectors.Append(target.Label, text), s.remoteFileUpload)
}

// AllByButton finds zero or more button elements with the provided text.
// Supports <button>, <input type="button">, and <input type="submit">.
func (s *selectable) AllByButton(text string) *MultiSelection {
	return newMultiSelection(s.session, s.selectors.Append(target.Button, text), s.remoteFileUpload)
}

// AllByName finds zero or more elements with the provided name attribute.
func (s *selectable) AllByName(name string) *MultiSelection {
	return newMultiSelection(s.session, s.selectors.Append(target.Name, name), s.remoteFileUpload)
}

// AllByClass finds zero or more elements with a given CSS class.
func (s *selectable) AllByClass(text string) *MultiSelection {
	return newMultiSelection(s.session, s.selectors.Append(target.Class, text), s.remoteFileUpload)
}

// AllByID finds zero or more elements with a given ID.
func (s *selectable) AllByID(text string) *MultiSelection {
	return newMultiSelection(s.session, s.selectors.Append(target.ID, text), s.remoteFileUpload)
}

// FirstByClass finds the first element with a given CSS class.
func (s *selectable) FindForAppium(selectorType string, text string) *Selection {
	return newSelection(s.session, s.selectors.Append(target.Class, text).At(0), s.remoteFileUpload)
}

func (s *selectable) Selectors() Selectors {
	return s.selectors
}
