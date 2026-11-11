package formatting

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestFormatGeometry_Point(t *testing.T) {
	out := FormatGeometry(json.RawMessage(`{"type":"Point","coordinates":[-122.4,37.7]}`))
	assert.Contains(t, out, "POINT")
	assert.Contains(t, out, "-122.40000")
	assert.Contains(t, out, "37.70000")
	assert.Contains(t, out, "bbox")
}

func TestFormatGeometry_Polygon(t *testing.T) {
	in := `{"type":"Polygon","coordinates":[[[0,0],[1,0],[1,1],[0,1],[0,0]]]}`
	out := FormatGeometry(json.RawMessage(in))
	assert.Contains(t, out, "POLYGON")
	assert.Contains(t, out, "outer")
	assert.Contains(t, out, "bbox")
}

func TestFormatGeometry_GeometryCollection(t *testing.T) {
	in := `{"type":"GeometryCollection","geometries":[{"type":"Point","coordinates":[1,2]},{"type":"Point","coordinates":[3,4]}]}`
	out := FormatGeometry(json.RawMessage(in))
	assert.Contains(t, out, "GEOMETRYCOLLECTION")
	assert.Contains(t, out, "2 geometries")
}

func TestFormatGeometry_NilAndNull(t *testing.T) {
	assert.Equal(t, "", FormatGeometry(nil))
	assert.Equal(t, "", FormatGeometry(json.RawMessage("null")))
	assert.Equal(t, "", FormatGeometry(json.RawMessage("")))
}

func TestFormatGeometry_AcceptsString(t *testing.T) {
	out := FormatGeometry(`{"type":"Point","coordinates":[0,0]}`)
	assert.Contains(t, out, "POINT")
}

func TestFormatGeometry_FallsBackOnInvalid(t *testing.T) {
	in := `{"type":"NotAType"}`
	out := FormatGeometry(json.RawMessage(in))
	// Either we get the raw fallback or some sensible string; just ensure
	// no panic and that we got *something*.
	assert.True(t, strings.Contains(out, "NotAType") || out != "")
}
