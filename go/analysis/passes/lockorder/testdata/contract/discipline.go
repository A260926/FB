package discipline

import "sync"

var dmu sync.Mutex

// unlockNotHeld releases a lock it never acquired: the lock is not held at
// the Unlock, which violates the helper contract.
func unlockNotHeld(a *sync.Mutex) {
	a.Unlock()
}

// unlockInherited is meant to be called while the caller already holds a. A
// conforming helper must not unlock a lock it did not acquire itself.
func unlockInherited(a *sync.Mutex) {
	a.Unlock()
}

// deferForeign registers a deferred unlock for a lock the helper itself never
// locked, another form of unlocking on the caller's behalf.
func deferForeign(a *sync.Mutex) {
	defer a.Unlock()
}

// cleanHelper is a conforming helper: it acquires and releases both locks on
// every return path and leaves nothing held for its caller.
func cleanHelper(a, b *sync.Mutex) {
	a.Lock()
	b.Lock()
	b.Unlock()
	a.Unlock()
}

func callClean() {
	cleanHelper(&dmu, &dmu)
}
