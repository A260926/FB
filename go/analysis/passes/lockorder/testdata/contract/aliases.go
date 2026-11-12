package aliases

import "sync"

type holder struct {
	mu sync.Mutex
	n  int
}

type node struct {
	mu sync.Mutex
}

type fake struct{ mu sync.Mutex }

func (f *fake) Lock()   {}
func (f *fake) Unlock() {}

type locker interface {
	Lock()
	Unlock()
}

var gmu sync.Mutex
var gholder holder

// aliasLocks resolves the p := &gmu, q := p alias chain to the same global
// lock before taking gholder.mu, yielding the single edge gmu -> gholder.mu.
func aliasLocks() {
	p := &gmu
	q := p
	q.Lock()
	gholder.mu.Lock()
	gholder.mu.Unlock()
	gmu.Unlock()
}

// fieldLocks reaches a lock through a definite struct field path r :=
// &gholder; r.mu, in the same global order as aliasLocks.
func fieldLocks() {
	r := &gholder
	gmu.Lock()
	r.mu.Lock()
	r.mu.Unlock()
	gmu.Unlock()
}

// blockScoped gives the same name mu to two different go/types bindings in
// two blocks; each denotes its own definite object. The if/else branches are
// mutually exclusive, so no edge (and no phantom double holding) is produced.
func blockScoped(x bool) {
	{
		mu := &gmu
		mu.Lock()
		mu.Unlock()
	}
	{
		mu := &gholder.mu
		mu.Lock()
		mu.Unlock()
	}
	if x {
		gmu.Lock()
		gmu.Unlock()
	} else {
		gholder.mu.Lock()
		gholder.mu.Unlock()
	}
}

// overwrite tracks a definite overwriting assignment: after p is repointed,
// p.Lock denotes gholder.mu, not gmu.
func overwrite() {
	p := &gmu
	p = &gholder.mu
	p.Lock()
	p.Unlock()
}

// unknowns cannot give a map-indexed pointer or a fresh heap allocation a
// definite identity; the function is marked incomplete but no identity is
// guessed.
func unknowns(m map[string]*node) {
	m["a"].mu.Lock()
	x := &node{}
	x.mu.Lock()
	_ = x
}

// closureCase launches a goroutine whose closure writes a captured pointer;
// the enclosing function is marked incomplete rather than treated as plain
// straight-line code.
func closureCase() {
	p := &gmu
	go func() {
		p = &gholder.mu
	}()
	p.Lock()
	p.Unlock()
}

// userMethod defines methods literally named Lock/Unlock on a user type;
// they must not be recognized as mutex operations.
func userMethod(f *fake) {
	f.Lock()
	f.Unlock()
}

// ifaceCase dispatches through an interface; the dynamic callee is unknown
// and the function is marked incomplete.
func ifaceCase(l locker) {
	l.Lock()
	l.Unlock()
}

var rw sync.RWMutex

// rwCase uses an RWMutex, which this pass does not model: the function is
// marked incomplete rather than treating RLock as a Mutex Lock.
func rwCase() {
	rw.RLock()
	rw.RUnlock()
}

// tryCase uses TryLock in straight-line code; the TryLock family is not
// modeled.
func tryCase() {
	acquired := gmu.TryLock()
	if acquired {
		gmu.Unlock()
	}
}

func condHelper() bool {
	gmu.Lock()
	gmu.Unlock()
	return true
}

// condCase performs a lock-helper call inside a branch condition; it must be
// reported as incomplete instead of being executed as straight-line code.
func condCase() {
	if condHelper() {
		gholder.mu.Lock()
		gholder.mu.Unlock()
	}
}

// loopCase acquires two locks in a fixed order on every iteration. The loop
// back edge joins at the loop head; reaching the fixed point must not invent
// a reverse edge or a cycle.
func loopCase() {
	for i := 0; i < 3; i++ {
		gmu.Lock()
		gholder.mu.Lock()
		gholder.mu.Unlock()
		gmu.Unlock()
	}
}
