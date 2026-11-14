package chromedp

import (
	"github.com/chromedp/cdproto"
	"github.com/chromedp/cdproto/cdp"
	"slices"
)

func runListeners(list []cancelableListener, ev any) []cancelableListener {
	for i := 0; i < len(list); {
		listener := list[i]
		select {
		case <-listener.ctx.Done():
			list = slices.Delete(list, i, i+1)
			continue
		default:
			listener.fn(ev)
			i++
		}
	}
	return list
}

// frameOp is a frame manipulation operation.
type frameOp func(*Frame)

func frameAttached(id cdp.FrameID) frameOp {
	return func(f *Frame) {
		f.ParentID = id
		setFrameState(f, FrameAttached)
	}
}

func frameDetached(f *Frame) {
	f.ParentID = EmptyFrameID
	clearFrameState(f, FrameAttached)
}

func frameStartedLoading(f *Frame) {
	setFrameState(f, FrameLoading)
}

func frameStoppedLoading(f *Frame) {
	clearFrameState(f, FrameLoading)
}

// setFrameState sets the frame state via bitwise or (|).
func setFrameState(f *Frame, fs FrameState) {
	f.State |= fs
}

// clearFrameState clears the frame state via bit clear (&^).
func clearFrameState(f *Frame, fs FrameState) {
	f.State &^= fs
}

// nodeOp is a node manipulation operation.
type nodeOp func(*Node)

// walk adds the node and its descendants to the index m, and links them to
// their parents.
func walk(m map[cdp.NodeID]*Node, n *Node) {
	n.RLock()
	defer n.RUnlock()
	m[n.NodeID] = n

	for _, c := range n.Children {
		c.Lock()
		c.Parent = n
		c.Unlock()

		walk(m, c)
	}

	for _, c := range n.ShadowRoots {
		c.Lock()
		c.Parent = n
		c.Unlock()

		walk(m, c)
	}

	for _, c := range n.PseudoElements {
		c.Lock()
		c.Parent = n
		c.Unlock()

		walk(m, c)
	}

	for _, c := range []*Node{n.ContentDocument, n.TemplateContent} {
		if c == nil {
			continue
		}

		c.Lock()
		c.Parent = n
		c.Unlock()

		walk(m, c)
	}
}

// detach removes the node and its subtree from the index m, and invalidates
// every node in it. The subtree holds the children, the shadow roots, the
// pseudo elements, the content document and the template content. The
// distributed nodes of a node only reference nodes of other subtrees, and
// they stay.
//
// The caller holds the lock of the frame. detach never holds the lock of a
// node while it takes the lock of a child, so a reader that walks the tree
// up, as FullXPath does, cannot block it.
func detach(m map[cdp.NodeID]*Node, n *Node) {
	n.RLock()
	children := n.Children
	shadowRoots := n.ShadowRoots
	pseudoElements := n.PseudoElements
	contentDocument := n.ContentDocument
	templateContent := n.TemplateContent
	n.RUnlock()

	if m[n.NodeID] == n {
		delete(m, n.NodeID)
	}
	n.invalidate()

	for _, c := range children {
		detach(m, c)
	}
	for _, c := range shadowRoots {
		detach(m, c)
	}
	for _, c := range pseudoElements {
		detach(m, c)
	}
	if contentDocument != nil {
		detach(m, contentDocument)
	}
	if templateContent != nil {
		detach(m, templateContent)
	}
}

func setChildNodes(m map[cdp.NodeID]*Node, nodes []*Node) nodeOp {
	return func(n *Node) {
		n.Lock()
		n.Children = nodes
		n.Unlock()

		walk(m, n)
	}
}

func attributeModified(name, value string) nodeOp {
	return func(n *Node) {
		n.Lock()
		defer n.Unlock()

		var found bool
		var i int
		for ; i < len(n.Attributes); i += 2 {
			if n.Attributes[i] == name {
				found = true
				break
			}
		}

		if found {
			n.Attributes[i] = name
			n.Attributes[i+1] = value
		} else {
			n.Attributes = append(n.Attributes, name, value)
		}
	}
}

func attributeRemoved(name string) nodeOp {
	return func(n *Node) {
		n.Lock()
		defer n.Unlock()

		var a []string
		for i := 0; i < len(n.Attributes); i += 2 {
			if n.Attributes[i] == name {
				continue
			}
			a = append(a, n.Attributes[i], n.Attributes[i+1])
		}
		n.Attributes = a
	}
}

func inlineStyleInvalidated(ids []cdp.NodeID) nodeOp {
	return func(n *Node) {
	}
}

func characterDataModified(characterData string) nodeOp {
	return func(n *Node) {
		n.Lock()
		defer n.Unlock()

		n.Value = characterData
	}
}

func childNodeCountUpdated(count int64) nodeOp {
	return func(n *Node) {
		n.Lock()
		defer n.Unlock()

		n.ChildNodeCount = count
	}
}

func childNodeInserted(m map[cdp.NodeID]*Node, prevID cdp.NodeID, c *Node) nodeOp {
	return func(n *Node) {
		n.Lock()
		n.Children = insertNode(n.Children, prevID, c)
		n.Unlock()

		walk(m, n)
	}
}

func childNodeRemoved(m map[cdp.NodeID]*Node, id cdp.NodeID) nodeOp {
	return func(n *Node) {
		n.Lock()
		var c *Node
		n.Children, c = removeNode(n.Children, id)
		n.Unlock()

		if c == nil {
			// The parent has no child with this id. The event can be a
			// duplicate, or it can name a node of another branch. Leave
			// the index alone.
			return
		}
		detach(m, c)
	}
}

func shadowRootPushed(m map[cdp.NodeID]*Node, c *Node) nodeOp {
	return func(n *Node) {
		n.Lock()
		n.ShadowRoots = append(n.ShadowRoots, c)
		n.Unlock()

		walk(m, n)
	}
}

func shadowRootPopped(m map[cdp.NodeID]*Node, id cdp.NodeID) nodeOp {
	return func(n *Node) {
		n.Lock()
		var c *Node
		n.ShadowRoots, c = removeNode(n.ShadowRoots, id)
		n.Unlock()

		if c == nil {
			// The host has no shadow root with this id. Leave the index
			// alone.
			return
		}
		detach(m, c)
	}
}

func pseudoElementAdded(m map[cdp.NodeID]*Node, c *Node) nodeOp {
	return func(n *Node) {
		n.Lock()
		n.PseudoElements = append(n.PseudoElements, c)
		n.Unlock()

		walk(m, n)
	}
}

func pseudoElementRemoved(m map[cdp.NodeID]*Node, id cdp.NodeID) nodeOp {
	return func(n *Node) {
		n.Lock()
		var c *Node
		n.PseudoElements, c = removeNode(n.PseudoElements, id)
		n.Unlock()

		if c == nil {
			// The element has no pseudo element with this id. Leave the
			// index alone.
			return
		}
		detach(m, c)
	}
}

func distributedNodesUpdated(nodes []*cdp.BackendNode) nodeOp {
	return func(n *Node) {
		n.Lock()
		defer n.Unlock()

		n.DistributedNodes = nodes
	}
}

func scrollableFlagUpdated(isScrollable bool) nodeOp {
	return func(n *Node) {
		n.Lock()
		defer n.Unlock()

		n.IsScrollable = isScrollable
	}
}

func adRelatedStateUpdated(provenance *cdp.AdProvenance) nodeOp {
	return func(n *Node) {
		n.Lock()
		defer n.Unlock()

		n.AdProvenance = provenance
	}
}

func adoptedStyleSheetsModified(ids []cdp.StyleSheetID) nodeOp {
	return func(n *Node) {
		n.Lock()
		defer n.Unlock()

		n.AdoptedStyleSheets = ids
	}
}

func affectedByStartingStylesFlagUpdated(affected bool) nodeOp {
	return func(n *Node) {
		n.Lock()
		defer n.Unlock()

		n.AffectedByStartingStyles = affected
	}
}

func insertNode(n []*Node, prevID cdp.NodeID, c *Node) []*Node {
	var i int
	var found bool
	for ; i < len(n); i++ {
		if n[i].NodeID == prevID {
			found = true
			break
		}
	}

	if !found {
		return append([]*Node{c}, n...)
	}

	i++
	n = append(n, nil)
	copy(n[i+1:], n[i:])
	n[i] = c

	return n
}

// removeNode removes the node with the id from the list, and returns the
// list and the node. It returns the list unchanged and a nil node when the
// list has no node with the id.
func removeNode(n []*Node, id cdp.NodeID) ([]*Node, *Node) {
	if len(n) == 0 {
		return n, nil
	}

	var found bool
	var i int
	for ; i < len(n); i++ {
		if n[i].NodeID == id {
			found = true
			break
		}
	}

	if !found {
		return n, nil
	}

	c := n[i]
	return slices.Delete(n, i, i+1), c
}

// isCouldNotComputeBoxModelError unwraps err as a MessageError and determines
// if it is a compute box model error.
func isCouldNotComputeBoxModelError(err error) bool {
	e, ok := err.(*cdproto.Error)
	return ok && e.Code == -32000 && e.Message == "Could not compute box model."
}

// ptr returns a pointer to a copy of v. It sets the optional fields of the
// protocol types, which are pointers.
func ptr[T any](v T) *T {
	return &v
}
