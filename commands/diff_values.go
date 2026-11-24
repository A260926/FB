package commands

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
)

// DiffKind identifies the kind of a DiffValue node.
type DiffKind uint8

const (
	// DiffScalar is a plain literal value compared by Text.
	DiffScalar DiffKind = iota
	// DiffCreate marks a newly created handle (or an alias of a live physical
	// handle already observed in the same implementation).
	DiffCreate
	// DiffRef references an already live handle.
	DiffRef
	// DiffList is an ordered sequence of DiffValues.
	DiffList
	// DiffObject is a keyed collection of DiffValues.
	DiffObject
)

// DiffValue is a single node of a differential observation tree.
//
// The valid field combinations depend on Kind, see the differential
// contract for details.
type DiffValue struct {
	Kind   DiffKind
	Text   string
	Items  []DiffValue
	Fields map[string]DiffValue
}

// DiffOutput is the complete observation of one DifferentialCommand run:
// the value tree plus the physical handles released by this round.
type DiffOutput struct {
	Value   DiffValue
	Release []string
}

// HandleBindings is a read-only snapshot of one implementation's committed
// handle bindings. The zero value is an empty snapshot.
type HandleBindings struct {
	logicalToPhysical map[string]string
}

// Resolve returns the physical handle string currently bound to the exact
// live logical identity. It never allocates or mutates bindings. Empty,
// unknown or released identities resolve to ("", false).
func (h *HandleBindings) Resolve(identity string) (physical string, ok bool) {
	if h == nil || h.logicalToPhysical == nil {
		return "", false
	}
	physical, ok = h.logicalToPhysical[identity]
	return physical, ok
}

// handleTable is the bidirectional physical <-> logical mapping of a single
// implementation. Committed tables are never mutated in place: every round
// stages changes on a clone and only replaces the committed table on commit.
type handleTable struct {
	physicalToLogical map[string]string
	logicalToPhysical map[string]string
}

func newHandleTable() *handleTable {
	return &handleTable{
		physicalToLogical: map[string]string{},
		logicalToPhysical: map[string]string{},
	}
}

func (t *handleTable) clone() *handleTable {
	cp := newHandleTable()
	if t == nil {
		return cp
	}
	for physical, logical := range t.physicalToLogical {
		cp.physicalToLogical[physical] = logical
	}
	for logical, physical := range t.logicalToPhysical {
		cp.logicalToPhysical[logical] = physical
	}
	return cp
}

func (t *handleTable) bind(physical, logical string) {
	t.physicalToLogical[physical] = logical
	t.logicalToPhysical[logical] = physical
}

// snapshot returns an immutable-to-callers HandleBindings carrying a copy of
// the committed logical -> physical mapping.
func (t *handleTable) snapshot() *HandleBindings {
	bindings := &HandleBindings{logicalToPhysical: map[string]string{}}
	if t != nil {
		for logical, physical := range t.logicalToPhysical {
			bindings.logicalToPhysical[logical] = physical
		}
	}
	return bindings
}

// jsonPointerEscape escapes one path token according to RFC 6901:
// '~' becomes '~0' and '/' becomes '~1'. Tildes must be escaped first.
func jsonPointerEscape(token string) string {
	token = strings.ReplaceAll(token, "~", "~0")
	token = strings.ReplaceAll(token, "/", "~1")
	return token
}

// normalizeRound validates and normalizes one implementation's output for a
// command at zero-based commandIndex. It returns the staged table (including
// new bindings and with released bindings removed), the normalized value
// tree (handle Text replaced by logical identities) and the sorted logical
// release identities.
func normalizeRound(commandIndex int, committed *handleTable, output DiffOutput) (*handleTable, DiffValue, []string, error) {
	staged := committed.clone()

	normalizedValue, err := normalizeValue(commandIndex, "", output.Value, staged)
	if err != nil {
		return nil, DiffValue{}, nil, err
	}

	releases, err := stageReleases(staged, output.Release)
	if err != nil {
		return nil, DiffValue{}, nil, err
	}

	return staged, normalizedValue, releases, nil
}

func normalizeValue(commandIndex int, path string, value DiffValue, table *handleTable) (DiffValue, error) {
	if err := validateShape(value); err != nil {
		return DiffValue{}, fmt.Errorf("canonical path %q: %w", path, err)
	}

	normalized := DiffValue{Kind: value.Kind}

	switch value.Kind {
	case DiffScalar:
		normalized.Text = value.Text

	case DiffCreate:
		physical := value.Text
		if logical, live := table.physicalToLogical[physical]; live {
			// A second Create of an still-live physical handle is an alias of
			// its first identity.
			normalized.Kind = DiffRef
			normalized.Text = logical
			break
		}
		logical := strconv.Itoa(commandIndex) + ":" + path
		table.bind(physical, logical)
		normalized.Kind = DiffRef
		normalized.Text = logical

	case DiffRef:
		logical, live := table.physicalToLogical[value.Text]
		if !live {
			return DiffValue{}, fmt.Errorf("canonical path %q: unknown reference to physical handle %q", path, value.Text)
		}
		normalized.Kind = DiffRef
		normalized.Text = logical

	case DiffList:
		normalized.Items = make([]DiffValue, len(value.Items))
		for i, item := range value.Items {
			itemPath := path + "/" + strconv.Itoa(i)
			normalizedItem, err := normalizeValue(commandIndex, itemPath, item, table)
			if err != nil {
				return DiffValue{}, err
			}
			normalized.Items[i] = normalizedItem
		}

	case DiffObject:
		keys := make([]string, 0, len(value.Fields))
		for key := range value.Fields {
			keys = append(keys, key)
		}
		sort.Strings(keys)

		normalized.Fields = make(map[string]DiffValue, len(value.Fields))
		for _, key := range keys {
			fieldPath := path + "/" + jsonPointerEscape(key)
			normalizedField, err := normalizeValue(commandIndex, fieldPath, value.Fields[key], table)
			if err != nil {
				return DiffValue{}, err
			}
			normalized.Fields[key] = normalizedField
		}

	default:
		// validateShape already rejects unknown kinds; kept for safety.
		return DiffValue{}, fmt.Errorf("canonical path %q: unknown DiffKind %d", path, value.Kind)
	}

	return normalized, nil
}

func validateShape(value DiffValue) error {
	nonEmptyItems := len(value.Items) > 0
	nonEmptyFields := len(value.Fields) > 0

	switch value.Kind {
	case DiffScalar:
		if nonEmptyItems || nonEmptyFields {
			return fmt.Errorf("DiffScalar must not carry Items or Fields")
		}
	case DiffCreate, DiffRef:
		if value.Text == "" {
			return fmt.Errorf("%s requires a nonempty physical handle Text", kindName(value.Kind))
		}
		if nonEmptyItems || nonEmptyFields {
			return fmt.Errorf("%s must not carry Items or Fields", kindName(value.Kind))
		}
	case DiffList:
		if value.Text != "" || nonEmptyFields {
			return fmt.Errorf("DiffList must have empty Text and Fields")
		}
	case DiffObject:
		if value.Text != "" || nonEmptyItems {
			return fmt.Errorf("DiffObject must have empty Text and Items")
		}
	default:
		return fmt.Errorf("unknown DiffKind %d", value.Kind)
	}
	return nil
}

func kindName(kind DiffKind) string {
	switch kind {
	case DiffScalar:
		return "DiffScalar"
	case DiffCreate:
		return "DiffCreate"
	case DiffRef:
		return "DiffRef"
	case DiffList:
		return "DiffList"
	case DiffObject:
		return "DiffObject"
	default:
		return fmt.Sprintf("DiffKind(%d)", kind)
	}
}

// stageReleases maps physical release strings to live logical identities,
// rejecting unknown and duplicate releases. Released bindings are removed
// from the staged table so reuse of the same physical string allocates a new
// identity later.
func stageReleases(table *handleTable, releases []string) ([]string, error) {
	logicalReleases := make([]string, 0, len(releases))
	seenPhysical := make(map[string]bool, len(releases))
	seenLogical := make(map[string]bool, len(releases))

	for _, physical := range releases {
		if seenPhysical[physical] {
			return nil, fmt.Errorf("duplicate release of physical handle %q", physical)
		}
		seenPhysical[physical] = true

		logical, live := table.physicalToLogical[physical]
		if !live {
			return nil, fmt.Errorf("release of unknown or already released physical handle %q", physical)
		}
		if seenLogical[logical] {
			return nil, fmt.Errorf("duplicate release of logical identity %q", logical)
		}
		seenLogical[logical] = true

		logicalReleases = append(logicalReleases, logical)
		delete(table.physicalToLogical, physical)
		delete(table.logicalToPhysical, logical)
	}

	sort.Strings(logicalReleases)
	return logicalReleases, nil
}

// compareNormalized returns the first canonical path at which two normalized
// values differ. The empty path denotes the root. The second return value
// describes the nature of the difference. An empty path with differ=false
// means the values are equivalent.
func compareNormalized(a, b DiffValue) (path string, reason string, differ bool) {
	return compareNormalizedAt(a, b, "")
}

func compareNormalizedAt(a, b DiffValue, path string) (string, string, bool) {
	if canonicalKind(a.Kind) != canonicalKind(b.Kind) {
		return path, "value kind differs: " + kindName(a.Kind) + " != " + kindName(b.Kind), true
	}

	switch canonicalKind(a.Kind) {
	case DiffScalar:
		if a.Text != b.Text {
			return path, "scalar Text differs: " + strconv.Quote(a.Text) + " != " + strconv.Quote(b.Text), true
		}

	case DiffRef:
		// Both Create and Ref were canonicalized to DiffRef and their Text is
		// the logical identity: compare identity, not marker spelling.
		if a.Text != b.Text {
			return path, "handle identity differs: " + strconv.Quote(a.Text) + " != " + strconv.Quote(b.Text), true
		}

	case DiffList:
		n := len(a.Items)
		if len(b.Items) < n {
			n = len(b.Items)
		}
		for i := 0; i < n; i++ {
			itemPath := path + "/" + strconv.Itoa(i)
			if p, reason, yes := compareNormalizedAt(a.Items[i], b.Items[i], itemPath); yes {
				return p, reason, true
			}
		}
		if len(a.Items) != len(b.Items) {
			// First position that exists only on the longer side.
			extraPath := path + "/" + strconv.Itoa(n)
			return extraPath, fmt.Sprintf("list length differs: %d != %d", len(a.Items), len(b.Items)), true
		}

	case DiffObject:
		keys := make(map[string]struct{}, len(a.Fields)+len(b.Fields))
		for key := range a.Fields {
			keys[key] = struct{}{}
		}
		for key := range b.Fields {
			keys[key] = struct{}{}
		}
		sortedKeys := make([]string, 0, len(keys))
		for key := range keys {
			sortedKeys = append(sortedKeys, key)
		}
		sort.Strings(sortedKeys)

		for _, key := range sortedKeys {
			fieldPath := path + "/" + jsonPointerEscape(key)
			av, aok := a.Fields[key]
			bv, bok := b.Fields[key]
			if aok != bok {
				return fieldPath, "object field presence differs (missing versus present/empty)", true
			}
			if p, reason, yes := compareNormalizedAt(av, bv, fieldPath); yes {
				return p, reason, true
			}
		}
	}

	return "", "", false
}

// canonicalKind groups DiffCreate and DiffRef into one handle category after
// normalization.
func canonicalKind(kind DiffKind) DiffKind {
	if kind == DiffCreate || kind == DiffRef {
		return DiffRef
	}
	return kind
}

func releasesEqual(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// deepCopyOutput returns a deep copy of an observation so adapters are free
// to recycle the buffers/maps they returned after the callback.
func deepCopyOutput(output DiffOutput) DiffOutput {
	copyOutput := DiffOutput{
		Value:   deepCopyValue(output.Value),
		Release: nil,
	}
	if output.Release != nil {
		copyOutput.Release = append(make([]string, 0, len(output.Release)), output.Release...)
	}
	return copyOutput
}

func deepCopyValue(value DiffValue) DiffValue {
	copyValue := DiffValue{
		Kind: value.Kind,
		Text: value.Text,
	}
	if value.Items != nil {
		copyValue.Items = make([]DiffValue, len(value.Items))
		for i := range value.Items {
			copyValue.Items[i] = deepCopyValue(value.Items[i])
		}
	}
	if value.Fields != nil {
		copyValue.Fields = make(map[string]DiffValue, len(value.Fields))
		for key, field := range value.Fields {
			copyValue.Fields[key] = deepCopyValue(field)
		}
	}
	return copyValue
}
