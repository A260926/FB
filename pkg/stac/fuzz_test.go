package stac

import (
	"encoding/json"
	"testing"
)

// FuzzItemUnmarshal exercises Item.UnmarshalJSON against arbitrary inputs to
// catch panics on malformed STAC data.
func FuzzItemUnmarshal(f *testing.F) {
	f.Add(`{"type":"Feature","stac_version":"1.0.0","id":"x","geometry":null,"properties":{},"links":[],"assets":{}}`)
	f.Add(`{"type":"Feature","id":"x","geometry":{"type":"Point","coordinates":[0,0]}}`)
	f.Add(`{"type":"Feature","extra":[1,2,3]}`)
	f.Fuzz(func(t *testing.T, in string) {
		var it Item
		_ = json.Unmarshal([]byte(in), &it)
	})
}

// FuzzCollectionUnmarshal exercises Collection.UnmarshalJSON.
func FuzzCollectionUnmarshal(f *testing.F) {
	f.Add(`{"type":"Collection","stac_version":"1.0.0","id":"x","description":"y","license":"MIT","extent":{},"links":[]}`)
	f.Fuzz(func(t *testing.T, in string) {
		var c Collection
		_ = json.Unmarshal([]byte(in), &c)
	})
}

// FuzzCatalogUnmarshal exercises Catalog.UnmarshalJSON.
func FuzzCatalogUnmarshal(f *testing.F) {
	f.Add(`{"type":"Catalog","stac_version":"1.0.0","id":"x","description":"y","links":[]}`)
	f.Fuzz(func(t *testing.T, in string) {
		var c Catalog
		_ = json.Unmarshal([]byte(in), &c)
	})
}

// FuzzLinkUnmarshal exercises Link.UnmarshalJSON.
func FuzzLinkUnmarshal(f *testing.F) {
	f.Add(`{"href":"https://example.com","rel":"self"}`)
	f.Add(`{"href":"https://example.com","rel":"next","method":"POST","body":{"x":1}}`)
	f.Fuzz(func(t *testing.T, in string) {
		var l Link
		_ = json.Unmarshal([]byte(in), &l)
	})
}
