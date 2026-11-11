// Package stac provides Go types for the SpatioTemporal Asset Catalog (STAC)
// 1.0.0 specification.
//
// The Item, Collection, Catalog, Asset, Link, and Queryables types all
// preserve "foreign members" — JSON fields not defined in the core STAC
// specification — through round-trip marshaling, so STAC extension fields
// (e.g. eo:bands, proj:epsg) and custom server fields are not lost.
//
// Geometry on an Item is exposed as json.RawMessage so that 3D coordinates
// and arbitrary GeoJSON object shapes are preserved verbatim. Decode it into
// any GeoJSON-compatible type when needed:
//
//	var geom map[string]any
//	if err := json.Unmarshal(item.Geometry, &geom); err != nil { ... }
//
// The Conformance* constants list well-known STAC API and OGC API conformance
// class URIs.
package stac
