package deferred

import "sync"

var da, db sync.Mutex

func deferBasic() {
	da.Lock()
	defer da.Unlock()
	db.Lock()
	db.Unlock()
}

func deferRepoint() {
	p := &da
	p.Lock()
	defer p.Unlock()
	p = &db
	p.Lock()
	p.Unlock()
}

func deferBranch(x bool) {
	da.Lock()
	if x {
		defer da.Unlock()
	} else {
		da.Unlock()
	}
}

func deferOther() {
	defer func() {}()
}
