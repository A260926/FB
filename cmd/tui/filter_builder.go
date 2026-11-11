package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"time"

	cql2 "github.com/exergy-dev/go-cql2"
	cql2json "github.com/exergy-dev/go-cql2/json"
	"github.com/gdamore/tcell/v2"
	"github.com/rivo/tview"
	"github.com/robert-malhotra/go-stac-client/cmd/tui/formatting"
	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

const pageFilterBuilder = "filterBuilder"

// CQL2 comparison operator labels exposed by the UI. The mapping to
// go-cql2's typed builder lives in buildConditionExpr.
var cql2ComparisonOps = []string{"=", "<>", "<", "<=", ">", ">=", "like", "is null"}

// CQL2 logical operator labels (see logicalCombine for the go-cql2 mapping).
var cql2LogicalOps = []string{"and", "or"}

// filterCondition represents a single filter condition
type filterCondition struct {
	property string
	operator string
	value    string
}

// filterBuilder manages the CQL2 filter building UI
type filterBuilder struct {
	tui *TUI

	// UI components
	collectionDropdown *tview.DropDown
	propertyList       *tview.List
	propertyDetail     *tview.TextView
	conditionsList     *tview.List
	operatorDropdown   *tview.DropDown
	valueInput         *tview.InputField
	logicalOpDropdown  *tview.DropDown
	previewText        *tview.TextView

	// State
	queryables       *stac.Queryables
	queryableKeys    []string
	selectedProperty string
	conditions       []filterCondition
	logicalOp        string

	// Callback to return filter
	onComplete func(filterJSON string)
}

// newFilterBuilder creates a new filter builder instance
func newFilterBuilder(t *TUI) *filterBuilder {
	return &filterBuilder{
		tui:       t,
		logicalOp: "and",
	}
}

// setupFilterBuilderPage creates the filter builder page
func (t *TUI) setupFilterBuilderPage() {
	if t.filterBuilder == nil {
		t.filterBuilder = newFilterBuilder(t)
	}
	t.filterBuilder.setup()
}

// setup creates all the UI components for the filter builder
func (fb *filterBuilder) setup() {
	// Collection dropdown
	fb.collectionDropdown = tview.NewDropDown().
		SetLabel("Collection: ").
		SetFieldWidth(40)
	fb.collectionDropdown.SetSelectedFunc(func(text string, index int) {
		fb.onCollectionSelected(index)
	})

	// Property list
	fb.propertyList = tview.NewList()
	fb.propertyList.SetBorder(true).SetTitle("Queryable Properties")
	fb.propertyList.ShowSecondaryText(true)
	fb.propertyList.SetSecondaryTextColor(tcell.ColorGray)
	fb.propertyList.SetChangedFunc(func(index int, mainText, secondaryText string, shortcut rune) {
		fb.onPropertyChanged(index)
	})

	// Property detail
	fb.propertyDetail = tview.NewTextView().
		SetDynamicColors(true).
		SetWordWrap(true)
	fb.propertyDetail.SetBorder(true).SetTitle("Property Details")

	// Operator dropdown
	fb.operatorDropdown = tview.NewDropDown().
		SetLabel("Operator: ").
		SetFieldWidth(15).
		SetOptions(cql2ComparisonOps, nil)
	fb.operatorDropdown.SetCurrentOption(0)

	// Value input
	fb.valueInput = tview.NewInputField().
		SetLabel("Value: ").
		SetFieldWidth(30)

	// Preview text - create first so callbacks can use it
	fb.previewText = tview.NewTextView().
		SetDynamicColors(true).
		SetWordWrap(true)
	fb.previewText.SetBorder(true).SetTitle("CQL2 Filter Preview")
	fb.previewText.SetText("[gray]No conditions added yet[white]")

	// Logical operator dropdown
	fb.logicalOpDropdown = tview.NewDropDown().
		SetLabel("Combine with: ").
		SetFieldWidth(10).
		SetOptions(cql2LogicalOps, func(text string, index int) {
			fb.logicalOp = text
			fb.updatePreview()
		})
	fb.logicalOpDropdown.SetCurrentOption(0)

	// Conditions list
	fb.conditionsList = tview.NewList()
	fb.conditionsList.SetBorder(true).SetTitle("Filter Conditions (0)")
	fb.conditionsList.ShowSecondaryText(false)
	fb.conditionsList.SetSelectedFunc(func(index int, mainText, secondaryText string, shortcut rune) {
		fb.removeCondition(index)
	})

	// Build the layout
	fb.buildLayout()
}

// buildLayout creates the page layout
func (fb *filterBuilder) buildLayout() {
	// Left panel: collection + properties
	leftPanel := tview.NewFlex().SetDirection(tview.FlexRow).
		AddItem(fb.collectionDropdown, 1, 0, false).
		AddItem(fb.propertyList, 0, 2, true).
		AddItem(fb.propertyDetail, 8, 0, false)

	// Right top: condition builder
	conditionForm := tview.NewFlex().SetDirection(tview.FlexRow).
		AddItem(fb.operatorDropdown, 1, 0, false).
		AddItem(fb.valueInput, 1, 0, false).
		AddItem(fb.logicalOpDropdown, 1, 0, false)
	conditionForm.SetBorder(true).SetTitle("Add Condition")

	// Right panel: conditions + preview
	rightPanel := tview.NewFlex().SetDirection(tview.FlexRow).
		AddItem(conditionForm, 5, 0, false).
		AddItem(fb.conditionsList, 0, 1, false).
		AddItem(fb.previewText, 6, 0, false)

	// Main content
	mainContent := tview.NewFlex().
		AddItem(leftPanel, 0, 1, true).
		AddItem(rightPanel, 0, 1, false)

	// Buttons
	buttonFlex := tview.NewFlex().
		AddItem(nil, 0, 1, false)

	addBtn := tview.NewButton("Add Condition").SetSelectedFunc(func() {
		fb.addCondition()
	})
	clearBtn := tview.NewButton("Clear All").SetSelectedFunc(func() {
		fb.clearConditions()
	})
	applyBtn := tview.NewButton("Apply Filter").SetSelectedFunc(func() {
		fb.applyFilter()
	})
	cancelBtn := tview.NewButton("Cancel").SetSelectedFunc(func() {
		fb.cancel()
	})

	buttonFlex.
		AddItem(addBtn, 16, 0, false).
		AddItem(nil, 2, 0, false).
		AddItem(clearBtn, 12, 0, false).
		AddItem(nil, 2, 0, false).
		AddItem(applyBtn, 14, 0, false).
		AddItem(nil, 2, 0, false).
		AddItem(cancelBtn, 10, 0, false).
		AddItem(nil, 0, 1, false)

	// Help text
	help := formatting.MakeHelpText("[yellow]Tab[white] switch focus  [yellow]Enter[white] select/add  [yellow]a[white] add condition  [yellow]c[white] clear  [yellow]Esc[white] cancel")

	// Full page
	page := tview.NewFlex().SetDirection(tview.FlexRow).
		AddItem(mainContent, 0, 1, true).
		AddItem(buttonFlex, 1, 0, false).
		AddItem(help, 3, 0, false)

	page.SetInputCapture(fb.handleInput)

	fb.tui.pages.AddPage(pageFilterBuilder, page, true, false)
}

// handleInput handles key events for the filter builder
func (fb *filterBuilder) handleInput(event *tcell.EventKey) *tcell.EventKey {
	// Don't intercept keys when a dropdown is focused - let it handle its own input
	if fb.collectionDropdown.HasFocus() || fb.operatorDropdown.HasFocus() || fb.logicalOpDropdown.HasFocus() {
		// Only handle Escape to close
		if event.Key() == tcell.KeyEscape {
			fb.cancel()
			return nil
		}
		return event
	}

	// Don't intercept when typing in value input
	if fb.valueInput.HasFocus() {
		if event.Key() == tcell.KeyEscape {
			fb.cancel()
			return nil
		}
		if event.Key() == tcell.KeyTab {
			fb.cycleFocus(1)
			return nil
		}
		if event.Key() == tcell.KeyBacktab {
			fb.cycleFocus(-1)
			return nil
		}
		return event
	}

	switch event.Key() {
	case tcell.KeyEscape:
		fb.cancel()
		return nil
	case tcell.KeyTab:
		fb.cycleFocus(1)
		return nil
	case tcell.KeyBacktab:
		fb.cycleFocus(-1)
		return nil
	case tcell.KeyEnter:
		// If on property list, add condition
		if fb.propertyList.HasFocus() {
			fb.addCondition()
			return nil
		}
	case tcell.KeyRune:
		switch event.Rune() {
		case 'a', 'A':
			fb.addCondition()
			return nil
		case 'c', 'C':
			fb.clearConditions()
			return nil
		}
	}
	return event
}

// cycleFocus cycles through focusable elements
func (fb *filterBuilder) cycleFocus(direction int) {
	focusables := []tview.Primitive{
		fb.collectionDropdown,
		fb.propertyList,
		fb.operatorDropdown,
		fb.valueInput,
		fb.logicalOpDropdown,
		fb.conditionsList,
	}

	current := -1
	for i, p := range focusables {
		if p.HasFocus() {
			current = i
			break
		}
	}

	if current == -1 {
		fb.tui.app.SetFocus(focusables[0])
		return
	}

	next := (current + direction + len(focusables)) % len(focusables)
	fb.tui.app.SetFocus(focusables[next])
}

// show displays the filter builder
func (fb *filterBuilder) show(onComplete func(filterJSON string)) {
	fb.onComplete = onComplete
	fb.conditions = nil
	fb.queryables = nil
	fb.selectedProperty = ""

	// Populate collections dropdown
	fb.populateCollections()

	fb.updateConditionsList()
	fb.updatePreview()

	fb.tui.pages.ShowPage(pageFilterBuilder)
	fb.tui.app.SetFocus(fb.collectionDropdown)
}

// populateCollections fills the collection dropdown
func (fb *filterBuilder) populateCollections() {
	if len(fb.tui.cols) == 0 {
		fb.collectionDropdown.SetOptions([]string{"(no collections)"}, nil)
		return
	}

	options := make([]string, len(fb.tui.cols))
	for i, col := range fb.tui.cols {
		label := col.Title
		if label == "" {
			label = col.ID
		}
		options[i] = label
	}
	fb.collectionDropdown.SetOptions(options, nil)

	// If there's a selected collection in the search, use it
	if len(fb.tui.searchSelectedOrder) > 0 {
		for i, col := range fb.tui.cols {
			if col.ID == fb.tui.searchSelectedOrder[0] {
				fb.collectionDropdown.SetCurrentOption(i)
				fb.onCollectionSelected(i)
				return
			}
		}
	}

	// Default to first
	if len(fb.tui.cols) > 0 {
		fb.collectionDropdown.SetCurrentOption(0)
		fb.onCollectionSelected(0)
	}
}

// onCollectionSelected handles collection selection
func (fb *filterBuilder) onCollectionSelected(index int) {
	if index < 0 || index >= len(fb.tui.cols) {
		return
	}

	col := fb.tui.cols[index]

	// Fetch queryables for this collection
	go fb.fetchQueryables(col.ID)
}

// fetchQueryables fetches and displays queryables for a collection
func (fb *filterBuilder) fetchQueryables(collectionID string) {
	fb.tui.app.QueueUpdateDraw(func() {
		fb.propertyList.Clear()
		fb.propertyList.AddItem("Loading queryables...", "", 0, nil)
		fb.propertyDetail.SetText("[gray]Fetching queryable properties...[white]")
	})

	ctx, cancel := context.WithTimeout(fb.tui.baseCtx, 30*time.Second)
	defer cancel()

	queryables, err := fb.tui.client.GetQueryables(ctx, collectionID)

	fb.tui.app.QueueUpdateDraw(func() {
		fb.propertyList.Clear()

		if err != nil {
			fb.propertyList.AddItem("Queryables not available", "", 0, nil)
			fb.propertyDetail.SetText(fmt.Sprintf("[red]Error:[white] %v\n\n[gray]This collection may not support the filter extension.[white]", err))
			fb.queryables = nil
			fb.queryableKeys = nil
			return
		}

		fb.queryables = queryables
		fb.queryableKeys = nil

		if queryables.Properties == nil || len(queryables.Properties) == 0 {
			fb.propertyList.AddItem("No queryable properties", "", 0, nil)
			fb.propertyDetail.SetText("[gray]This collection has no queryable properties defined.[white]")
			return
		}

		// Sort keys for consistent display
		for key := range queryables.Properties {
			fb.queryableKeys = append(fb.queryableKeys, key)
		}
		sort.Strings(fb.queryableKeys)

		for _, key := range fb.queryableKeys {
			prop := queryables.Properties[key]
			title := prop.DisplayName(key)
			typeDesc := prop.TypeDescription()
			fb.propertyList.AddItem(title, typeDesc, 0, nil)
		}

		if len(fb.queryableKeys) > 0 {
			fb.propertyList.SetCurrentItem(0)
			fb.onPropertyChanged(0)
		}
	})
}

// onPropertyChanged handles property selection change
func (fb *filterBuilder) onPropertyChanged(index int) {
	if fb.queryables == nil || index < 0 || index >= len(fb.queryableKeys) {
		return
	}

	key := fb.queryableKeys[index]
	fb.selectedProperty = key
	prop := fb.queryables.Properties[key]

	var detail strings.Builder
	detail.WriteString(fmt.Sprintf("[yellow]Property:[white] %s\n", key))

	if prop.Title != "" {
		detail.WriteString(fmt.Sprintf("[yellow]Title:[white] %s\n", prop.Title))
	}

	if prop.Description != "" {
		detail.WriteString(fmt.Sprintf("[yellow]Description:[white] %s\n", prop.Description))
	}

	detail.WriteString(fmt.Sprintf("[yellow]Type:[white] %s\n", prop.TypeDescription()))

	if prop.Enum != nil && len(prop.Enum) > 0 {
		detail.WriteString("[yellow]Allowed values:[white]\n")
		for _, v := range prop.Enum {
			detail.WriteString(fmt.Sprintf("  • %v\n", v))
		}
	}

	if prop.Minimum != nil {
		detail.WriteString(fmt.Sprintf("[yellow]Minimum:[white] %v\n", *prop.Minimum))
	}
	if prop.Maximum != nil {
		detail.WriteString(fmt.Sprintf("[yellow]Maximum:[white] %v\n", *prop.Maximum))
	}

	if prop.Pattern != "" {
		detail.WriteString(fmt.Sprintf("[yellow]Pattern:[white] %s\n", prop.Pattern))
	}

	fb.propertyDetail.SetText(detail.String())

	// Update placeholder based on type
	fb.updateValuePlaceholder(prop)
}

// updateValuePlaceholder sets appropriate placeholder for the value input
func (fb *filterBuilder) updateValuePlaceholder(prop *stac.QueryableField) {
	placeholder := "Enter value"

	switch prop.Type.First() {
	case "number", "integer":
		placeholder = "Enter number"
		if prop.Minimum != nil && prop.Maximum != nil {
			placeholder = fmt.Sprintf("%.0f to %.0f", *prop.Minimum, *prop.Maximum)
		}
	case "boolean":
		placeholder = "true or false"
	case "string":
		if prop.Format == "date-time" {
			placeholder = "YYYY-MM-DD or YYYY-MM-DDTHH:MM:SSZ"
		} else if prop.Enum != nil && len(prop.Enum) > 0 {
			placeholder = fmt.Sprintf("e.g., %v", prop.Enum[0])
		}
	case "array":
		placeholder = "value1, value2, ..."
	}

	fb.valueInput.SetPlaceholder(placeholder)
}

// addCondition adds a new filter condition
func (fb *filterBuilder) addCondition() {
	if fb.selectedProperty == "" {
		fb.tui.showError("Please select a property first")
		return
	}

	opIndex, operator := fb.operatorDropdown.GetCurrentOption()
	if opIndex < 0 || operator == "" {
		operator = cql2ComparisonOps[0]
	}

	value := strings.TrimSpace(fb.valueInput.GetText())

	// "is null" doesn't need a value
	if operator != "is null" && value == "" {
		fb.tui.showError("Please enter a value")
		return
	}

	condition := filterCondition{
		property: fb.selectedProperty,
		operator: operator,
		value:    value,
	}

	fb.conditions = append(fb.conditions, condition)
	fb.valueInput.SetText("")

	fb.updateConditionsList()
	fb.updatePreview()
}

// removeCondition removes a condition by index
func (fb *filterBuilder) removeCondition(index int) {
	if index < 0 || index >= len(fb.conditions) {
		return
	}

	fb.conditions = append(fb.conditions[:index], fb.conditions[index+1:]...)
	fb.updateConditionsList()
	fb.updatePreview()
}

// clearConditions removes all conditions
func (fb *filterBuilder) clearConditions() {
	fb.conditions = nil
	fb.updateConditionsList()
	fb.updatePreview()
}

// updateConditionsList updates the conditions list display
func (fb *filterBuilder) updateConditionsList() {
	fb.conditionsList.Clear()
	fb.conditionsList.SetTitle(fmt.Sprintf("Filter Conditions (%d)", len(fb.conditions)))

	if len(fb.conditions) == 0 {
		fb.conditionsList.AddItem("[gray](no conditions - select property and add)[white]", "", 0, nil)
		return
	}

	for i, cond := range fb.conditions {
		var display string
		if cond.operator == "is null" {
			display = fmt.Sprintf("%d. %s IS NULL", i+1, cond.property)
		} else {
			display = fmt.Sprintf("%d. %s %s %s", i+1, cond.property, cond.operator, cond.value)
		}
		fb.conditionsList.AddItem(display, "", 0, nil)
	}

	fb.conditionsList.AddItem("[dim](click to remove)[white]", "", 0, nil)
}

// updatePreview updates the CQL2 filter preview
func (fb *filterBuilder) updatePreview() {
	if len(fb.conditions) == 0 {
		fb.previewText.SetText("[gray]No conditions added yet[white]")
		return
	}

	filterJSON := fb.buildCQL2Filter()
	if filterJSON == "" {
		fb.previewText.SetText("[red]Error building filter[white]")
		return
	}

	// Pretty print
	var prettyBuf bytes.Buffer
	if err := json.Indent(&prettyBuf, []byte(filterJSON), "", "  "); err != nil {
		fb.previewText.SetText("[green]" + filterJSON + "[white]")
		return
	}

	fb.previewText.SetText("[green]" + prettyBuf.String() + "[white]")
}

// buildCQL2Filter constructs the CQL2-JSON filter as a string by composing
// go-cql2 builder expressions and encoding through the official
// github.com/exergy-dev/go-cql2/json encoder.
func (fb *filterBuilder) buildCQL2Filter() string {
	if len(fb.conditions) == 0 {
		return ""
	}
	exprs := make([]cql2.Expr, 0, len(fb.conditions))
	for _, cond := range fb.conditions {
		expr, ok := fb.buildConditionExpr(cond)
		if !ok {
			continue
		}
		exprs = append(exprs, expr)
	}
	if len(exprs) == 0 {
		return ""
	}
	combined, err := logicalCombine(fb.logicalOp, exprs)
	if err != nil {
		return ""
	}
	data, err := cql2json.Encode(combined.Node())
	if err != nil {
		return ""
	}
	return string(data)
}

// buildConditionExpr maps one UI filterCondition to a go-cql2 builder
// expression. Returns false if the condition cannot be expressed (e.g.,
// unknown operator).
func (fb *filterBuilder) buildConditionExpr(cond filterCondition) (cql2.Expr, bool) {
	if cond.operator == "is null" {
		return cql2.IsNull(cond.property), true
	}

	var value any = cond.value
	if fb.queryables != nil && fb.queryables.Properties != nil {
		if prop, ok := fb.queryables.Properties[cond.property]; ok {
			value = fb.parseValue(cond.value, prop)
		}
	}

	switch cond.operator {
	case "=":
		return cql2.Eq(cond.property, value), true
	case "<>":
		return cql2.Neq(cond.property, value), true
	case "<":
		return cql2.Lt(cond.property, value), true
	case "<=":
		return cql2.Lte(cond.property, value), true
	case ">":
		return cql2.Gt(cond.property, value), true
	case ">=":
		return cql2.Gte(cond.property, value), true
	case "like":
		pattern, ok := value.(string)
		if !ok {
			pattern = cond.value
		}
		return cql2.Like(cond.property, pattern), true
	}
	return cql2.Expr{}, false
}

// logicalCombine wraps a list of expressions in the chosen top-level
// boolean operator. With a single expression the operator is irrelevant
// and the expression is returned as-is.
func logicalCombine(op string, exprs []cql2.Expr) (cql2.Expr, error) {
	if len(exprs) == 1 {
		return exprs[0], nil
	}
	switch strings.ToLower(op) {
	case "or":
		return cql2.Or(exprs...), nil
	default:
		return cql2.And(exprs...), nil
	}
}

// parseValue converts string value to appropriate type
func (fb *filterBuilder) parseValue(value string, prop *stac.QueryableField) any {
	switch prop.Type.First() {
	case "integer":
		if i, err := strconv.ParseInt(value, 10, 64); err == nil {
			return i
		}
	case "number":
		if f, err := strconv.ParseFloat(value, 64); err == nil {
			return f
		}
	case "boolean":
		lower := strings.ToLower(value)
		if lower == "true" || lower == "1" || lower == "yes" {
			return true
		}
		if lower == "false" || lower == "0" || lower == "no" {
			return false
		}
	case "array":
		// Split by comma
		parts := strings.Split(value, ",")
		var arr []any
		for _, p := range parts {
			arr = append(arr, strings.TrimSpace(p))
		}
		return arr
	}
	return value
}

// applyFilter applies the built filter
func (fb *filterBuilder) applyFilter() {
	filterJSON := fb.buildCQL2Filter()

	fb.tui.pages.HidePage(pageFilterBuilder)

	if fb.onComplete != nil {
		fb.onComplete(filterJSON)
	}
}

// cancel closes the filter builder without applying
func (fb *filterBuilder) cancel() {
	fb.tui.pages.HidePage(pageFilterBuilder)

	if fb.onComplete != nil {
		fb.onComplete("")
	}
}
