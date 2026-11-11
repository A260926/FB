package stac

// Extent represents the spatial and temporal extent of a STAC Collection.
type Extent struct {
	Spatial  *SpatialExtent  `json:"spatial,omitempty"`
	Temporal *TemporalExtent `json:"temporal,omitempty"`
}

// SpatialExtent represents the spatial extent of a STAC Collection.
//
// Bbox is one or more bounding boxes; the first is the overall extent and any
// additional entries are sub-extents. Each bbox has 4 elements (2D) or 6
// elements (3D) in the order specified by the STAC and OGC API specifications:
// [west, south, east, north] or [west, south, min_elev, east, north, max_elev].
type SpatialExtent struct {
	Bbox [][]float64 `json:"bbox"`
}

// TemporalExtent represents the temporal extent of a STAC Collection.
//
// Each interval is a two-element array [start, end] of RFC 3339 datetime
// strings; either endpoint may be nil to indicate an open-ended interval.
type TemporalExtent struct {
	Interval [][]*string `json:"interval"`
}
