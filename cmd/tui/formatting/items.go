package formatting

import (
	"encoding/json"
	"fmt"
	"sort"
	"strings"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

func FormatItemSummary(item *stac.Item) string {
	var builder strings.Builder
	builder.WriteString(fmt.Sprintf("[yellow]ID: [white]%s\n", item.ID))
	if dt, ok := item.Properties["datetime"].(string); ok {
		builder.WriteString(fmt.Sprintf("[yellow]Datetime: [white]%s\n", dt))
	}
	if p, ok := item.Properties["platform"].(string); ok {
		builder.WriteString(fmt.Sprintf("[yellow]Platform: [white]%s\n", p))
	}
	if c, ok := item.Properties["constellation"].(string); ok {
		builder.WriteString(fmt.Sprintf("[yellow]Constellation: [white]%s\n", c))
	}
	if geomText := FormatGeometry(item.Geometry); geomText != "" {
		builder.WriteString("[yellow]Geometry:[white]\n")
		builder.WriteString(geomText)
		if !strings.HasSuffix(geomText, "\n") {
			builder.WriteString("\n")
		}
	}
	return builder.String()
}

func FormatProperties(properties map[string]interface{}, indent int) string {
	var builder strings.Builder
	keys := make([]string, 0, len(properties))
	for k := range properties {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	for _, key := range keys {
		val := properties[key]
		indentedKey := fmt.Sprintf("%s%s:", strings.Repeat("  ", indent), key)
		builder.WriteString(fmt.Sprintf("[yellow]%-30s[white]", indentedKey))

		jsonBytes, err := json.MarshalIndent(val, "", "  ")
		if err != nil {
			builder.WriteString(" Error marshalling value\n")
		} else {
			builder.WriteString(fmt.Sprintf(" %s\n", string(jsonBytes)))
		}
	}
	return builder.String()
}

func FormatAssetListItem(key string, asset *stac.Asset) (string, string) {
	primary := strings.TrimSpace(key)
	if asset != nil {
		if primary == "" {
			primary = strings.TrimSpace(asset.Title)
		}
		if primary == "" {
			primary = strings.TrimSpace(asset.Type)
		}
	}
	if primary == "" {
		primary = "(asset)"
	}

	secondary := ""
	if asset != nil {
		title := strings.TrimSpace(asset.Title)
		if title != "" && !strings.EqualFold(title, primary) {
			secondary = title
		} else {
			typ := strings.TrimSpace(asset.Type)
			if typ != "" && !strings.EqualFold(typ, primary) {
				secondary = typ
			}
		}
	}

	return primary, secondary
}

func FormatAssetDetailBlock(key string, asset *stac.Asset) string {
	if asset == nil {
		return "No asset details available."
	}

	var builder strings.Builder
	write := func(label, value string) {
		value = strings.TrimSpace(value)
		if value == "" {
			return
		}
		builder.WriteString(fmt.Sprintf("[yellow]%s: [white]%s\n", label, value))
	}

	write("Key", strings.TrimSpace(key))
	write("Title", asset.Title)
	write("Description", asset.Description)
	write("Type", asset.Type)
	if len(asset.Roles) > 0 {
		write("Roles", strings.Join(asset.Roles, ", "))
	}
	// Check for created in AdditionalFields (extension field)
	if created, ok := asset.AdditionalFields["created"].(string); ok {
		write("Created", created)
	}
	write("Href", asset.Href)

	text := strings.TrimRight(builder.String(), "\n")
	if text == "" {
		return "No asset details available."
	}
	return text
}
