package formatting

import (
	"encoding/json"
	"fmt"
	"sort"
	"strings"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

func FormatCollectionDetails(col *stac.Collection) string {
	if col == nil {
		return ""
	}

	var builder strings.Builder
	writeField := func(label, value string) {
		value = strings.TrimSpace(value)
		if value == "" {
			return
		}
		if strings.Contains(value, "\n") {
			builder.WriteString(fmt.Sprintf("[yellow]%s:[white]\n", label))
			writeIndentedLines(&builder, value, "  ")
		} else {
			builder.WriteString(fmt.Sprintf("[yellow]%s: [white]%s\n", label, value))
		}
	}

	writeField("Title", col.Title)
	writeField("ID", col.ID)
	writeField("Version", col.Version)
	writeField("Description", col.Description)
	writeField("License", col.License)

	if len(col.Keywords) > 0 {
		writeField("Keywords", strings.Join(col.Keywords, ", "))
	}

	if len(col.Extensions) > 0 {
		uris := make([]string, len(col.Extensions))
		copy(uris, col.Extensions)
		sort.Strings(uris)
		writeField("Extensions", strings.Join(uris, ", "))
	}

	if len(col.Providers) > 0 {
		builder.WriteString("[yellow]Providers:[white]\n")
		for _, provider := range col.Providers {
			if provider == nil {
				continue
			}
			if provider.Name != "" {
				builder.WriteString(fmt.Sprintf("  - Name: %s\n", provider.Name))
			} else {
				builder.WriteString("  -\n")
			}
			if provider.Description != "" {
				builder.WriteString(fmt.Sprintf("    Description: %s\n", provider.Description))
			}
			if len(provider.Roles) > 0 {
				builder.WriteString(fmt.Sprintf("    Roles: %s\n", strings.Join(provider.Roles, ", ")))
			}
			if provider.URL != "" {
				builder.WriteString(fmt.Sprintf("    URL: %s\n", provider.URL))
			}
		}
	}

	if col.Extent != nil {
		builder.WriteString("[yellow]Extent:[white]\n")
		if col.Extent.Spatial != nil && len(col.Extent.Spatial.Bbox) > 0 {
			total := len(col.Extent.Spatial.Bbox)
			for i, bbox := range col.Extent.Spatial.Bbox {
				label := "  Spatial bbox"
				if total > 1 {
					label = fmt.Sprintf("%s %d", label, i+1)
				}
				builder.WriteString(fmt.Sprintf("%s: %s\n", label, formatFloatSlice(bbox)))
			}
		}
		if col.Extent.Temporal != nil && len(col.Extent.Temporal.Interval) > 0 {
			total := len(col.Extent.Temporal.Interval)
			for i, interval := range col.Extent.Temporal.Interval {
				label := "  Temporal interval"
				if total > 1 {
					label = fmt.Sprintf("%s %d", label, i+1)
				}
				builder.WriteString(fmt.Sprintf("%s: %s\n", label, formatTemporalInterval(interval)))
			}
		}
	}

	if len(col.Summaries) > 0 {
		builder.WriteString("[yellow]Summaries:[white]\n")
		keys := make([]string, 0, len(col.Summaries))
		for key := range col.Summaries {
			keys = append(keys, key)
		}
		sort.Strings(keys)
		for _, key := range keys {
			value := col.Summaries[key]
			jsonBytes, err := json.MarshalIndent(value, "", "  ")
			if err != nil {
				builder.WriteString(fmt.Sprintf("  %s: %v\n", key, value))
				continue
			}
			lines := strings.Split(strings.TrimRight(string(jsonBytes), "\n"), "\n")
			if len(lines) == 1 {
				builder.WriteString(fmt.Sprintf("  %s: %s\n", key, lines[0]))
			} else {
				builder.WriteString(fmt.Sprintf("  %s:\n", key))
				writeIndentedLines(&builder, strings.Join(lines, "\n"), "    ")
			}
		}
	}

	if len(col.Assets) > 0 {
		builder.WriteString("[yellow]Assets:[white]\n")
		assetKeys := make([]string, 0, len(col.Assets))
		for key := range col.Assets {
			assetKeys = append(assetKeys, key)
		}
		sort.Strings(assetKeys)
		for _, key := range assetKeys {
			asset := col.Assets[key]
			if asset == nil {
				continue
			}
			builder.WriteString(fmt.Sprintf("  - %s\n", key))
			if asset.Title != "" {
				builder.WriteString(fmt.Sprintf("    Title: %s\n", asset.Title))
			}
			if asset.Description != "" {
				builder.WriteString(fmt.Sprintf("    Description: %s\n", asset.Description))
			}
			if asset.Type != "" {
				builder.WriteString(fmt.Sprintf("    Type: %s\n", asset.Type))
			}
			if len(asset.Roles) > 0 {
				builder.WriteString(fmt.Sprintf("    Roles: %s\n", strings.Join(asset.Roles, ", ")))
			}
			if asset.Href != "" {
				builder.WriteString(fmt.Sprintf("    Href: %s\n", asset.Href))
			}
		}
	}

	if len(col.Links) > 0 {
		builder.WriteString("[yellow]Links:[white]\n")
		for _, link := range col.Links {
			if link == nil {
				continue
			}
			rel := link.Rel
			if rel == "" {
				rel = "(unknown)"
			}
			href := link.Href
			if href == "" {
				href = "(missing)"
			}
			builder.WriteString(fmt.Sprintf("  - %s -> %s\n", rel, href))
			if link.Type != "" {
				builder.WriteString(fmt.Sprintf("    Type: %s\n", link.Type))
			}
			if link.Title != "" {
				builder.WriteString(fmt.Sprintf("    Title: %s\n", link.Title))
			}
			if len(link.AdditionalFields) > 0 {
				jsonBytes, err := json.MarshalIndent(link.AdditionalFields, "", "  ")
				if err == nil {
					builder.WriteString("    Additional Fields:\n")
					writeIndentedLines(&builder, string(jsonBytes), "      ")
				}
			}
		}
	}

	return strings.TrimRight(builder.String(), "\n")
}

func formatTemporalInterval(interval []*string) string {
	if len(interval) == 0 {
		return "[]"
	}
	parts := make([]string, len(interval))
	for i, p := range interval {
		if p == nil {
			parts[i] = "null"
		} else {
			parts[i] = fmt.Sprintf("%q", *p)
		}
	}
	return "[" + strings.Join(parts, ", ") + "]"
}
