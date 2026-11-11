package main

import (
	"sort"

	"github.com/rivo/tview"
	"github.com/robert-malhotra/go-stac-client/cmd/tui/formatting"
	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

type assetListEntry struct {
	key   string
	asset *stac.Asset
}

func (t *TUI) showItemDetail(item *stac.Item) {
	t.currentItem = item

	// Properties
	t.itemProperties.Clear()
	t.itemProperties.SetText(formatting.FormatProperties(item.Properties, 0))
	t.itemProperties.ScrollToBeginning()

	// Assets
	t.buildAssetsView(item)

	rightPane := tview.NewFlex().SetDirection(tview.FlexRow).
		AddItem(t.itemAssets, 0, 1, true).
		AddItem(t.itemAssetDetail, 0, 1, false)

	t.itemDetail.Clear().
		AddItem(t.itemProperties, 0, 0, 1, 1, 0, 0, true).
		AddItem(rightPane, 0, 1, 1, 1, 0, 0, false)

	t.itemDetailFocus = 0
	t.app.SetFocus(t.itemDetailPanes[t.itemDetailFocus])

	t.pages.SwitchToPage(pageItemDetail)
}

func (t *TUI) buildAssetsView(item *stac.Item) {
	t.itemAssets.Clear()
	t.itemAssetEntries = t.itemAssetEntries[:0]

	if t.itemAssetDetail != nil {
		if item == nil || len(item.Assets) == 0 {
			t.itemAssetDetail.SetText("This item does not list any assets.")
		} else {
			t.itemAssetDetail.SetText("Select an asset to view details.")
		}
	}

	if item == nil || len(item.Assets) == 0 {
		t.itemAssets.AddItem("No assets", "", 0, nil)
		t.itemAssets.SetCurrentItem(0)
		return
	}

	keys := make([]string, 0, len(item.Assets))
	for key := range item.Assets {
		keys = append(keys, key)
	}
	sort.Strings(keys)

	for _, key := range keys {
		asset := item.Assets[key]
		if asset == nil {
			continue
		}
		main, secondary := formatting.FormatAssetListItem(key, asset)
		entry := assetListEntry{key: key, asset: asset}
		t.itemAssetEntries = append(t.itemAssetEntries, entry)
		assetCopy := asset
		t.itemAssets.AddItem(main, secondary, 0, func() {
			t.downloadAsset(assetCopy)
		})
	}

	if len(t.itemAssetEntries) == 0 {
		t.itemAssets.AddItem("No assets", "", 0, nil)
		t.itemAssets.SetCurrentItem(0)
		if t.itemAssetDetail != nil {
			t.itemAssetDetail.SetText("This item does not list any assets.")
		}
		return
	}

	t.itemAssets.SetCurrentItem(0)
	t.updateItemAssetDetail(0)
}

func (t *TUI) updateItemAssetDetail(index int) {
	if t.itemAssetDetail == nil {
		return
	}

	if len(t.itemAssetEntries) == 0 {
		t.itemAssetDetail.SetText("This item does not list any assets.")
		return
	}

	if index < 0 || index >= len(t.itemAssetEntries) {
		t.itemAssetDetail.SetText("Select an asset to view details.")
		return
	}

	entry := t.itemAssetEntries[index]
	t.itemAssetDetail.SetText(formatting.FormatAssetDetailBlock(entry.key, entry.asset))
	t.itemAssetDetail.ScrollToBeginning()
}
