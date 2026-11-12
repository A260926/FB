package summaries

import (
	"sync"
	"sync/atomic"
)

var mu1, mu2, mu3 sync.Mutex

type box struct {
	mu sync.Mutex
}

type link struct {
	mu   sync.Mutex
	next *link
}

// pair locks a then b and releases both on every return path.
func pair(a, b *sync.Mutex) {
	a.Lock()
	b.Lock()
	b.Unlock()
	a.Unlock()
}

func callerSlots() {
	pair(&mu1, &mu2)
	pair(&mu2, &mu3)
}

func sameLock() {
	pair(&mu1, &mu1)
}

func nested() {
	mu1.Lock()
	pair(&mu2, &mu3)
	mu1.Unlock()
}

func mixed(g *box) {
	g.mu.Lock()
	mu1.Lock()
	mu1.Unlock()
	g.mu.Unlock()
}

func callMixed() {
	var b box
	mixed(&b)
}

func badHelper(a *sync.Mutex) {
	a.Lock()
}

func callBad() {
	mu1.Lock()
	mu2.Lock()
	mu2.Unlock()
	mu1.Unlock()
	badHelper(&mu3)
}

func extCall(v *atomic.Int64) {
	mu1.Lock()
	v.Add(1)
	mu1.Unlock()
}

func recFinite(n int) {
	if n <= 0 {
		return
	}
	mu1.Lock()
	mu2.Lock()
	mu2.Unlock()
	mu1.Unlock()
	recFinite(n - 1)
}

func recGrowth(p *link) {
	mu1.Lock()
	p.mu.Lock()
	p.mu.Unlock()
	mu1.Unlock()
	if p.next != nil {
		recGrowth(p.next)
	}
}
