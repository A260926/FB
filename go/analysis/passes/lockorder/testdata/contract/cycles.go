package cycles

import "sync"

var A, B, C sync.Mutex

func lockAB() {
	A.Lock()
	B.Lock()
	B.Unlock()
	A.Unlock()
}

func lockBA() {
	B.Lock()
	A.Lock()
	A.Unlock()
	B.Unlock()
}

func helper(x, y *sync.Mutex) {
	x.Lock()
	y.Lock()
	y.Unlock()
	x.Unlock()
}

func viaCall() {
	helper(&B, &A)
}

func selfLoop() {
	helper(&C, &C)
}

func branchOnly(x bool) {
	if x {
		A.Lock()
		A.Unlock()
	} else {
		B.Lock()
		B.Unlock()
	}
}
