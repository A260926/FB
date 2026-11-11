package formatting

import (
	"encoding/json"
	"fmt"
	"strconv"
	"strings"

	"github.com/exergy-dev/go-topology-suite/geojson"
	"github.com/exergy-dev/go-topology-suite/geom"
)

// FormatGeometry renders a STAC Item geometry (raw GeoJSON bytes, a
// json.RawMessage, a string, or any value that can be re-marshaled to
// GeoJSON) as a multi-line summary suitable for display in the TUI.
//
// Parsing is delegated to github.com/exergy-dev/go-topology-suite so the
// TUI does not implement its own GeoJSON walker.
func FormatGeometry(geometry interface{}) string {
	if geometry == nil {
		return ""
	}

	data, err := toGeoJSONBytes(geometry)
	if err != nil || len(data) == 0 || isJSONNull(data) {
		return ""
	}

	g, err := geojson.Unmarshal(data)
	if err != nil {
		// Fall back to raw JSON so the user at least sees something.
		return string(data)
	}

	var sections []string
	sections = append(sections, g.Type().String())

	switch v := g.(type) {
	case *geom.Point:
		if !v.IsEmpty() {
			sections = append(sections, formatXY(v.XY()))
		}
	case *geom.LineString:
		sections = append(sections, formatXYs(v.XYs()))
	case *geom.Polygon:
		var rings []string
		for i := 0; i < v.NumRings(); i++ {
			label := "outer"
			if i > 0 {
				label = fmt.Sprintf("hole %d", i)
			}
			rings = append(rings, label+": "+formatXYs(v.Ring(i)))
		}
		sections = append(sections, strings.Join(rings, "\n"))
	case *geom.MultiPoint:
		var pts []string
		for _, p := range geom.PointsOf(v) {
			if !p.IsEmpty() {
				pts = append(pts, formatXY(p.XY()))
			}
		}
		sections = append(sections, strings.Join(pts, ", "))
	case *geom.MultiLineString:
		var lines []string
		for i, ls := range geom.LineStringsOf(v) {
			lines = append(lines, fmt.Sprintf("line %d: %s", i+1, formatXYs(ls.XYs())))
		}
		sections = append(sections, strings.Join(lines, "\n"))
	case *geom.MultiPolygon:
		var polys []string
		for i, p := range geom.PolygonsOf(v) {
			polys = append(polys, fmt.Sprintf("polygon %d (rings=%d)", i+1, p.NumRings()))
		}
		sections = append(sections, strings.Join(polys, "\n"))
	case *geom.GeometryCollection:
		sections = append(sections, fmt.Sprintf("%d geometries", v.NumGeometries()))
	}

	if env := g.Envelope(); !env.IsEmpty() {
		sections = append(sections,
			fmt.Sprintf("bbox [%s, %s, %s, %s]",
				fmtFloat(env.MinX), fmtFloat(env.MinY),
				fmtFloat(env.MaxX), fmtFloat(env.MaxY)))
	}

	return strings.Join(sections, "\n")
}

// toGeoJSONBytes coerces the various concrete types we receive (json.RawMessage,
// []byte, string, struct, map) into a GeoJSON byte slice.
func toGeoJSONBytes(v interface{}) ([]byte, error) {
	switch t := v.(type) {
	case nil:
		return nil, nil
	case json.RawMessage:
		return []byte(t), nil
	case []byte:
		return t, nil
	case string:
		return []byte(t), nil
	default:
		return json.Marshal(t)
	}
}

func formatXY(p geom.XY) string {
	return "[" + fmtFloat(p.X) + ", " + fmtFloat(p.Y) + "]"
}

func formatXYs(pts []geom.XY) string {
	parts := make([]string, len(pts))
	for i, p := range pts {
		parts[i] = formatXY(p)
	}
	return wrapCoordinateString("["+strings.Join(parts, ", ")+"]", 70)
}

func fmtFloat(f float64) string {
	return strconv.FormatFloat(f, 'f', 5, 64)
}

func wrapCoordinateString(s string, width int) string {
	if len(s) <= width || width <= 0 {
		return s
	}
	var out strings.Builder
	lineLen := 0
	for i := 0; i < len(s); i++ {
		ch := s[i]
		if lineLen >= width && (ch == ',' || ch == ']' || ch == ' ') {
			out.WriteByte('\n')
			lineLen = 0
			if ch == ' ' {
				continue
			}
		}
		out.WriteByte(ch)
		lineLen++
	}
	return out.String()
}

func isJSONNull(b []byte) bool {
	i := 0
	for i < len(b) && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r') {
		i++
	}
	return i+4 <= len(b) && string(b[i:i+4]) == "null"
}
