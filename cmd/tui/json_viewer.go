package main

import (
	"encoding/json"
	"fmt"
	"os"
	"sync"

	"github.com/gdamore/tcell/v2"
	"github.com/rivo/tview"
	"github.com/robert-malhotra/go-stac-client/cmd/tui/formatting"
)

const jsonPageID = "jsonView"

// jsonViewer owns the transient page used to display raw JSON.
type jsonViewer struct {
	tui       *TUI
	mu        sync.Mutex
	prevPage  string
	prevFocus tview.Primitive

	snapshotTitle string
	snapshotData  []byte
}

func newJSONViewer(t *TUI) *jsonViewer {
	return &jsonViewer{tui: t}
}

func (v *jsonViewer) Show(title string, value any) {
	if value == nil {
		return
	}

	// Snapshot current navigation state immediately to avoid races.
	focus := v.tui.app.GetFocus()
	currentPage, _ := v.tui.pages.GetFrontPage()

	v.mu.Lock()
	v.prevFocus = focus
	v.prevPage = currentPage
	v.snapshotTitle = title
	v.mu.Unlock()

	go func(val any, pageTitle string) {
		encoded, err := json.MarshalIndent(val, "", "  ")
		if err != nil {
			v.tui.showError(fmt.Sprintf("Failed to render JSON: %v", err))
			return
		}

		text := string(encoded)
		dataCopy := append([]byte(nil), encoded...)

		v.mu.Lock()
		v.snapshotData = dataCopy
		v.mu.Unlock()

		v.tui.app.QueueUpdateDraw(func() {
			textView := tview.NewTextView().
				SetDynamicColors(true).
				SetScrollable(true).
				SetWordWrap(false)
			textView.SetChangedFunc(func() { v.tui.app.Draw() })
			textView.SetBorder(true).SetTitle(pageTitle)
			textView.SetText(text)
			textView.SetInputCapture(v.handleInput)

			instructions := formatting.MakeHelpText("[yellow]Esc[white] close  |  [yellow]s[white] save JSON  |  [yellow]Ctrl+C[white] quit")
			layout := tview.NewFlex().
				SetDirection(tview.FlexRow).
				AddItem(textView, 0, 1, true).
				AddItem(instructions, 3, 0, false)

			v.tui.pages.RemovePage(jsonPageID)
			v.tui.pages.AddPage(jsonPageID, layout, true, false)
			v.tui.pages.ShowPage(jsonPageID)
			v.tui.pages.SwitchToPage(jsonPageID)
			v.tui.app.SetFocus(textView)
		})
	}(value, title)
}

func (v *jsonViewer) Close() {
	v.mu.Lock()
	prevFocus := v.prevFocus
	prevPage := v.prevPage
	v.prevFocus = nil
	v.prevPage = ""
	v.snapshotTitle = ""
	v.snapshotData = nil
	v.mu.Unlock()

	updateUI := func() {
		if prevPage != "" {
			v.tui.pages.SwitchToPage(prevPage)
		}
		v.tui.pages.HidePage(jsonPageID)

		if prevFocus != nil {
			v.tui.app.SetFocus(prevFocus)
		}
	}

	go v.tui.app.QueueUpdateDraw(updateUI)
}

func (v *jsonViewer) Save() {
	v.mu.Lock()
	data := append([]byte(nil), v.snapshotData...)
	title := v.snapshotTitle
	v.mu.Unlock()

	if len(data) == 0 {
		return
	}

	filename := formatting.GenerateJSONFilename(title)
	if err := os.WriteFile(filename, data, 0o644); err != nil {
		v.tui.showError(fmt.Sprintf("Failed to save JSON: %v", err))
		return
	}

	v.tui.showInfo(fmt.Sprintf("JSON saved to %s", filename))
}

func (v *jsonViewer) handleInput(event *tcell.EventKey) *tcell.EventKey {
	switch event.Key() {
	case tcell.KeyCtrlC:
		v.tui.Stop()
		return nil
	case tcell.KeyEscape:
		v.Close()
		return nil
	case tcell.KeyRune:
		r := event.Rune()
		if r == 's' || r == 'S' {
			go v.Save()
			return nil
		}
	}

	return event
}

// showJSON exposes the viewer through the TUI type for handlers.
func (t *TUI) showJSON(title string, value interface{}) {
	if t.jsonViewer == nil {
		t.showError("JSON viewer not initialized")
		return
	}
	t.jsonViewer.Show(title, value)
}

func (t *TUI) closeJSONView() {
	if t.jsonViewer != nil {
		t.jsonViewer.Close()
	}
}

func (t *TUI) saveJSONToFile() {
	if t.jsonViewer != nil {
		t.jsonViewer.Save()
	}
}
