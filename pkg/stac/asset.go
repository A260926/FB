package stac

import "encoding/json"

// Asset represents a STAC Asset with support for additional fields.
type Asset struct {
	Type        string   `json:"type,omitempty"`
	Href        string   `json:"href"`
	Title       string   `json:"title,omitempty"`
	Description string   `json:"description,omitempty"`
	Roles       []string `json:"roles,omitempty"`

	// AdditionalFields holds foreign members from extensions
	// (e.g. "eo:bands", "proj:epsg", "created").
	AdditionalFields map[string]any `json:"-"`
}

var knownAssetFields = map[string]bool{
	"type": true, "href": true, "title": true, "description": true,
	"roles": true,
}

// UnmarshalJSON implements custom unmarshaling to capture foreign members.
func (asset *Asset) UnmarshalJSON(data []byte) error {
	type assetAlias Asset
	var aux assetAlias
	if err := json.Unmarshal(data, &aux); err != nil {
		return err
	}
	*asset = Asset(aux)

	var raw map[string]json.RawMessage
	if err := json.Unmarshal(data, &raw); err != nil {
		return err
	}

	asset.AdditionalFields = make(map[string]any)
	for key, val := range raw {
		if !knownAssetFields[key] {
			var decoded any
			if err := json.Unmarshal(val, &decoded); err != nil {
				continue
			}
			asset.AdditionalFields[key] = decoded
		}
	}

	return nil
}

// MarshalJSON implements custom marshaling to include foreign members.
func (asset Asset) MarshalJSON() ([]byte, error) {
	type assetAlias Asset
	aux := assetAlias(asset)

	data, err := json.Marshal(aux)
	if err != nil {
		return nil, err
	}

	if len(asset.AdditionalFields) == 0 {
		return data, nil
	}

	var obj map[string]json.RawMessage
	if err := json.Unmarshal(data, &obj); err != nil {
		return nil, err
	}

	for key, val := range asset.AdditionalFields {
		if knownAssetFields[key] {
			continue
		}
		encoded, err := json.Marshal(val)
		if err != nil {
			return nil, err
		}
		obj[key] = encoded
	}

	return json.Marshal(obj)
}
