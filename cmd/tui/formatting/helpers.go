package formatting

import (
	"fmt"
	"strconv"
	"strings"
	"time"
	"unicode"

	"github.com/rivo/tview"
)

func writeIndentedLines(builder *strings.Builder, text string, indent string) {
	text = strings.TrimRight(text, "\n")
	if text == "" {
		return
	}
	lines := strings.Split(text, "\n")
	for _, line := range lines {
		builder.WriteString(indent)
		builder.WriteString(line)
		builder.WriteByte('\n')
	}
}

func formatFloatSlice(values []float64) string {
	if len(values) == 0 {
		return "[]"
	}
	parts := make([]string, len(values))
	for i, v := range values {
		s := strconv.FormatFloat(v, 'f', 6, 64)
		s = strings.TrimRight(s, "0")
		s = strings.TrimSuffix(s, ".")
		if s == "" {
			s = "0"
		}
		parts[i] = s
	}
	return "[" + strings.Join(parts, ", ") + "]"
}

func RenderDownloadProgress(downloaded, total int64) string {
	if total > 0 {
		if downloaded > total {
			downloaded = total
		}
		const barWidth = 30
		ratio := float64(downloaded) / float64(total)
		if ratio > 1 {
			ratio = 1
		}
		filled := int(ratio * barWidth)
		if filled > barWidth {
			filled = barWidth
		}
		bar := strings.Repeat("#", filled) + strings.Repeat("-", barWidth-filled)
		percent := ratio * 100
		return fmt.Sprintf("[yellow][%s][white] %s / %s (%.1f%%)", bar, FormatBytes(downloaded), FormatBytes(total), percent)
	}
	return fmt.Sprintf("[yellow]%s downloaded[white]", FormatBytes(downloaded))
}

func FormatBytes(value int64) string {
	if value < 0 {
		value = 0
	}
	const unit = 1024
	if value < unit {
		return fmt.Sprintf("%d B", value)
	}
	div := float64(unit)
	exp := 0
	for n := value / unit; n >= unit && exp < 5; n /= unit {
		div *= unit
		exp++
	}
	result := float64(value) / div
	units := []string{"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"}
	if exp >= len(units) {
		exp = len(units) - 1
	}
	return fmt.Sprintf("%.1f %s", result, units[exp])
}

func MakeHelpText(text string) *tview.TextView {
	view := tview.NewTextView().
		SetDynamicColors(true).
		SetWrap(false).
		SetTextAlign(tview.AlignCenter).
		SetText(text)
	view.SetBorder(true).SetTitle("Controls")
	return view
}

func Slugify(input string) string {
	var builder strings.Builder
	for _, r := range input {
		switch {
		case unicode.IsLetter(r), unicode.IsDigit(r):
			builder.WriteRune(unicode.ToLower(r))
		case r == '-', r == '_':
			builder.WriteRune(r)
		case unicode.IsSpace(r):
			builder.WriteRune('-')
		}
	}
	return strings.Trim(builder.String(), "-_")
}

func GetOutputFilename(assetUrl string) string {
	parts := strings.Split(assetUrl, "/")
	return parts[len(parts)-1]
}

func GenerateJSONFilename(title string) string {
	slug := Slugify(title)
	if slug == "" {
		slug = "stac_object"
	}
	return fmt.Sprintf("%s_%s.json", slug, time.Now().Format("20060102_150405"))
}
