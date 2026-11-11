package stac

import (
	"encoding/json"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestJSONSchemaType_AcceptsStringAndArray(t *testing.T) {
	t.Run("string form", func(t *testing.T) {
		var qf QueryableField
		err := json.Unmarshal([]byte(`{"type":"integer"}`), &qf)
		require.NoError(t, err)
		assert.Equal(t, []string{"integer"}, []string(qf.Type))
		assert.Equal(t, "integer", qf.Type.First())
	})
	t.Run("array form (PC nullable)", func(t *testing.T) {
		var qf QueryableField
		err := json.Unmarshal([]byte(`{"type":["integer","null"]}`), &qf)
		require.NoError(t, err)
		assert.Equal(t, []string{"integer", "null"}, []string(qf.Type))
		assert.Equal(t, "integer", qf.Type.First())
		assert.Equal(t, "integer|null", qf.Type.String())
	})
	t.Run("marshal canonicalizes", func(t *testing.T) {
		single := JSONSchemaType{"integer"}
		b, err := json.Marshal(single)
		require.NoError(t, err)
		assert.Equal(t, `"integer"`, string(b))

		multi := JSONSchemaType{"integer", "null"}
		b, err = json.Marshal(multi)
		require.NoError(t, err)
		assert.Equal(t, `["integer","null"]`, string(b))
	})
}

func TestItemForeignMembers(t *testing.T) {
	t.Run("unmarshal preserves foreign members", func(t *testing.T) {
		jsonData := `{
			"type": "Feature",
			"stac_version": "1.0.0",
			"id": "test-item",
			"geometry": {"type": "Point", "coordinates": [0, 0]},
			"properties": {"datetime": "2023-01-01T00:00:00Z"},
			"links": [],
			"assets": {},
			"custom_field": "custom_value",
			"another_field": 42
		}`

		var item Item
		err := json.Unmarshal([]byte(jsonData), &item)
		require.NoError(t, err)

		assert.Equal(t, "test-item", item.ID)
		assert.Equal(t, "1.0.0", item.Version)
		assert.Contains(t, item.AdditionalFields, "custom_field")
		assert.Equal(t, "custom_value", item.AdditionalFields["custom_field"])
		assert.Contains(t, item.AdditionalFields, "another_field")
		assert.Equal(t, float64(42), item.AdditionalFields["another_field"])
	})

	t.Run("marshal includes foreign members and type", func(t *testing.T) {
		item := Item{
			Version:    "1.0.0",
			ID:         "test-item",
			Geometry:   json.RawMessage(`{"type":"Point","coordinates":[0,0]}`),
			Properties: map[string]any{"datetime": "2023-01-01T00:00:00Z"},
			Links:      []*Link{},
			Assets:     map[string]*Asset{},
			AdditionalFields: map[string]any{
				"custom_field":  "custom_value",
				"another_field": 42,
			},
		}

		data, err := json.Marshal(item)
		require.NoError(t, err)

		var decoded map[string]any
		require.NoError(t, json.Unmarshal(data, &decoded))

		assert.Equal(t, "Feature", decoded["type"]) // Type is always included
		assert.Equal(t, "custom_value", decoded["custom_field"])
		assert.Equal(t, float64(42), decoded["another_field"])
	})

	t.Run("round-trip preserves all fields", func(t *testing.T) {
		original := `{
			"type": "Feature",
			"stac_version": "1.0.0",
			"id": "test-item",
			"geometry": null,
			"properties": {},
			"links": [],
			"assets": {},
			"foreign_member": {"nested": "value"}
		}`

		var item Item
		require.NoError(t, json.Unmarshal([]byte(original), &item))

		output, err := json.Marshal(item)
		require.NoError(t, err)

		var decoded map[string]any
		require.NoError(t, json.Unmarshal(output, &decoded))

		assert.Equal(t, "Feature", decoded["type"]) // Type preserved
		assert.Contains(t, decoded, "foreign_member")
		fm := decoded["foreign_member"].(map[string]any)
		assert.Equal(t, "value", fm["nested"])
	})

	t.Run("rejects invalid type", func(t *testing.T) {
		jsonData := `{
			"type": "InvalidType",
			"stac_version": "1.0.0",
			"id": "test-item",
			"geometry": null,
			"properties": {},
			"links": [],
			"assets": {}
		}`

		var item Item
		err := json.Unmarshal([]byte(jsonData), &item)
		assert.Error(t, err)
		assert.Contains(t, err.Error(), "invalid item type")
	})
}

func TestCollectionForeignMembers(t *testing.T) {
	t.Run("unmarshal preserves foreign members", func(t *testing.T) {
		jsonData := `{
			"type": "Collection",
			"stac_version": "1.0.0",
			"id": "test-collection",
			"description": "Test collection",
			"license": "MIT",
			"extent": {"spatial": {"bbox": [[-180, -90, 180, 90]]}, "temporal": {"interval": [["2020-01-01T00:00:00Z", null]]}},
			"links": [],
			"custom_extension": {"enabled": true}
		}`

		var col Collection
		err := json.Unmarshal([]byte(jsonData), &col)
		require.NoError(t, err)

		assert.Equal(t, "test-collection", col.ID)
		assert.Contains(t, col.AdditionalFields, "custom_extension")
		ce := col.AdditionalFields["custom_extension"].(map[string]any)
		assert.Equal(t, true, ce["enabled"])
	})

	t.Run("marshal includes foreign members and type", func(t *testing.T) {
		col := Collection{
			Version:     "1.0.0",
			ID:          "test-collection",
			Description: "Test",
			License:     "MIT",
			Extent:      &Extent{},
			Links:       []*Link{},
			AdditionalFields: map[string]any{
				"custom_extension": map[string]any{"enabled": true},
			},
		}

		data, err := json.Marshal(col)
		require.NoError(t, err)

		var decoded map[string]any
		require.NoError(t, json.Unmarshal(data, &decoded))

		assert.Equal(t, "Collection", decoded["type"]) // Type is always included
		assert.Contains(t, decoded, "custom_extension")
	})

	t.Run("rejects invalid type", func(t *testing.T) {
		jsonData := `{
			"type": "InvalidType",
			"stac_version": "1.0.0",
			"id": "test-collection",
			"description": "Test",
			"license": "MIT",
			"extent": {},
			"links": []
		}`

		var col Collection
		err := json.Unmarshal([]byte(jsonData), &col)
		assert.Error(t, err)
		assert.Contains(t, err.Error(), "invalid collection type")
	})
}

func TestLinkForeignMembers(t *testing.T) {
	t.Run("unmarshal preserves foreign members", func(t *testing.T) {
		jsonData := `{
			"href": "https://example.com",
			"rel": "self",
			"method": "POST",
			"body": {"token": "abc123"}
		}`

		var link Link
		err := json.Unmarshal([]byte(jsonData), &link)
		require.NoError(t, err)

		assert.Equal(t, "https://example.com", link.Href)
		assert.Equal(t, "self", link.Rel)
		assert.Contains(t, link.AdditionalFields, "method")
		assert.Equal(t, "POST", link.AdditionalFields["method"])
		assert.Contains(t, link.AdditionalFields, "body")
	})

	t.Run("marshal includes foreign members", func(t *testing.T) {
		link := Link{
			Href: "https://example.com",
			Rel:  "next",
			AdditionalFields: map[string]any{
				"method": "POST",
			},
		}

		data, err := json.Marshal(link)
		require.NoError(t, err)

		var decoded map[string]any
		require.NoError(t, json.Unmarshal(data, &decoded))

		assert.Equal(t, "POST", decoded["method"])
	})
}

func TestAssetForeignMembers(t *testing.T) {
	t.Run("unmarshal preserves foreign members", func(t *testing.T) {
		jsonData := `{
			"href": "https://example.com/image.tif",
			"type": "image/tiff",
			"eo:bands": [{"name": "B01"}],
			"proj:epsg": 32632
		}`

		var asset Asset
		err := json.Unmarshal([]byte(jsonData), &asset)
		require.NoError(t, err)

		assert.Equal(t, "https://example.com/image.tif", asset.Href)
		assert.Contains(t, asset.AdditionalFields, "eo:bands")
		assert.Contains(t, asset.AdditionalFields, "proj:epsg")
		assert.Equal(t, float64(32632), asset.AdditionalFields["proj:epsg"])
	})

	t.Run("marshal includes foreign members", func(t *testing.T) {
		asset := Asset{
			Href: "https://example.com/image.tif",
			Type: "image/tiff",
			AdditionalFields: map[string]any{
				"proj:epsg": 32632,
			},
		}

		data, err := json.Marshal(asset)
		require.NoError(t, err)

		var decoded map[string]any
		require.NoError(t, json.Unmarshal(data, &decoded))

		assert.Equal(t, float64(32632), decoded["proj:epsg"])
	})
}

func TestItemWithNestedTypes(t *testing.T) {
	t.Run("unmarshal with links and assets containing foreign members", func(t *testing.T) {
		jsonData := `{
			"type": "Feature",
			"stac_version": "1.0.0",
			"id": "test-item",
			"geometry": null,
			"properties": {},
			"links": [
				{"href": "https://example.com", "rel": "self", "custom": "link_value"}
			],
			"assets": {
				"data": {"href": "https://example.com/data.tif", "custom": "asset_value"}
			}
		}`

		var item Item
		err := json.Unmarshal([]byte(jsonData), &item)
		require.NoError(t, err)

		require.Len(t, item.Links, 1)
		assert.Contains(t, item.Links[0].AdditionalFields, "custom")
		assert.Equal(t, "link_value", item.Links[0].AdditionalFields["custom"])

		require.Contains(t, item.Assets, "data")
		assert.Contains(t, item.Assets["data"].AdditionalFields, "custom")
		assert.Equal(t, "asset_value", item.Assets["data"].AdditionalFields["custom"])
	})
}
