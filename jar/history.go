package jar

import (
	"container/list"
	"net/http"
	"net/url"

	"github.com/PuerkitoBio/goquery"
)

// State represents a single, fully recovered page produced by one completed
// navigation. Every field belongs to the same request/response pair:
//
//   - Request is the original navigation request. Reload re-sends this
//     request (including its method, entry URL, headers and body) rather than
//     any request produced while following redirects.
//   - Response is the final response after redirects.
//   - Dom is the parsed document.
//   - Body is the decoded (for example gunzipped) response body, exactly as
//     Download serves it.
//   - Base is the relative URL base used to resolve links and form actions.
//     When the page has no <base> tag it equals the final document URL, so a
//     page never inherits the base of a previously visited page.
//   - Replay holds the stored request body used to re-send Request. It may be
//     nil for requests without a body. The value is reference counted and
//     must be released through ReleaseResources once the state is dropped by
//     every owner.
type State struct {
	Request  *http.Request
	Response *http.Response
	Dom      *goquery.Document
	Body     []byte
	Base     *url.URL
	Replay   *ReplayBody
}

// NewHistoryState creates and returns a new *State type.
func NewHistoryState(req *http.Request, resp *http.Response, dom *goquery.Document) *State {
	return &State{
		Request:  req,
		Response: resp,
		Dom:      dom,
	}
}

// RetainResources marks the replay body, if any, as having one more owner.
// It is used when a State is shared, for example through Browser.NewTab.
func (s *State) RetainResources() {
	if s == nil {
		return
	}
	if s.Replay != nil {
		s.Replay.Retain()
	}
}

// ReleaseResources frees resources owned exclusively by this State, notably
// the temporary file backing a replayed request body. It is safe to call on
// a nil state or a state without resources.
func (s *State) ReleaseResources() {
	if s == nil {
		return
	}
	if s.Replay != nil {
		s.Replay.Release()
	}
}

// History is a type that records browser state.
//
// Implementations own the States they hold through Push: a State removed by
// Pop, eviction, Clear or any other means must have State.ReleaseResources
// called exactly once by the implementation when no other reference to the
// State remains. Pop transfers ownership of the State, including its
// resources, to the caller. Custom implementations that cannot honor this
// contract should not store States carrying replay resources (States whose
// Replay field is non-nil).
type History interface {
	Clear()
	SetMax(max int)
	Len() int
	Push(p *State) int
	Pop() *State
	Top() *State
}

// Node holds stack values and points to the next element.
type Node struct {
	Value *State
	Next  *Node
}

// MemoryHistory is an in-memory implementation of the History interface.
//
// It honors the resource ownership contract described on History: States
// removed by Pop, eviction while trimming, SetMax shrinking, Clear or
// replacing the container have their replay resources released.
//
// A single MemoryHistory can be shared by several browser tabs created with
// Browser.NewTab. Tabs register themselves with Attach/Detach so the
// container knows when the last owner goes away.
type MemoryHistory struct {
	list    *list.List
	maxHist int
	tabs    int
}

// NewMemoryHistory creates and returns a new *StateHistory type. The
// container starts with a single owning browser tab; additional tabs created
// with Browser.NewTab register through Attach.
func NewMemoryHistory() *MemoryHistory {
	return &MemoryHistory{list: list.New(), tabs: 1}
}

// Attach registers a browser tab as an owner of the history.
func (his *MemoryHistory) Attach() {
	his.tabs++
}

// Detach unregisters a browser tab. It returns true when no tab owns the
// history anymore, in which case the caller is expected to Clear it so the
// last replay resources are released.
func (his *MemoryHistory) Detach() bool {
	if his.tabs > 0 {
		his.tabs--
	}
	return his.tabs == 0
}

// Len returns the number of states in the history.
func (his *MemoryHistory) Len() int {
	return his.list.Len()
}

// SetMax sets the max history length.  Setting values
// to 0 will disable history trimming, keeping a infinite list.
//
// Shrinking the limit drops and releases the oldest States immediately.
func (his *MemoryHistory) SetMax(max int) {
	his.maxHist = max
	his.trim()
}

// Clear removes all history and releases the resources of every stored
// State.
func (his *MemoryHistory) Clear() {
	for e := his.list.Front(); e != nil; e = e.Next() {
		e.Value.(*State).ReleaseResources()
	}
	his.list.Init()
}

// Push adds a new State at the front of the history.
func (his *MemoryHistory) Push(p *State) int {
	his.list.PushFront(p)
	his.trim()
	return his.list.Len()
}

// Pop removes and returns the State at the front of the history. Ownership
// of the State and its replay resources transfers to the caller.
func (his *MemoryHistory) Pop() *State {
	if his.list.Len() > 0 {
		return his.list.Remove(his.list.Front()).(*State)
	}
	return nil
}

// Top returns the State at the front of the history without removing it.
func (his *MemoryHistory) Top() *State {
	if his.list.Len() == 0 {
		return nil
	}
	return his.list.Front().Value.(*State)
}

// trim evicts States beyond maxHist, releasing their resources. A maxHist of
// zero keeps an infinite history.
func (his *MemoryHistory) trim() {
	if his.maxHist <= 0 {
		return
	}
	for his.list.Len() > his.maxHist {
		oldest := his.list.Back()
		state := his.list.Remove(oldest).(*State)
		state.ReleaseResources()
	}
}
