package main

import (
	"context"
	"sync"

	"github.com/gdamore/tcell/v2"
	"github.com/rivo/tview"
	"github.com/robert-malhotra/go-stac-client/pkg/client"
	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

type TUI struct {
	app                   *tview.Application
	pages                 *tview.Pages
	input                 *tview.InputField
	authTypeDropDown      *tview.DropDown
	authTokenField        *tview.InputField
	authUsernameField     *tview.InputField
	authPasswordField     *tview.InputField
	authHeaderNameField   *tview.InputField
	authHeaderValueField  *tview.InputField
	authFieldsContainer   *tview.Flex
	searchForm            *tview.Form
	searchSummary         *tview.InputField
	searchDatetime        *tview.InputField
	searchBbox            *tview.InputField
	searchLimit           *tview.InputField
	searchCollectionsList *tview.List
	collectionsList       *tview.List
	colDetail             *tview.TextView
	itemsList             *tview.List
	itemSummary           *tview.TextView
	itemsHelp             *tview.TextView
	itemDetail            *tview.Grid

	// Item detail panes
	itemProperties  *tview.TextView
	itemAssets      *tview.List
	itemAssetDetail *tview.TextView
	itemDetailPanes []tview.Primitive
	itemDetailFocus int

	client  *client.Client
	baseURL string
	cols    []*stac.Collection
	items   []*stac.Item

	activeResultLabel         string
	lastSearchMetadata        map[string]string
	searchReturnPage          string
	searchSelectedOrder       []string
	searchSelectedCollections map[string]bool
	currentItem               *stac.Item
	itemAssetEntries          []assetListEntry

	// Iterator for items (used synchronously, on-demand)
	stacItemsIterator       func() (*stac.Item, error, bool)
	stacItemsIteratorStop   func()
	stacItemsIteratorCancel context.CancelFunc
	searchResultsReturnPage string

	// Paging state
	pageSize       int
	isLoadingItems bool
	isExhausted    bool

	itemLoadingMutex sync.Mutex

	baseCtx    context.Context
	baseCancel context.CancelFunc
	stopOnce   sync.Once

	authMode authMode

	downloadMu     sync.Mutex
	activeDownload *downloadSession

	jsonViewer *jsonViewer

	currentAuth authConfig

	filterBuilder *filterBuilder
}

// configureStyles sets the tview global styles for the TUI.
// Note: This modifies global state in tview.Styles.
func configureStyles() {
	tview.Styles.PrimitiveBackgroundColor = tcell.ColorBlack
	tview.Styles.ContrastBackgroundColor = tcell.ColorDarkSlateGray
	tview.Styles.MoreContrastBackgroundColor = tcell.ColorGreen
	tview.Styles.BorderColor = tcell.ColorWhite
	tview.Styles.TitleColor = tcell.ColorWhite
	tview.Styles.GraphicsColor = tcell.ColorWhite
	tview.Styles.PrimaryTextColor = tcell.ColorWhite
	tview.Styles.SecondaryTextColor = tcell.ColorYellow
	tview.Styles.TertiaryTextColor = tcell.ColorGreen
	tview.Styles.InverseTextColor = tcell.ColorBlue
	tview.Styles.ContrastSecondaryTextColor = tcell.ColorNavy
}

// NewTUI creates a new TUI instance. The provided context controls the
// lifetime of background operations; pass nil to use context.Background().
func NewTUI(ctx context.Context) *TUI {
	if ctx == nil {
		ctx = context.Background()
	}
	baseCtx, baseCancel := context.WithCancel(ctx)

	configureStyles()

	tui := &TUI{
		app:                       tview.NewApplication(),
		pages:                     tview.NewPages(),
		pageSize:                  10,
		baseCtx:                   baseCtx,
		baseCancel:                baseCancel,
		searchSelectedCollections: make(map[string]bool),
	}

	tui.setupPages()
	tui.jsonViewer = newJSONViewer(tui)

	tui.app.SetInputCapture(tui.onInputCapture)
	tui.app.SetFocus(tui.input)

	return tui
}

// Run starts the TUI event loop. It blocks until the application exits
// and returns any error that occurred.
func (t *TUI) Run() error {
	return t.app.SetRoot(t.pages, true).Run()
}

func (t *TUI) Stop() {
	t.stopOnce.Do(func() {
		if t.baseCancel != nil {
			t.baseCancel()
		}
		t.cancelActiveDownload()
		t.cancelItemIteration()
		t.app.Stop()
	})
}

func (t *TUI) cancelItemIteration() {
	if t.stacItemsIteratorCancel != nil {
		t.stacItemsIteratorCancel()
		t.stacItemsIteratorCancel = nil
	}
	if t.stacItemsIteratorStop != nil {
		t.stacItemsIteratorStop()
		t.stacItemsIteratorStop = nil
	}
	t.stacItemsIterator = nil

	t.itemLoadingMutex.Lock()
	t.isLoadingItems = false
	t.itemLoadingMutex.Unlock()
}
