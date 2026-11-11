package stac

import (
	"encoding/json"
	"fmt"
)

// CatalogType is the STAC type for Catalogs (always "Catalog").
const CatalogType = "Catalog"

// Catalog represents a STAC Catalog with support for foreign members.
//
// A Catalog is the root entry point for a STAC API or static catalog,
// providing links to collections, items, and other catalogs.
// The Type field is implicit and always "Catalog" per the STAC specification.
type Catalog struct {
	Version     string   `json:"stac_version"`
	Extensions  []string `json:"stac_extensions,omitempty"`
	ID          string   `json:"id"`
	Title       string   `json:"title,omitempty"`
	Description string   `json:"description"`
	Links       []*Link  `json:"links"`
	ConformsTo  []string `json:"conformsTo,omitempty"`

	// AdditionalFields holds foreign members not defined in the STAC spec.
	AdditionalFields map[string]any `json:"-"`
}

var knownCatalogFields = map[string]bool{
	"type": true, "stac_version": true, "stac_extensions": true,
	"id": true, "title": true, "description": true, "links": true,
	"conformsTo": true,
}

// UnmarshalJSON implements custom unmarshaling to capture foreign members.
func (cat *Catalog) UnmarshalJSON(data []byte) error {
	type catalogAlias Catalog
	var aux catalogAlias
	if err := json.Unmarshal(data, &aux); err != nil {
		return err
	}
	*cat = Catalog(aux)

	var raw map[string]json.RawMessage
	if err := json.Unmarshal(data, &raw); err != nil {
		return err
	}

	if typeVal, ok := raw["type"]; ok {
		var t string
		if err := json.Unmarshal(typeVal, &t); err == nil && t != "" && t != CatalogType {
			return fmt.Errorf("invalid catalog type: expected %q, got %q", CatalogType, t)
		}
	}

	cat.AdditionalFields = make(map[string]any)
	for key, val := range raw {
		if !knownCatalogFields[key] {
			var decoded any
			if err := json.Unmarshal(val, &decoded); err != nil {
				continue
			}
			cat.AdditionalFields[key] = decoded
		}
	}

	return nil
}

// MarshalJSON implements custom marshaling to include foreign members.
//
// The canonical "type" field is always written last so that
// AdditionalFields cannot override it.
func (cat Catalog) MarshalJSON() ([]byte, error) {
	type catalogAlias Catalog
	aux := catalogAlias(cat)

	data, err := json.Marshal(aux)
	if err != nil {
		return nil, err
	}

	var obj map[string]json.RawMessage
	if err := json.Unmarshal(data, &obj); err != nil {
		return nil, err
	}

	for key, val := range cat.AdditionalFields {
		if knownCatalogFields[key] {
			continue
		}
		encoded, err := json.Marshal(val)
		if err != nil {
			return nil, err
		}
		obj[key] = encoded
	}

	typeJSON, _ := json.Marshal(CatalogType)
	obj["type"] = typeJSON

	return json.Marshal(obj)
}

// GetLink returns the first link with the specified rel type, or nil if not found.
func (cat *Catalog) GetLink(rel string) *Link {
	return firstLinkByRel(cat.Links, rel)
}

// GetLinks returns all links with the specified rel type.
func (cat *Catalog) GetLinks(rel string) []*Link {
	return linksByRel(cat.Links, rel)
}

// HasConformance reports whether the catalog declares the given conformance class.
func (cat *Catalog) HasConformance(conformanceClass string) bool {
	for _, c := range cat.ConformsTo {
		if c == conformanceClass {
			return true
		}
	}
	return false
}

// Common STAC API conformance class URIs.
const (
	ConformanceCore         = "https://api.stacspec.org/v1.0.0/core"
	ConformanceCollections  = "https://api.stacspec.org/v1.0.0/collections"
	ConformanceFeatures     = "https://api.stacspec.org/v1.0.0/ogcapi-features"
	ConformanceItemSearch   = "https://api.stacspec.org/v1.0.0/item-search"
	ConformanceFilter       = "https://api.stacspec.org/v1.0.0/item-search#filter"
	ConformanceSort         = "https://api.stacspec.org/v1.0.0/item-search#sort"
	ConformanceFields       = "https://api.stacspec.org/v1.0.0/item-search#fields"
	ConformanceQuery        = "https://api.stacspec.org/v1.0.0/item-search#query"
	ConformanceContext      = "https://api.stacspec.org/v1.0.0/item-search#context"
	ConformanceCQL2Text     = "http://www.opengis.net/spec/cql2/1.0/conf/cql2-text"
	ConformanceCQL2JSON     = "http://www.opengis.net/spec/cql2/1.0/conf/cql2-json"
	ConformanceBasicCQL2    = "http://www.opengis.net/spec/cql2/1.0/conf/basic-cql2"
	ConformanceAdvancedCQL2 = "http://www.opengis.net/spec/cql2/1.0/conf/advanced-comparison-operators"
	ConformanceSpatialCQL2  = "http://www.opengis.net/spec/cql2/1.0/conf/basic-spatial-operators"
	ConformanceTemporalCQL2 = "http://www.opengis.net/spec/cql2/1.0/conf/temporal-operators"

	// OGC API - Features Part 1 conformance URIs.
	ConformanceOGCFeaturesCore    = "http://www.opengis.net/spec/ogcapi-features-1/1.0/conf/core"
	ConformanceOGCFeaturesGeoJSON = "http://www.opengis.net/spec/ogcapi-features-1/1.0/conf/geojson"
	ConformanceOGCFeaturesOAS30   = "http://www.opengis.net/spec/ogcapi-features-1/1.0/conf/oas30"
)
