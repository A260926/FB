package godet

import (
	"encoding/json"
	"errors"
	"fmt"
	"reflect"
)

// AXStep is one step of an accessible-tree query.
//
// Role must be non-empty and is compared exactly against the browser's
// role.value. When Name is nil the step matches any accessible name;
// otherwise name.value is compared exactly. A pointer to an empty string
// matches nodes whose name is missing or empty, and is deliberately not the
// same as a nil Name. No trimming, case folding or Unicode normalization is
// applied.
type AXStep struct {
	Role string
	Name *string
}

// AXLimits bounds a single FindAX/FocusAX collection. All fields must be
// positive. The budget is measured against the raw AX tree: ignored nodes
// count as nodes and depth, and the root is at depth 1. MaxRequests counts
// the getRootAXNode request plus every getChildAXNodes request actually sent.
type AXLimits struct {
	MaxNodes    int
	MaxRequests int
	MaxDepth    int
}

// AXMatch is one control found by FindAX.
//
// AXNodeID is the Accessibility domain node id. BackendNodeID is the DOM
// backend node id (zero when the browser did not provide one; it is never
// guessed from AXNodeID). Role and Name hold the browser's role.value and
// name.value (a missing name is reported as ""). Ancestors is the matched
// node's exposed path from the document root down to the node itself,
// including the matched node as the last element.
type AXMatch struct {
	AXNodeID      string
	BackendNodeID int
	Role          string
	Name          string
	Ancestors     []AXStep
}

var (
	errAXEmptyPath = errors.New("godet: FindAX path is empty")
	errAXEmptyRole = errors.New("godet: FindAX path contains an empty Role")
	errAXBadBudget = errors.New("godet: AXLimits must have positive MaxNodes, MaxRequests and MaxDepth")
	errAXNoResult  = errors.New("godet: empty Accessibility response")
	errAXNoRoot    = errors.New("godet: Accessibility.getRootAXNode returned no node")
)

// axValue mirrors Accessibility.AXValue; only the fields the query relies on
// are decoded, and a wrong JSON type makes json.Unmarshal fail so that
// unexplainable field types are reported instead of silently ignored.
type axValue struct {
	Type  string `json:"type"`
	Value string `json:"value"`
}

// axNode mirrors the relevant fields of Accessibility.AXNode.
type axNode struct {
	NodeID           string   `json:"nodeId"`
	Ignored          bool     `json:"ignored"`
	Role             *axValue `json:"role"`
	Name             *axValue `json:"name"`
	ParentID         string   `json:"parentId"`
	ChildIDs         []string `json:"childIds"`
	BackendDOMNodeID int      `json:"backendDOMNodeId"`
	FrameID          string   `json:"frameId"`
}

func (n *axNode) roleValue() string {
	if n.Role == nil {
		return ""
	}
	return n.Role.Value
}

func (n *axNode) nameValue() string {
	if n.Name == nil {
		return ""
	}
	return n.Name.Value
}

func (n *axNode) toStep() AXStep {
	return AXStep{Role: n.roleValue(), Name: stringPtr(n.nameValue())}
}

func stringPtr(s string) *string { return &s }

// axCollector holds the state of one collection. Nothing is kept between
// calls: every FindAX/FocusAX builds a fresh collector.
type axCollector struct {
	remote   *RemoteDebugger
	frameID  string // frame requested by the caller ("" means the main frame)
	docFrame string // frame id of the document actually collected
	limits   AXLimits

	nodes     map[string]*axNode // every complete record received, keyed by nodeId
	requested map[string]bool    // parents whose children were already fetched (at most once each)
	parentOf  map[string]string  // provenance of every child edge that was traversed
	depth     map[string]int     // raw-tree depth, root at 1
	rootID    string

	requests int
}

func validateAXQuery(path []AXStep, limits AXLimits) error {
	if len(path) == 0 {
		return errAXEmptyPath
	}
	for _, step := range path {
		if step.Role == "" {
			return errAXEmptyRole
		}
	}
	if limits.MaxNodes <= 0 || limits.MaxRequests <= 0 || limits.MaxDepth <= 0 {
		return errAXBadBudget
	}
	return nil
}

// FindAX collects the accessibility tree of one document (the frame identified
// by frameID, or the current target's main frame when frameID is empty) and
// returns the controls whose exposed ancestor chain ends with path.
//
// The caller must have enabled the Accessibility domain first; FindAX never
// disables it. The tree is fetched only with Accessibility.getRootAXNode and
// Accessibility.getChildAXNodes, HTML is never parsed, accessible names are
// taken from the browser as-is, and no JavaScript is evaluated. A failed
// collection returns an error and no partial matches; no match returns a
// non-nil empty slice.
func (remote *RemoteDebugger) FindAX(frameID string, path []AXStep, limits AXLimits) ([]AXMatch, error) {
	if err := validateAXQuery(path, limits); err != nil {
		return nil, err
	}

	c := &axCollector{
		remote:    remote,
		frameID:   frameID,
		limits:    limits,
		nodes:     map[string]*axNode{},
		requested: map[string]bool{},
		parentOf:  map[string]string{},
		depth:     map[string]int{},
	}

	if err := c.collect(); err != nil {
		return nil, err
	}

	return c.match(path), nil
}

// FocusAX reruns the full FindAX lookup and, only when it yields exactly one
// match carrying a positive backend DOM node id, sends a single DOM.focus
// using backendNodeId. Zero matches, ambiguous matches, a missing backend id,
// budget exhaustion or a malformed tree all return an error without sending
// any focus request. A failing DOM.focus returns the protocol error as-is;
// there is no CSS fallback and no retry.
func (remote *RemoteDebugger) FocusAX(frameID string, path []AXStep, limits AXLimits) error {
	matches, err := remote.FindAX(frameID, path, limits)
	if err != nil {
		return err
	}

	backend, err := decideFocusBackend(matches)
	if err != nil {
		return err
	}

	if _, err := remote.SendRequest("DOM.focus", Params{"backendNodeId": backend}); err != nil {
		return err
	}
	return nil
}

// decideFocusBackend enforces the single, backend-addressable match rule
// without performing any protocol work, so the decision is testable on its
// own.
func decideFocusBackend(matches []AXMatch) (int, error) {
	if len(matches) == 0 {
		return 0, errors.New("godet: FocusAX matched no accessibility node")
	}
	if len(matches) > 1 {
		return 0, fmt.Errorf("godet: FocusAX path is ambiguous, %d nodes matched", len(matches))
	}
	if matches[0].BackendNodeID <= 0 {
		return 0, fmt.Errorf("godet: FocusAX node %s has no backendDOMNodeId", matches[0].AXNodeID)
	}
	return matches[0].BackendNodeID, nil
}

func (c *axCollector) collect() error {
	root, err := c.fetchRoot()
	if err != nil {
		return err
	}

	c.rootID = root.NodeID
	if root.FrameID != "" {
		c.docFrame = root.FrameID
	} else {
		c.docFrame = c.frameID
	}
	c.depth[root.NodeID] = 1

	// Explicit bounded worklist: a long ignored chain must not grow the
	// call stack.
	queue := []string{root.NodeID}
	done := map[string]bool{}

	for len(queue) > 0 {
		parentID := queue[0]
		queue = queue[1:]
		if done[parentID] {
			continue
		}

		parent, ok := c.nodes[parentID]
		if !ok {
			return fmt.Errorf("godet: AX node %q vanished before its children were processed", parentID)
		}

		if err := c.resolveChildren(parent); err != nil {
			return err
		}
		if err := c.validateEdges(parent, &queue); err != nil {
			return err
		}
		done[parentID] = true
	}

	return nil
}

func (c *axCollector) fetchRoot() (*axNode, error) {
	params := Params{}
	if c.frameID != "" {
		params["frameId"] = c.frameID
	}

	if err := c.budgetRequest(); err != nil {
		return nil, err
	}
	res, err := c.remote.SendRequest("Accessibility.getRootAXNode", params)
	if err != nil {
		return nil, err
	}
	if res == nil {
		return nil, errAXNoResult
	}

	raw, ok := res["node"]
	if !ok {
		return nil, errAXNoRoot
	}
	var node axNode
	if err := convert(raw, &node); err != nil {
		return nil, fmt.Errorf("godet: invalid root AX node: %w", err)
	}
	if node.NodeID == "" {
		return nil, errors.New("godet: root AX node has no nodeId")
	}
	if err := c.merge([]*axNode{&node}); err != nil {
		return nil, err
	}
	return c.nodes[node.NodeID], nil
}

// resolveChildren makes sure every id declared in childIds has a record,
// requesting the parent's children at most once.
func (c *axCollector) resolveChildren(parent *axNode) error {
	missing := c.missingChildren(parent)
	if len(missing) == 0 {
		return nil
	}
	if c.requested[parent.NodeID] {
		return fmt.Errorf("godet: AX node %q still does not report declared children %v after one fetch",
			parent.NodeID, missing)
	}
	if err := c.budgetRequest(); err != nil {
		return err
	}

	params := Params{"id": parent.NodeID}
	if c.docFrame != "" {
		params["frameId"] = c.docFrame
	}
	res, err := c.remote.SendRequest("Accessibility.getChildAXNodes", params)
	if err != nil {
		return err
	}
	if res == nil {
		return errAXNoResult
	}
	c.requested[parent.NodeID] = true

	raw, ok := res["nodes"]
	if !ok {
		return errors.New("godet: Accessibility.getChildAXNodes returned no nodes")
	}
	var children []*axNode
	if err := convert(raw, &children); err != nil {
		return fmt.Errorf("godet: invalid AX child nodes for %q: %w", parent.NodeID, err)
	}
	if err := c.merge(children); err != nil {
		return err
	}

	if still := c.missingChildren(parent); len(still) > 0 {
		return fmt.Errorf("godet: AX node %q is missing declared children %v", parent.NodeID, still)
	}
	return nil
}

func (c *axCollector) missingChildren(parent *axNode) []string {
	var missing []string
	for _, id := range parent.ChildIDs {
		if _, ok := c.nodes[id]; !ok {
			missing = append(missing, id)
		}
	}
	return missing
}

func (c *axCollector) budgetRequest() error {
	if c.requests >= c.limits.MaxRequests {
		return fmt.Errorf("godet: AX request budget exhausted at %d requests", c.requests)
	}
	c.requests++
	return nil
}

// merge stores records. Repeating the same nodeId with identical relevant
// fields is legal; conflicting relevant fields are an error instead of the
// newer record overwriting the older one.
func (c *axCollector) merge(incoming []*axNode) error {
	for _, node := range incoming {
		if node == nil {
			return errors.New("godet: AX response contains a null node")
		}
		if node.NodeID == "" {
			return errors.New("godet: AX node has an empty nodeId")
		}

		if existing, ok := c.nodes[node.NodeID]; ok {
			if !sameAXNode(existing, node) {
				return fmt.Errorf("godet: conflicting AX records for nodeId %q", node.NodeID)
			}
			continue
		}

		c.nodes[node.NodeID] = node
		if len(c.nodes) > c.limits.MaxNodes {
			return fmt.Errorf("godet: AX node budget exhausted at %d nodes", len(c.nodes))
		}
	}
	return nil
}

func sameAXNode(a, b *axNode) bool {
	return a.Ignored == b.Ignored &&
		a.ParentID == b.ParentID &&
		a.BackendDOMNodeID == b.BackendDOMNodeID &&
		reflect.DeepEqual(a.Role, b.Role) &&
		reflect.DeepEqual(a.Name, b.Name) &&
		sameStringSet(a.ChildIDs, b.ChildIDs)
}

// sameStringSet compares ordered child ids while treating a nil slice and an
// empty slice as the same (empty) set of children.
func sameStringSet(a, b []string) bool {
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

// validateEdges walks one parent's childIds in declared order (response array
// order is never used) and checks every structural invariant.
func (c *axCollector) validateEdges(parent *axNode, queue *[]string) error {
	seen := map[string]bool{}

	for _, childID := range parent.ChildIDs {
		if childID == "" {
			return fmt.Errorf("godet: AX node %q declares an empty childId", parent.NodeID)
		}
		if seen[childID] {
			return fmt.Errorf("godet: duplicate child edge %q under AX node %q", childID, parent.NodeID)
		}
		seen[childID] = true

		if other, ok := c.parentOf[childID]; ok && other != parent.NodeID {
			return fmt.Errorf("godet: AX node %q has two parents, %q and %q", childID, other, parent.NodeID)
		}

		child, ok := c.nodes[childID]
		if !ok {
			// resolveChildren already guarantees the record exists.
			return fmt.Errorf("godet: dangling AX child %q under %q", childID, parent.NodeID)
		}

		// Child documents belonging to another frame are a hard scope
		// boundary: the edge is remembered but the subtree is not entered.
		// OOPIF cross-session splicing is not supported. A node carrying any
		// frame id that is not this document's frame is treated as a boundary.
		if child.FrameID != "" && child.FrameID != c.docFrame {
			c.parentOf[childID] = parent.NodeID
			continue
		}

		// The root may point at a parent outside this document; that edge is
		// not part of the query. Every other parentId must match the edge.
		if childID != c.rootID && child.ParentID != "" && child.ParentID != parent.NodeID {
			return fmt.Errorf("godet: AX node %q parentId %q does not match referencing parent %q",
				childID, child.ParentID, parent.NodeID)
		}

		if _, reached := c.depth[childID]; reached {
			return fmt.Errorf("godet: AX tree contains a cycle through node %q", childID)
		}

		depth := c.depth[parent.NodeID] + 1
		if depth > c.limits.MaxDepth {
			return fmt.Errorf("godet: AX depth budget exhausted at depth %d", depth)
		}
		c.depth[childID] = depth
		c.parentOf[childID] = parent.NodeID
		*queue = append(*queue, childID)
	}
	return nil
}

// match projects the raw tree into the exposed tree and returns every
// candidate whose continuous exposed ancestor chain ends with path, in
// exposed-tree DFS preorder.
func (c *axCollector) match(path []AXStep) []AXMatch {
	exposed := c.buildExposedChildren()

	matches := []AXMatch{}
	type dfsEntry struct {
		nodeID string
		chain  []AXStep
	}

	// Seed the DFS with the exposed top-level nodes: the root itself when it
	// is exposed, otherwise the descendants lifted across an ignored root.
	var stack []dfsEntry
	for _, id := range c.exposedRoots(exposed) {
		stack = append(stack, dfsEntry{nodeID: id, chain: []AXStep{c.nodes[id].toStep()}})
	}

	for len(stack) > 0 {
		entry := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		node := c.nodes[entry.nodeID]

		if suffixMatches(entry.chain, path) {
			matches = append(matches, AXMatch{
				AXNodeID:      node.NodeID,
				BackendNodeID: node.BackendDOMNodeID,
				Role:          node.roleValue(),
				Name:          node.nameValue(),
				Ancestors:     append([]AXStep(nil), entry.chain...),
			})
		}

		kids := exposed[entry.nodeID]
		for i := len(kids) - 1; i >= 0; i-- {
			child := c.nodes[kids[i]]
			chain := make([]AXStep, 0, len(entry.chain)+1)
			chain = append(chain, entry.chain...)
			chain = append(chain, child.toStep())
			stack = append(stack, dfsEntry{nodeID: kids[i], chain: chain})
		}
	}

	return matches
}

// exposedRoots returns the first exposed layer of the document, lifting
// across an ignored root when necessary.
func (c *axCollector) exposedRoots(exposed map[string][]string) []string {
	root := c.nodes[c.rootID]
	if root == nil {
		return nil
	}
	if !root.Ignored {
		return []string{c.rootID}
	}
	return exposed[c.rootID]
}

// buildExposedChildren returns, for every node, the non-ignored descendants
// reached after lifting children of ignored nodes into the nearest
// non-ignored ancestor while preserving original order. All expansions use
// explicit stacks.
func (c *axCollector) buildExposedChildren() map[string][]string {
	out := map[string][]string{}

	for parentID, parent := range c.nodes {
		if parent.Ignored {
			continue
		}

		var lifted []string
		stack := append([]string(nil), reverse(parent.ChildIDs)...)
		for len(stack) > 0 {
			id := stack[len(stack)-1]
			stack = stack[:len(stack)-1]

			node, ok := c.nodes[id]
			if !ok {
				continue
			}
			if node.FrameID != "" && node.FrameID != c.docFrame {
				continue
			}
			if !node.Ignored {
				lifted = append(lifted, id)
				continue
			}
			for i := len(node.ChildIDs) - 1; i >= 0; i-- {
				stack = append(stack, node.ChildIDs[i])
			}
		}
		out[parentID] = lifted
	}
	return out
}

func reverse(in []string) []string {
	out := append([]string(nil), in...)
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	return out
}

// suffixMatches reports whether path equals the last len(path) steps of
// chain, role and name compared exactly. The candidate is always the last
// step, so an exposed group between the form and the control cannot be
// skipped.
func suffixMatches(chain []AXStep, path []AXStep) bool {
	if len(chain) < len(path) {
		return false
	}
	start := len(chain) - len(path)
	for i, step := range path {
		got := chain[start+i]
		if got.Role != step.Role {
			return false
		}
		if step.Name != nil && got.Name == nil {
			return false
		}
		if step.Name != nil && *got.Name != *step.Name {
			return false
		}
	}
	return true
}

// convert re-decodes a value already unmarshaled by SendRequest into the
// typed AX structures so wrong field types surface as decode errors.
func convert(in, out interface{}) error {
	raw, err := json.Marshal(in)
	if err != nil {
		return err
	}
	if err := json.Unmarshal(raw, out); err != nil {
		return err
	}
	return nil
}
