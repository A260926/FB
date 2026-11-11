package stac

// ItemsList represents a GeoJSON FeatureCollection of STAC Items.
type ItemsList struct {
	Type           string  `json:"type"`
	Features       []*Item `json:"features"`
	Links          []*Link `json:"links,omitempty"`
	NumberMatched  *int    `json:"numberMatched,omitempty"`  // Total number of matching items
	NumberReturned *int    `json:"numberReturned,omitempty"` // Number of items in this response
	Context        any     `json:"context,omitempty"`        // STAC context extension
}

// CollectionsList represents a list of STAC Collections.
type CollectionsList struct {
	Collections    []*Collection `json:"collections"`
	Links          []*Link       `json:"links,omitempty"`
	NumberMatched  *int          `json:"numberMatched,omitempty"`  // Total number of matching collections
	NumberReturned *int          `json:"numberReturned,omitempty"` // Number of collections in this response
}
