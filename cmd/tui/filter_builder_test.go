package main

import (
	"encoding/json"
	"testing"

	cql2 "github.com/exergy-dev/go-cql2"
	cql2json "github.com/exergy-dev/go-cql2/json"
	"github.com/robert-malhotra/go-stac-client/pkg/stac"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestFilterBuilder_SingleCondition(t *testing.T) {
	fb := &filterBuilder{
		logicalOp: "and",
		conditions: []filterCondition{
			{property: "eo:cloud_cover", operator: "<", value: "10"},
		},
		queryables: &stac.Queryables{
			Properties: map[string]*stac.QueryableField{
				"eo:cloud_cover": {Type: stac.JSONSchemaType{"number"}},
			},
		},
	}
	got := fb.buildCQL2Filter()
	require.NotEmpty(t, got)

	want, err := cql2json.Encode(cql2.Lt("eo:cloud_cover", 10.0).Node())
	require.NoError(t, err)
	assertJSONEqual(t, string(want), got)
}

func TestFilterBuilder_AndCombination(t *testing.T) {
	fb := &filterBuilder{
		logicalOp: "and",
		conditions: []filterCondition{
			{property: "eo:cloud_cover", operator: "<", value: "10"},
			{property: "collection", operator: "=", value: "sentinel-2-l2a"},
		},
		queryables: &stac.Queryables{
			Properties: map[string]*stac.QueryableField{
				"eo:cloud_cover": {Type: stac.JSONSchemaType{"number"}},
				"collection":     {Type: stac.JSONSchemaType{"string"}},
			},
		},
	}
	got := fb.buildCQL2Filter()
	want, err := cql2json.Encode(cql2.And(
		cql2.Lt("eo:cloud_cover", 10.0),
		cql2.Eq("collection", "sentinel-2-l2a"),
	).Node())
	require.NoError(t, err)
	assertJSONEqual(t, string(want), got)
}

func TestFilterBuilder_IsNull(t *testing.T) {
	fb := &filterBuilder{
		logicalOp:  "and",
		conditions: []filterCondition{{property: "platform", operator: "is null"}},
	}
	got := fb.buildCQL2Filter()
	want, err := cql2json.Encode(cql2.IsNull("platform").Node())
	require.NoError(t, err)
	assertJSONEqual(t, string(want), got)
}

func TestFilterBuilder_AllOperators(t *testing.T) {
	cases := []struct {
		op   string
		want cql2.Expr
	}{
		{"=", cql2.Eq("x", "v")},
		{"<>", cql2.Neq("x", "v")},
		{"<", cql2.Lt("x", "v")},
		{"<=", cql2.Lte("x", "v")},
		{">", cql2.Gt("x", "v")},
		{">=", cql2.Gte("x", "v")},
		{"like", cql2.Like("x", "v")},
	}
	for _, c := range cases {
		t.Run(c.op, func(t *testing.T) {
			fb := &filterBuilder{
				logicalOp:  "and",
				conditions: []filterCondition{{property: "x", operator: c.op, value: "v"}},
			}
			got := fb.buildCQL2Filter()
			want, err := cql2json.Encode(c.want.Node())
			require.NoError(t, err)
			assertJSONEqual(t, string(want), got)
		})
	}
}

func TestFilterBuilder_OrLogical(t *testing.T) {
	fb := &filterBuilder{
		logicalOp: "or",
		conditions: []filterCondition{
			{property: "platform", operator: "=", value: "sentinel-2a"},
			{property: "platform", operator: "=", value: "sentinel-2b"},
		},
	}
	got := fb.buildCQL2Filter()
	want, err := cql2json.Encode(cql2.Or(
		cql2.Eq("platform", "sentinel-2a"),
		cql2.Eq("platform", "sentinel-2b"),
	).Node())
	require.NoError(t, err)
	assertJSONEqual(t, string(want), got)
}

func TestFilterBuilder_EmptyReturnsEmptyString(t *testing.T) {
	fb := &filterBuilder{logicalOp: "and"}
	assert.Empty(t, fb.buildCQL2Filter())
}

// assertJSONEqual asserts two JSON strings are semantically equivalent
// (key order independent).
func assertJSONEqual(t *testing.T, want, got string) {
	t.Helper()
	var w, g any
	require.NoError(t, json.Unmarshal([]byte(want), &w), "want is invalid JSON: %s", want)
	require.NoError(t, json.Unmarshal([]byte(got), &g), "got is invalid JSON: %s", got)
	assert.Equal(t, w, g)
}
