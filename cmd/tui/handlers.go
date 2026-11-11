package main

import (
	"fmt"
	"strings"

	"github.com/gdamore/tcell/v2"
	"github.com/rivo/tview"
)

func (t *TUI) onInputDone(key tcell.Key) {
	switch key {
	case tcell.KeyEnter:
		t.connectToAPI()
	case tcell.KeyTab:
		t.focusInputPageField(1)
	case tcell.KeyBacktab:
		t.focusInputPageField(-1)
	}
}

func (t *TUI) onInputCapture(event *tcell.EventKey) *tcell.EventKey {
	if event.Key() == tcell.KeyCtrlC {
		t.Stop()
		return nil
	}

	currentPage, _ := t.pages.GetFrontPage()

	if currentPage == pageInput {
		switch event.Key() {
		case tcell.KeyTab:
			if t.focusInputPageField(1) {
				return nil
			}
		case tcell.KeyBacktab:
			if t.focusInputPageField(-1) {
				return nil
			}
		}
	}

	// Handle 'j' key for JSON view
	if event.Key() == tcell.KeyRune {
		switch r := event.Rune(); {
		case r == 'j' || r == 'J':
			switch currentPage {
			case pageCollections:
				index := t.collectionsList.GetCurrentItem()
				if index >= 0 && index < len(t.cols) {
					col := t.cols[index]
					t.showJSON(fmt.Sprintf("Collection %s", col.ID), col)
				}
				return nil
			case pageItems:
				index := t.itemsList.GetCurrentItem()
				if index >= 0 && index < len(t.items) {
					item := t.items[index]
					t.showJSON(fmt.Sprintf("Item %s", item.ID), item)
				}
				return nil
			case pageItemDetail:
				if t.currentItem != nil {
					t.showJSON(fmt.Sprintf("Item %s", t.currentItem.ID), t.currentItem)
				}
				return nil
			}
		case r == 's' || r == 'S':
			switch currentPage {
			case pageCollections, pageItems:
				t.openBasicSearchForm()
				return nil
			}
		}
	}

	// Item detail pane navigation
	if currentPage == pageItemDetail {
		if event.Key() == tcell.KeyTab {
			t.itemDetailFocus = (t.itemDetailFocus + 1) % len(t.itemDetailPanes)
			t.app.SetFocus(t.itemDetailPanes[t.itemDetailFocus])
			return nil
		} else if event.Key() == tcell.KeyBacktab {
			t.itemDetailFocus = (t.itemDetailFocus - 1 + len(t.itemDetailPanes)) % len(t.itemDetailPanes)
			t.app.SetFocus(t.itemDetailPanes[t.itemDetailFocus])
			return nil
		}
	}

	// Collections page focus toggle
	if currentPage == pageCollections {
		switch event.Key() {
		case tcell.KeyTab, tcell.KeyBacktab:
			if t.app.GetFocus() == t.collectionsList {
				t.app.SetFocus(t.colDetail)
			} else {
				t.app.SetFocus(t.collectionsList)
			}
			return nil
		}
	}

	if currentPage == pageSearch {
		switch event.Key() {
		case tcell.KeyTab:
			if t.searchCollectionsList != nil && t.searchCollectionsList.HasFocus() {
				t.focusSearchFormFirstField()
				return nil
			}
			if t.searchForm != nil && t.searchForm.HasFocus() {
				if t.searchCollectionsList != nil {
					t.app.SetFocus(t.searchCollectionsList)
				}
				return nil
			}
		case tcell.KeyBacktab:
			if t.searchForm != nil && t.searchForm.HasFocus() {
				if t.searchCollectionsList != nil {
					t.app.SetFocus(t.searchCollectionsList)
				}
				return nil
			}
			if t.searchCollectionsList != nil && t.searchCollectionsList.HasFocus() {
				t.focusSearchFormLastElement()
				return nil
			}
		}
	}

	// Escape key navigation
	if event.Key() == tcell.KeyEscape {
		// If JSON view is active, let its own handler deal with Escape.
		if currentPage == jsonPageID {
			return event
		}

		switch currentPage {
		case pageDownload:
			t.cancelActiveDownload()
			t.restoreFocusAfterModal()
			return nil
		case pageError, pageInfo:
			t.pages.HidePage(currentPage)
			t.restoreFocusAfterModal()
			return nil
		case pageItemDetail:
			t.pages.SwitchToPage(pageItems)
			t.app.SetFocus(t.itemsList)
			return nil
		case pageItems:
			t.exitSearchResults()
			return nil
		case pageCollections:
			t.pages.SwitchToPage(pageInput)
			t.app.SetFocus(t.input)
			return nil
		case pageSearch:
			t.closeSearchForm()
			return nil
		}
	}

	return event
}

func (t *TUI) connectToAPI() {
	if t.input == nil {
		return
	}

	url := strings.TrimSpace(t.input.GetText())
	if url == "" {
		t.showError("STAC API URL is required")
		return
	}

	cfg := t.currentAuthConfig()
	if err := cfg.validate(); err != nil {
		t.showError(err.Error())
		return
	}

	go t.fetchCollections(url, cfg)
}

func (t *TUI) currentAuthConfig() authConfig {
	cfg := authConfig{mode: t.authMode}

	switch cfg.mode {
	case authModeBearer:
		if t.authTokenField != nil {
			cfg.token = strings.TrimSpace(t.authTokenField.GetText())
		}
	case authModeBasic:
		if t.authUsernameField != nil {
			cfg.username = strings.TrimSpace(t.authUsernameField.GetText())
		}
		if t.authPasswordField != nil {
			cfg.password = t.authPasswordField.GetText()
		}
	case authModeHeader:
		if t.authHeaderNameField != nil {
			cfg.headerName = strings.TrimSpace(t.authHeaderNameField.GetText())
		}
		if t.authHeaderValueField != nil {
			cfg.headerValue = t.authHeaderValueField.GetText()
		}
	}

	return cfg
}

func (t *TUI) inputPageFocusOrder() []tview.Primitive {
	var fields []tview.Primitive
	if t.input != nil {
		fields = append(fields, t.input)
	}
	if t.authTypeDropDown != nil {
		fields = append(fields, t.authTypeDropDown)
	}

	switch t.authMode {
	case authModeBearer:
		if t.authTokenField != nil {
			fields = append(fields, t.authTokenField)
		}
	case authModeBasic:
		if t.authUsernameField != nil {
			fields = append(fields, t.authUsernameField)
		}
		if t.authPasswordField != nil {
			fields = append(fields, t.authPasswordField)
		}
	case authModeHeader:
		if t.authHeaderNameField != nil {
			fields = append(fields, t.authHeaderNameField)
		}
		if t.authHeaderValueField != nil {
			fields = append(fields, t.authHeaderValueField)
		}
	}

	return fields
}

func (t *TUI) focusInputPageField(offset int) bool {
	if t.app == nil {
		return false
	}

	fields := t.inputPageFocusOrder()
	if len(fields) == 0 {
		return false
	}

	current := t.app.GetFocus()
	index := -1
	for i, fld := range fields {
		if fld == current {
			index = i
			break
		}
	}

	if index == -1 {
		if offset >= 0 {
			t.app.SetFocus(fields[0])
		} else {
			t.app.SetFocus(fields[len(fields)-1])
		}
		return true
	}

	next := (index + offset + len(fields)) % len(fields)
	t.app.SetFocus(fields[next])
	return true
}

func (t *TUI) restoreFocusAfterModal() {
	if t.app == nil || t.pages == nil {
		return
	}

	currentPage, primitive := t.pages.GetFrontPage()
	if primitive == nil {
		return
	}

	switch currentPage {
	case pageItems:
		if t.itemsList != nil {
			t.app.SetFocus(t.itemsList)
		}
	case pageItemDetail:
		if len(t.itemDetailPanes) > 0 {
			if t.itemDetailFocus < 0 || t.itemDetailFocus >= len(t.itemDetailPanes) {
				t.itemDetailFocus = 0
			}
			t.app.SetFocus(t.itemDetailPanes[t.itemDetailFocus])
		}
	case pageCollections:
		if t.collectionsList != nil {
			t.app.SetFocus(t.collectionsList)
		}
	case pageSearch:
		if t.searchCollectionsList != nil {
			t.app.SetFocus(t.searchCollectionsList)
		}
	case pageInput:
		if t.input != nil {
			t.app.SetFocus(t.input)
		}
	}
}
