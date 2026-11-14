package chromedp

import (
	"context"
	"errors"
	"fmt"
	"io"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/chromedp/cdproto"
	"github.com/chromedp/cdproto/cdp"
	jsonv2 "github.com/chromedp/cdproto/cdp/jsonv2"
	"github.com/chromedp/cdproto/dom"
	"github.com/chromedp/cdproto/runtime"
)

// The tests of this file replay DOM events with Target.domEvent, and check
// the lifetime of the nodes of the mirror: the index of the frame, the
// Invalidated channels, and the queries with FromNode. They need no
// browser. The target sends its protocol commands to a channel, and the
// tests answer them.

// lifetimeTarget is a Target with one frame, for the lifetime tests. The
// browser of the target is a channel that records the commands.
type lifetimeTarget struct {
	*Target

	frame    *Frame
	commands chan *cdproto.Message

	mu   sync.Mutex
	errs []string
}

// newLifetimeTarget returns a lifetimeTarget with one empty frame. The
// frame has an execution context, so ensureFrame succeeds once the frame
// has a root node.
func newLifetimeTarget() *lifetimeTarget {
	lt := &lifetimeTarget{commands: make(chan *cdproto.Message, 32)}
	f := &Frame{Frame: &cdp.Frame{ID: "frame"}, Nodes: make(map[cdp.NodeID]*Node)}
	lt.frame = f
	lt.Target = &Target{
		browser:      &Browser{cmdQueue: lt.commands},
		frames:       map[cdp.FrameID]*Frame{f.ID: f},
		execContexts: map[cdp.FrameID]runtime.ExecutionContextID{f.ID: 1},
		cur:          f.ID,
		logf:         func(string, ...any) {},
		errf: func(format string, args ...any) {
			lt.mu.Lock()
			defer lt.mu.Unlock()
			lt.errs = append(lt.errs, strings.TrimSpace(fmt.Sprintf(format, args...)))
		},
	}
	return lt
}

// answer waits for the next command of the target, checks its method, and
// replies with the result. It returns an error after five seconds without
// a command, or when the method differs.
func (lt *lifetimeTarget) answer(method string, result any) error {
	res, err := jsonv2.Marshal(result, DefaultMarshalOptions)
	if err != nil {
		return fmt.Errorf("marshaling the result of %s: %w", method, err)
	}
	var cmd *cdproto.Message
	select {
	case cmd = <-lt.commands:
	case <-time.After(5 * time.Second):
		return fmt.Errorf("the target sent no command, want %s", method)
	}
	if string(cmd.Method) != method {
		return fmt.Errorf("the target sent %s, want %s", cmd.Method, method)
	}
	lt.listenersMu.Lock()
	lt.listeners = runListeners(lt.listeners, &cdproto.Message{
		ID:        cmd.ID,
		SessionID: cmd.SessionID,
		Result:    res,
	})
	lt.listenersMu.Unlock()
	return nil
}

// mustAnswer calls answer, and fails the test on an error.
func (lt *lifetimeTarget) mustAnswer(t *testing.T, method string, result any) {
	t.Helper()
	if err := lt.answer(method, result); err != nil {
		t.Fatal(err)
	}
}

// noCommand fails the test when the target sent a command. It waits 50
// milliseconds, so a command of a broken query arrives first.
func (lt *lifetimeTarget) noCommand(t *testing.T) {
	t.Helper()
	select {
	case cmd := <-lt.commands:
		t.Fatalf("the target sent %s, want no command", cmd.Method)
	case <-time.After(50 * time.Millisecond):
	}
}

// updateDocument replays a DOM.documentUpdated event, and answers the
// DOM.getDocument command of the target with the root. It returns the new
// root of the frame.
func (lt *lifetimeTarget) updateDocument(root *cdp.Node) (*Node, error) {
	done := make(chan struct{})
	go func() {
		lt.domEvent(context.Background(), &dom.EventDocumentUpdated{})
		close(done)
	}()
	if err := lt.answer(dom.GetDocument.Method, &dom.GetDocumentResult{Root: root}); err != nil {
		return nil, err
	}
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		return nil, errors.New("domEvent did not finish DOM.documentUpdated")
	}
	lt.frame.RLock()
	defer lt.frame.RUnlock()
	return lt.frame.Root, nil
}

// mustUpdateDocument calls updateDocument, and fails the test on an error.
func (lt *lifetimeTarget) mustUpdateDocument(t *testing.T, root *cdp.Node) *Node {
	t.Helper()
	n, err := lt.updateDocument(root)
	if err != nil {
		t.Fatal(err)
	}
	return n
}

// node returns the node of the frame with the id, or fails the test.
func (lt *lifetimeTarget) node(t *testing.T, id cdp.NodeID) *Node {
	t.Helper()
	lt.frame.RLock()
	defer lt.frame.RUnlock()
	n := lt.frame.Nodes[id]
	if n == nil {
		t.Fatalf("node %d is not in the index", id)
	}
	return n
}

// mustNotIndex fails the test when the index holds a node with one of the
// IDs.
func (lt *lifetimeTarget) mustNotIndex(t *testing.T, ids ...cdp.NodeID) {
	t.Helper()
	lt.frame.RLock()
	defer lt.frame.RUnlock()
	for _, id := range ids {
		if lt.frame.Nodes[id] != nil {
			t.Errorf("node %d is still in the index", id)
		}
	}
}

// checkNoErrors fails the test when the target logged an error.
func (lt *lifetimeTarget) checkNoErrors(t *testing.T) {
	t.Helper()
	lt.mu.Lock()
	defer lt.mu.Unlock()
	if len(lt.errs) > 0 {
		t.Fatalf("the target logged errors: %v", lt.errs)
	}
}

// query runs the Nodes action with a CSS selector and the options. The
// context ends after five seconds, so a broken query fails the test
// instead of blocking it.
func (lt *lifetimeTarget) query(sel CSS, opts ...QueryOption) ([]*Node, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return Nodes(sel, opts...)(ctx, lt.Target)
}

// queryAnswered runs a CSS query, and answers the DOM.querySelector
// command that the query sends with the node ID.
func (lt *lifetimeTarget) queryAnswered(t *testing.T, id cdp.NodeID, sel CSS, opts ...QueryOption) ([]*Node, error) {
	t.Helper()
	type result struct {
		nodes []*Node
		err   error
	}
	ch := make(chan result, 1)
	go func() {
		nodes, err := lt.query(sel, opts...)
		ch <- result{nodes, err}
	}()
	lt.mustAnswer(t, dom.QuerySelector.Method, &dom.QuerySelectorResult{NodeID: id})
	select {
	case r := <-ch:
		return r.nodes, r.err
	case <-time.After(5 * time.Second):
		t.Fatal("the query did not finish")
		return nil, nil
	}
}

// lifetimeDoc builds a protocol document node with the children.
func lifetimeDoc(id cdp.NodeID, children ...*cdp.Node) *cdp.Node {
	return &cdp.Node{
		NodeID:         id,
		BackendNodeID:  cdp.BackendNodeID(1000 + id),
		NodeType:       NodeTypeDocument,
		NodeName:       "#document",
		Children:       children,
		ChildNodeCount: int64(len(children)),
		DocumentURL:    "http://localhost/form",
	}
}

// lifetimeEl builds a protocol element node with the attributes and the
// children.
func lifetimeEl(id cdp.NodeID, name string, attrs []string, children ...*cdp.Node) *cdp.Node {
	return &cdp.Node{
		NodeID:         id,
		BackendNodeID:  cdp.BackendNodeID(1000 + id),
		NodeType:       NodeTypeElement,
		NodeName:       strings.ToUpper(name),
		LocalName:      name,
		Attributes:     attrs,
		Children:       children,
		ChildNodeCount: int64(len(children)),
	}
}

// lifetimeClosed reports whether the channel is closed.
func lifetimeClosed(c <-chan struct{}) bool {
	select {
	case <-c:
		return true
	default:
		return false
	}
}

// mustBeValid fails the test when the node is invalid.
func mustBeValid(t *testing.T, n *Node) {
	t.Helper()
	if lifetimeClosed(n.Invalidated) {
		t.Fatalf("node %d is invalid, want valid", n.NodeID)
	}
}

// mustBeInvalid fails the test when the node is valid.
func mustBeInvalid(t *testing.T, n *Node) {
	t.Helper()
	if !lifetimeClosed(n.Invalidated) {
		t.Fatalf("node %d is valid, want invalid", n.NodeID)
	}
}

// lifetimeFormDoc builds the protocol tree of a page with a form. The
// field group of the form holds every kind of owned node: an input with a
// pseudo element, a template with its content, a shadow host with a shadow
// root, and an iframe with its content document.
func lifetimeFormDoc() *cdp.Node {
	input := lifetimeEl(7, "input", nil)
	input.PseudoElements = []*cdp.Node{{
		NodeID:        8,
		BackendNodeID: 1008,
		NodeType:      NodeTypeElement,
		NodeName:      "::before",
		LocalName:     "::before",
		PseudoType:    cdp.PseudoTypeBefore,
	}}

	template := lifetimeEl(9, "template", nil)
	template.TemplateContent = &cdp.Node{
		NodeID:         10,
		BackendNodeID:  1010,
		NodeType:       NodeTypeDocumentFragment,
		NodeName:       "#document-fragment",
		Children:       []*cdp.Node{lifetimeEl(11, "li", nil)},
		ChildNodeCount: 1,
	}

	host := lifetimeEl(12, "div", []string{"id", "host"})
	host.ShadowRoots = []*cdp.Node{{
		NodeID:         13,
		BackendNodeID:  1013,
		NodeType:       NodeTypeDocumentFragment,
		NodeName:       "#shadow-root",
		ShadowRootType: cdp.ShadowRootTypeOpen,
		Children:       []*cdp.Node{lifetimeEl(14, "slot", nil)},
		ChildNodeCount: 1,
	}}

	iframe := lifetimeEl(15, "iframe", nil)
	iframe.ContentDocument = lifetimeDoc(16, lifetimeEl(17, "body", nil))

	return lifetimeDoc(1,
		lifetimeEl(2, "html", nil,
			lifetimeEl(3, "body", nil,
				lifetimeEl(4, "div", []string{"id", "keep"},
					lifetimeEl(5, "span", nil)),
				lifetimeEl(6, "div", []string{"id", "form"},
					input, template, host, iframe))))
}

// TestDOMLifetimeRemovalOwnsWholeSubtree removes a branch with every kind
// of owned node, and checks that the whole subtree leaves the index and
// becomes invalid, while the rest of the document stays valid.
func TestDOMLifetimeRemovalOwnsWholeSubtree(t *testing.T) {
	t.Parallel()

	lt := newLifetimeTarget()
	lt.mustUpdateDocument(t, lifetimeFormDoc())

	// The slot of the shadow root distributes a node of the kept branch.
	// The distributed node is a reference to another subtree, and the
	// removal of the slot does not remove it.
	lt.domEvent(context.Background(), &dom.EventDistributedNodesUpdated{
		InsertionPointID: 14,
		DistributedNodes: []*cdp.BackendNode{{NodeType: NodeTypeElement, NodeName: "SPAN", BackendNodeID: 1005}},
	})

	// Save the handles that a caller keeps after a query.
	removed := make(map[cdp.NodeID]*Node)
	for _, id := range []cdp.NodeID{6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17} {
		removed[id] = lt.node(t, id)
	}
	kept := make(map[cdp.NodeID]*Node)
	for _, id := range []cdp.NodeID{1, 2, 3, 4, 5} {
		kept[id] = lt.node(t, id)
	}

	// The page replaces the form: the field group leaves the document.
	lt.domEvent(context.Background(), &dom.EventChildNodeRemoved{ParentNodeID: 3, NodeID: 6})

	// The whole subtree of the group is invalid and out of the index.
	for _, n := range removed {
		mustBeInvalid(t, n)
	}
	gone := make([]cdp.NodeID, 0, len(removed))
	for id := range removed {
		gone = append(gone, id)
	}
	lt.mustNotIndex(t, gone...)

	// The kept branch stays valid and indexed. The node that the slot
	// distributes is in the kept branch, and stays too.
	for _, n := range kept {
		mustBeValid(t, n)
	}
	for id := range kept {
		lt.node(t, id)
	}

	// The children of the body are the kept branch alone.
	body := kept[3]
	body.RLock()
	count := len(body.Children)
	var first *Node
	if count > 0 {
		first = body.Children[0]
	}
	body.RUnlock()
	if count != 1 || first != kept[4] {
		t.Errorf("the body has %d children, want the one kept branch", count)
	}

	// A removal that names a node of another branch changes nothing.
	lt.domEvent(context.Background(), &dom.EventChildNodeRemoved{ParentNodeID: 4, NodeID: 2})
	// A repeated removal changes nothing.
	lt.domEvent(context.Background(), &dom.EventChildNodeRemoved{ParentNodeID: 3, NodeID: 6})
	// A removal with a parent that is gone changes nothing.
	lt.domEvent(context.Background(), &dom.EventChildNodeRemoved{ParentNodeID: 6, NodeID: 7})

	for _, n := range kept {
		mustBeValid(t, n)
	}
	for id := range kept {
		lt.node(t, id)
	}
	lt.checkNoErrors(t)
}

// TestDOMLifetimeShadowRootAndPseudoElement pops a shadow root and removes
// a pseudo element, and checks that only the owned subtrees leave the
// index and become invalid.
func TestDOMLifetimeShadowRootAndPseudoElement(t *testing.T) {
	t.Parallel()

	host := lifetimeEl(4, "div", []string{"id", "host"})
	host.ShadowRoots = []*cdp.Node{{
		NodeID:         5,
		BackendNodeID:  1005,
		NodeType:       NodeTypeDocumentFragment,
		NodeName:       "#shadow-root",
		ShadowRootType: cdp.ShadowRootTypeOpen,
		Children:       []*cdp.Node{lifetimeEl(6, "span", nil)},
		ChildNodeCount: 1,
	}}
	field := lifetimeEl(7, "div", []string{"id", "field"})
	field.PseudoElements = []*cdp.Node{{
		NodeID:         8,
		BackendNodeID:  1008,
		NodeType:       NodeTypeElement,
		NodeName:       "::before",
		LocalName:      "::before",
		PseudoType:     cdp.PseudoTypeBefore,
		Children:       []*cdp.Node{{NodeID: 9, BackendNodeID: 1009, NodeType: NodeTypeText, NodeName: "#text", NodeValue: "*"}},
		ChildNodeCount: 1,
	}}

	lt := newLifetimeTarget()
	lt.mustUpdateDocument(t, lifetimeDoc(1,
		lifetimeEl(2, "html", nil,
			lifetimeEl(3, "body", nil, host, field))))

	shadow, shadowChild := lt.node(t, 5), lt.node(t, 6)
	pseudo, pseudoChild := lt.node(t, 8), lt.node(t, 9)

	// Pop the shadow root. Its subtree leaves the index, the host stays.
	lt.domEvent(context.Background(), &dom.EventShadowRootPopped{HostID: 4, RootID: 5})
	mustBeInvalid(t, shadow)
	mustBeInvalid(t, shadowChild)
	lt.mustNotIndex(t, 5, 6)
	hostNode := lt.node(t, 4)
	mustBeValid(t, hostNode)
	hostNode.RLock()
	roots := len(hostNode.ShadowRoots)
	hostNode.RUnlock()
	if roots != 0 {
		t.Errorf("the host has %d shadow roots, want 0", roots)
	}

	// Remove the pseudo element. Its subtree leaves the index, the
	// element stays.
	lt.domEvent(context.Background(), &dom.EventPseudoElementRemoved{ParentID: 7, PseudoElementID: 8})
	mustBeInvalid(t, pseudo)
	mustBeInvalid(t, pseudoChild)
	lt.mustNotIndex(t, 8, 9)
	fieldNode := lt.node(t, 7)
	mustBeValid(t, fieldNode)
	fieldNode.RLock()
	pseudos := len(fieldNode.PseudoElements)
	fieldNode.RUnlock()
	if pseudos != 0 {
		t.Errorf("the field has %d pseudo elements, want 0", pseudos)
	}

	// Repeated removals and unknown IDs change nothing. A removal that
	// names the host element does not remove the host.
	lt.domEvent(context.Background(), &dom.EventShadowRootPopped{HostID: 4, RootID: 5})
	lt.domEvent(context.Background(), &dom.EventShadowRootPopped{HostID: 4, RootID: 99})
	lt.domEvent(context.Background(), &dom.EventPseudoElementRemoved{ParentID: 7, PseudoElementID: 8})
	lt.domEvent(context.Background(), &dom.EventPseudoElementRemoved{ParentID: 7, PseudoElementID: 4})
	mustBeValid(t, lt.node(t, 4))
	mustBeValid(t, lt.node(t, 7))
	lt.checkNoErrors(t)
}

// TestDOMLifetimeReusedNodeID inserts a new node with the ID of a removed
// node, and checks that the new node has a new lifecycle while the old
// handle stays invalid.
func TestDOMLifetimeReusedNodeID(t *testing.T) {
	t.Parallel()

	lt := newLifetimeTarget()
	lt.mustUpdateDocument(t, lifetimeDoc(1,
		lifetimeEl(2, "html", nil,
			lifetimeEl(3, "body", nil,
				lifetimeEl(4, "div", []string{"id", "group"},
					lifetimeEl(5, "input", nil))))))

	oldGroup := lt.node(t, 4)
	oldInput := lt.node(t, 5)

	// The page replaces the group, and the browser reuses the node IDs.
	lt.domEvent(context.Background(), &dom.EventChildNodeRemoved{ParentNodeID: 3, NodeID: 4})
	mustBeInvalid(t, oldGroup)
	mustBeInvalid(t, oldInput)

	lt.domEvent(context.Background(), &dom.EventChildNodeInserted{
		ParentNodeID:   3,
		PreviousNodeID: 0,
		Node: lifetimeEl(4, "div", []string{"id", "group"},
			lifetimeEl(5, "input", nil)),
	})

	// The new nodes have new channels, and the old handles stay invalid.
	newGroup, newInput := lt.node(t, 4), lt.node(t, 5)
	if newGroup == oldGroup || newInput == oldInput {
		t.Fatal("the index still holds the old nodes")
	}
	mustBeValid(t, newGroup)
	mustBeValid(t, newInput)
	mustBeInvalid(t, oldGroup)
	mustBeInvalid(t, oldInput)
	if newGroup.Invalidated == oldGroup.Invalidated {
		t.Error("the new node shares the Invalidated channel of the old node")
	}

	// The parent chain and the index are complete again.
	newGroup.RLock()
	groupParent := newGroup.Parent
	newGroup.RUnlock()
	if groupParent != lt.node(t, 3) {
		t.Error("the parent of the new group is not the body")
	}
	newInput.RLock()
	inputParent := newInput.Parent
	newInput.RUnlock()
	if inputParent != newGroup {
		t.Error("the parent of the new input is not the new group")
	}
	body := lt.node(t, 3)
	body.RLock()
	count := len(body.Children)
	var first *Node
	if count > 0 {
		first = body.Children[0]
	}
	body.RUnlock()
	if count != 1 || first != newGroup {
		t.Errorf("the body has %d children, want the one new group", count)
	}

	// A query from the old handle fails. A query from the new node works.
	if _, err := lt.query(CSS("input"), FromNode(oldGroup)); !errors.Is(err, ErrNodeDetached) {
		t.Fatalf("query from the old group: got %v, want ErrNodeDetached", err)
	}
	lt.noCommand(t)

	nodes, err := lt.queryAnswered(t, 5, CSS("input"), FromNode(newGroup))
	if err != nil {
		t.Fatal(err)
	}
	if len(nodes) != 1 || nodes[0] != newInput {
		t.Fatalf("the query returned %d nodes, want the new input", len(nodes))
	}
	lt.checkNoErrors(t)
}

// TestDOMLifetimeDocumentUpdate checks that a document update invalidates
// the old nodes that stayed, and that the nodes a removal already
// invalidated are not closed again.
func TestDOMLifetimeDocumentUpdate(t *testing.T) {
	t.Parallel()

	lt := newLifetimeTarget()
	lt.mustUpdateDocument(t, lifetimeDoc(1,
		lifetimeEl(2, "html", nil,
			lifetimeEl(3, "body", nil,
				lifetimeEl(4, "div", []string{"id", "a"}),
				lifetimeEl(5, "div", []string{"id", "b"})))))

	old := make(map[cdp.NodeID]*Node)
	for _, id := range []cdp.NodeID{1, 2, 3, 4, 5} {
		old[id] = lt.node(t, id)
	}

	// Remove one branch first. Its node is invalid before the update.
	lt.domEvent(context.Background(), &dom.EventChildNodeRemoved{ParentNodeID: 3, NodeID: 4})
	mustBeInvalid(t, old[4])

	// The document update invalidates the nodes that stayed. The removed
	// node is not closed again: a second close panics.
	root := lt.mustUpdateDocument(t, lifetimeDoc(11,
		lifetimeEl(12, "html", nil,
			lifetimeEl(13, "body", nil))))

	for _, id := range []cdp.NodeID{1, 2, 3, 4, 5} {
		mustBeInvalid(t, old[id])
	}

	// The index holds only the new document, and the new nodes are valid.
	lt.frame.RLock()
	count := len(lt.frame.Nodes)
	lt.frame.RUnlock()
	if count != 3 {
		t.Fatalf("the index holds %d nodes, want 3", count)
	}
	if root.NodeID != 11 {
		t.Fatalf("the new root is %d, want 11", root.NodeID)
	}
	second := make(map[cdp.NodeID]*Node)
	for _, id := range []cdp.NodeID{11, 12, 13} {
		second[id] = lt.node(t, id)
		mustBeValid(t, second[id])
	}

	// A query from a node of the old document fails at once.
	if _, err := lt.query(CSS("div"), FromNode(old[5])); !errors.Is(err, ErrNodeDetached) {
		t.Fatalf("query from the old document: got %v, want ErrNodeDetached", err)
	}
	lt.noCommand(t)

	// A second update invalidates the second document the same way.
	lt.mustUpdateDocument(t, lifetimeDoc(21, lifetimeEl(22, "html", nil)))
	for _, n := range second {
		mustBeInvalid(t, n)
	}
	mustBeValid(t, lt.node(t, 21))
	lt.checkNoErrors(t)
}

// TestDOMLifetimeQueryDetachedFromNode runs queries from nodes that left
// the document, and checks that they fail with ErrNodeDetached before they
// send a protocol command.
func TestDOMLifetimeQueryDetachedFromNode(t *testing.T) {
	t.Parallel()

	iframe := lifetimeEl(6, "iframe", nil)
	iframe.ContentDocument = lifetimeDoc(7, lifetimeEl(8, "body", nil))

	lt := newLifetimeTarget()
	lt.mustUpdateDocument(t, lifetimeDoc(1,
		lifetimeEl(2, "html", nil,
			lifetimeEl(3, "body", nil,
				lifetimeEl(4, "div", []string{"id", "group"},
					lifetimeEl(5, "input", nil)),
				iframe,
				lifetimeEl(9, "div", []string{"id", "keep"},
					lifetimeEl(10, "span", nil))))))

	group := lt.node(t, 4)
	frame := lt.node(t, 6)

	// The page removes the group and the iframe.
	lt.domEvent(context.Background(), &dom.EventChildNodeRemoved{ParentNodeID: 3, NodeID: 4})
	lt.domEvent(context.Background(), &dom.EventChildNodeRemoved{ParentNodeID: 3, NodeID: 6})

	t.Run("RemovedNode", func(t *testing.T) {
		if _, err := lt.query(CSS("input"), FromNode(group)); !errors.Is(err, ErrNodeDetached) {
			t.Fatalf("got %v, want ErrNodeDetached", err)
		}
		lt.noCommand(t)
	})

	t.Run("RemovedIframe", func(t *testing.T) {
		// The saved iframe node still has its content document.
		frame.RLock()
		doc := frame.ContentDocument
		frame.RUnlock()
		if doc == nil {
			t.Fatal("the saved iframe node lost its content document")
		}
		if _, err := lt.query(CSS("body"), FromNode(frame)); !errors.Is(err, ErrNodeDetached) {
			t.Fatalf("got %v, want ErrNodeDetached", err)
		}
		lt.noCommand(t)
	})

	t.Run("NodeOfAnotherTarget", func(t *testing.T) {
		other := newLifetimeTarget()
		other.mustUpdateDocument(t, lifetimeDoc(1,
			lifetimeEl(2, "html", nil,
				lifetimeEl(3, "body", nil,
					lifetimeEl(4, "div", []string{"id", "group"})))))
		// The node is valid in its own target, and its ID exists in the
		// index of the first target too. It is still a stranger there.
		stranger := other.node(t, 4)
		mustBeValid(t, stranger)
		if _, err := lt.query(CSS("div"), FromNode(stranger)); !errors.Is(err, ErrNodeDetached) {
			t.Fatalf("got %v, want ErrNodeDetached", err)
		}
		lt.noCommand(t)
	})

	t.Run("ValidNode", func(t *testing.T) {
		// A node that is still in the tree works as the start of a query.
		nodes, err := lt.queryAnswered(t, 10, CSS("span"), FromNode(lt.node(t, 9)))
		if err != nil {
			t.Fatal(err)
		}
		if len(nodes) != 1 || nodes[0] != lt.node(t, 10) {
			t.Fatalf("the query returned %d nodes, want the span", len(nodes))
		}
	})

	t.Run("NoFromNodeWaits", func(t *testing.T) {
		// A query without FromNode waits for the document, as before.
		empty := newLifetimeTarget()
		ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
		defer cancel()
		_, err := Nodes(CSS("div"))(ctx, empty.Target)
		if !errors.Is(err, context.DeadlineExceeded) {
			t.Fatalf("got %v, want the context deadline", err)
		}
		if errors.Is(err, ErrNodeDetached) {
			t.Fatal("a query without FromNode must wait for the document")
		}
	})

	lt.checkNoErrors(t)
}

// TestDOMLifetimeConcurrentReaders runs readers and a query while events
// update the tree, and checks that the locks do not deadlock.
func TestDOMLifetimeConcurrentReaders(t *testing.T) {
	t.Parallel()

	lt := newLifetimeTarget()
	root := lt.mustUpdateDocument(t, lifetimeDoc(1,
		lifetimeEl(2, "html", nil,
			lifetimeEl(3, "body", nil,
				lifetimeEl(4, "div", []string{"id", "keep"},
					lifetimeEl(5, "span", nil)),
				lifetimeEl(6, "div", []string{"id", "drop"},
					lifetimeEl(7, "p", nil),
					lifetimeEl(8, "p", nil))))))

	ctx := context.Background()

	// A reader holds the read lock of the parent of the subtree that the
	// removal takes apart. The removal waits for the reader, and it
	// finishes after the reader lets go.
	body := lt.node(t, 3)
	dropped := lt.node(t, 6)
	leaf := lt.node(t, 8)
	holding := make(chan struct{})
	letGo := make(chan struct{})
	readerDone := make(chan struct{})
	go func() {
		defer close(readerDone)
		body.RLock()
		close(holding)
		<-letGo
		body.RUnlock()
	}()
	<-holding
	eventDone := make(chan struct{})
	go func() {
		defer close(eventDone)
		lt.domEvent(ctx, &dom.EventChildNodeRemoved{ParentNodeID: 3, NodeID: 6})
	}()
	select {
	case <-eventDone:
		t.Fatal("the removal did not wait for the reader")
	case <-time.After(50 * time.Millisecond):
	}
	close(letGo)
	<-readerDone
	select {
	case <-eventDone:
	case <-time.After(5 * time.Second):
		t.Fatal("the removal did not finish: a lock is stuck")
	}
	mustBeInvalid(t, dropped)
	mustBeInvalid(t, leaf)

	// Readers walk the tree while a burst of events updates it. One
	// reader walks up from a node of the removed subtree.
	stop := make(chan struct{})
	var wg sync.WaitGroup
	for _, n := range []*Node{lt.node(t, 2), lt.node(t, 4), lt.node(t, 5), leaf} {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				select {
				case <-stop:
					return
				default:
				}
				_ = n.FullXPath()
				_ = n.AttributeValue("id")
			}
		}()
	}
	wg.Add(1)
	go func() {
		defer wg.Done()
		for {
			select {
			case <-stop:
				return
			default:
			}
			_, _ = root.WriteTo(io.Discard, "", "  ", true)
		}
	}()

	// A query looks up a node that stays in the document.
	queryCtx, stopQuery := context.WithCancel(ctx)
	wg.Add(1)
	go func() {
		defer wg.Done()
		for queryCtx.Err() == nil {
			_, _ = lt.query(CSS("html"), ByFunc(func(context.Context, *Target, *Node) ([]cdp.NodeID, error) {
				return []cdp.NodeID{2}, nil
			}))
		}
	}()

	// The burst inserts and removes subtrees.
	for i := 0; i < 20; i++ {
		id := cdp.NodeID(100 + i)
		lt.domEvent(ctx, &dom.EventChildNodeInserted{
			ParentNodeID:   3,
			PreviousNodeID: 0,
			Node: lifetimeEl(id, "div", nil,
				&cdp.Node{NodeID: id + 1000, NodeType: NodeTypeText, NodeName: "#text", NodeValue: "x"}),
		})
		lt.domEvent(ctx, &dom.EventChildNodeRemoved{ParentNodeID: 3, NodeID: id})
	}

	close(stop)
	stopQuery()
	waitDone := make(chan struct{})
	go func() {
		wg.Wait()
		close(waitDone)
	}()
	select {
	case <-waitDone:
	case <-time.After(10 * time.Second):
		t.Fatal("the readers and the events deadlocked")
	}

	// The kept branch is still valid and indexed.
	for _, id := range []cdp.NodeID{1, 2, 3, 4, 5} {
		mustBeValid(t, lt.node(t, id))
	}
	lt.checkNoErrors(t)
}

// lifetimeFormGroupDoc builds the protocol tree of a page with a form that
// has one field group.
func lifetimeFormGroupDoc() *cdp.Node {
	return lifetimeDoc(1,
		lifetimeEl(2, "html", nil,
			lifetimeEl(3, "body", nil,
				lifetimeEl(4, "div", []string{"id", "group"},
					lifetimeEl(5, "input", nil)))))
}

// A dynamic form renders its field group again when a value changes. The
// node of the old group is invalid after the replacement, and its
// Invalidated channel is closed. A query with FromNode and the old node
// returns an error that wraps ErrNodeDetached. Query the document again,
// and use the new node.
func ExampleFromNode_detached() {
	// A tab shows a form with a field group. chromedp keeps a mirror of
	// the document, and updates it from the events of the browser.
	lt := newLifetimeTarget()
	if _, err := lt.updateDocument(lifetimeFormGroupDoc()); err != nil {
		panic(err)
	}

	// A query saved the node of the field group.
	lt.frame.RLock()
	group := lt.frame.Nodes[4]
	lt.frame.RUnlock()

	// The user changes a value, and the page renders the field group
	// again. The browser sends a removal and an insertion.
	ctx := context.Background()
	lt.domEvent(ctx, &dom.EventChildNodeRemoved{ParentNodeID: 3, NodeID: 4})
	lt.domEvent(ctx, &dom.EventChildNodeInserted{
		ParentNodeID:   3,
		PreviousNodeID: 0,
		Node: lifetimeEl(9, "div", []string{"id", "group"},
			lifetimeEl(10, "input", nil)),
	})

	// The saved node is invalid now.
	invalid := false
	select {
	case <-group.Invalidated:
		invalid = true
	default:
	}
	fmt.Println("the saved group is invalid:", invalid)

	// A query from the saved node returns ErrNodeDetached.
	_, err := Nodes(CSS("input"), FromNode(group))(ctx, lt.Target)
	fmt.Println("query from the saved group:", err)
	fmt.Println("the error is ErrNodeDetached:", errors.Is(err, ErrNodeDetached))

	// Query the document again, and use the new node.
	found := make(chan *Node, 1)
	go func() {
		nodes, _ := Nodes(CSS("#group"))(ctx, lt.Target)
		if len(nodes) == 1 {
			found <- nodes[0]
		}
	}()
	if err := lt.answer(dom.QuerySelector.Method, &dom.QuerySelectorResult{NodeID: 9}); err != nil {
		panic(err)
	}
	newGroup := <-found
	valid := false
	select {
	case <-newGroup.Invalidated:
	default:
		valid = true
	}
	fmt.Println("the new group is valid:", valid)

	// Output:
	// the saved group is invalid: true
	// query from the saved group: selector "input": node detached
	// the error is ErrNodeDetached: true
	// the new group is valid: true
}
