package stac

import (
	"bytes"
	"encoding/json"
	"fmt"
	"strings"
)

// Queryables represents the queryable properties for a STAC API collection.
// This follows the OGC API - Features - Part 3: Filtering specification.
type Queryables struct {
	Schema      string                     `json:"$schema,omitempty"`
	ID          string                     `json:"$id,omitempty"`
	Type        string                     `json:"type,omitempty"`
	Title       string                     `json:"title,omitempty"`
	Description string                     `json:"description,omitempty"`
	Properties  map[string]*QueryableField `json:"properties,omitempty"`

	// AdditionalFields holds foreign members not defined in the spec.
	AdditionalFields map[string]any `json:"-"`
}

// JSONSchemaType represents JSON Schema's "type" keyword, which is either a
// string ("integer") or an array of strings (["integer", "null"]). Both forms
// are valid and both appear in the wild (e.g., Microsoft Planetary Computer
// uses arrays for nullable types).
type JSONSchemaType []string

// UnmarshalJSON accepts either a JSON string or an array of strings.
func (t *JSONSchemaType) UnmarshalJSON(data []byte) error {
	data = bytes.TrimSpace(data)
	if len(data) == 0 || bytes.Equal(data, []byte("null")) {
		*t = nil
		return nil
	}
	if data[0] == '"' {
		var s string
		if err := json.Unmarshal(data, &s); err != nil {
			return err
		}
		*t = []string{s}
		return nil
	}
	if data[0] == '[' {
		var s []string
		if err := json.Unmarshal(data, &s); err != nil {
			return err
		}
		*t = s
		return nil
	}
	return fmt.Errorf("queryables: type must be string or []string, got %s", string(data))
}

// MarshalJSON emits the canonical JSON Schema form: a bare string for a
// single type, an array otherwise.
func (t JSONSchemaType) MarshalJSON() ([]byte, error) {
	switch len(t) {
	case 0:
		return []byte("null"), nil
	case 1:
		return json.Marshal(t[0])
	default:
		return json.Marshal([]string(t))
	}
}

// String returns the type for the common single-value case, or a
// pipe-joined string ("integer|null") for multi-valued types.
func (t JSONSchemaType) String() string {
	switch len(t) {
	case 0:
		return ""
	case 1:
		return t[0]
	default:
		return strings.Join(t, "|")
	}
}

// First returns the first type in the list, or "" if empty.
func (t JSONSchemaType) First() string {
	if len(t) == 0 {
		return ""
	}
	return t[0]
}

// QueryableField represents a single queryable property with its JSON Schema
// definition.
//
// Type accepts JSON Schema's two valid forms — bare string ("integer") or an
// array of strings (["integer", "null"]).
type QueryableField struct {
	Title       string         `json:"title,omitempty"`
	Description string         `json:"description,omitempty"`
	Type        JSONSchemaType `json:"type,omitempty"`
	Format      string         `json:"format,omitempty"`
	Enum        []any          `json:"enum,omitempty"`
	Minimum     *float64       `json:"minimum,omitempty"`
	Maximum     *float64       `json:"maximum,omitempty"`
	MinItems    *int           `json:"minItems,omitempty"`
	MaxItems    *int           `json:"maxItems,omitempty"`
	Pattern     string         `json:"pattern,omitempty"`
	Items       *Items         `json:"items,omitempty"`
	Ref         string         `json:"$ref,omitempty"`
	OneOf       []any          `json:"oneOf,omitempty"`
	AnyOf       []any          `json:"anyOf,omitempty"`

	// AdditionalFields holds foreign members.
	AdditionalFields map[string]any `json:"-"`
}

// Items represents the items schema for array types.
type Items struct {
	Type JSONSchemaType `json:"type,omitempty"`
}

var knownQueryablesFields = map[string]bool{
	"$schema": true, "$id": true, "type": true, "title": true,
	"description": true, "properties": true,
}

var knownQueryableFieldFields = map[string]bool{
	"title": true, "description": true, "type": true, "format": true,
	"enum": true, "minimum": true, "maximum": true, "minItems": true,
	"maxItems": true, "pattern": true, "items": true, "$ref": true,
	"oneOf": true, "anyOf": true,
}

// UnmarshalJSON implements custom unmarshaling to capture foreign members.
func (q *Queryables) UnmarshalJSON(data []byte) error {
	type queryablesAlias Queryables
	var aux queryablesAlias
	if err := json.Unmarshal(data, &aux); err != nil {
		return err
	}
	*q = Queryables(aux)

	var raw map[string]json.RawMessage
	if err := json.Unmarshal(data, &raw); err != nil {
		return err
	}

	q.AdditionalFields = make(map[string]any)
	for key, val := range raw {
		if !knownQueryablesFields[key] {
			var decoded any
			if err := json.Unmarshal(val, &decoded); err != nil {
				continue
			}
			q.AdditionalFields[key] = decoded
		}
	}
	return nil
}

// MarshalJSON implements custom marshaling to include foreign members.
func (q Queryables) MarshalJSON() ([]byte, error) {
	type queryablesAlias Queryables
	aux := queryablesAlias(q)

	data, err := json.Marshal(aux)
	if err != nil {
		return nil, err
	}
	if len(q.AdditionalFields) == 0 {
		return data, nil
	}
	var obj map[string]json.RawMessage
	if err := json.Unmarshal(data, &obj); err != nil {
		return nil, err
	}
	for key, val := range q.AdditionalFields {
		if knownQueryablesFields[key] {
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

// UnmarshalJSON implements custom unmarshaling for QueryableField.
func (qf *QueryableField) UnmarshalJSON(data []byte) error {
	type fieldAlias QueryableField
	var aux fieldAlias
	if err := json.Unmarshal(data, &aux); err != nil {
		return err
	}
	*qf = QueryableField(aux)

	var raw map[string]json.RawMessage
	if err := json.Unmarshal(data, &raw); err != nil {
		return err
	}

	qf.AdditionalFields = make(map[string]any)
	for key, val := range raw {
		if !knownQueryableFieldFields[key] {
			var decoded any
			if err := json.Unmarshal(val, &decoded); err != nil {
				continue
			}
			qf.AdditionalFields[key] = decoded
		}
	}
	return nil
}

// MarshalJSON implements custom marshaling for QueryableField.
func (qf QueryableField) MarshalJSON() ([]byte, error) {
	type fieldAlias QueryableField
	aux := fieldAlias(qf)

	data, err := json.Marshal(aux)
	if err != nil {
		return nil, err
	}
	if len(qf.AdditionalFields) == 0 {
		return data, nil
	}
	var obj map[string]json.RawMessage
	if err := json.Unmarshal(data, &obj); err != nil {
		return nil, err
	}
	for key, val := range qf.AdditionalFields {
		if knownQueryableFieldFields[key] {
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

// DisplayName returns a user-friendly name for the field.
func (qf *QueryableField) DisplayName(key string) string {
	if qf.Title != "" {
		return qf.Title
	}
	return key
}

// TypeDescription returns a human-readable type description.
func (qf *QueryableField) TypeDescription() string {
	t := qf.Type.String()
	if t == "" {
		return "any"
	}
	if qf.Format != "" {
		t += " (" + qf.Format + ")"
	}
	return t
}
