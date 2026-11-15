// Package essentials is an independently authored standard-library adapter.
// It is packaging support, not upstream essentials implementation.
package essentials

import (
	"fmt"
	"reflect"
	"runtime"
	"sort"
	"sync"
	"sync/atomic"
)

func MaxInt(v ...int) int {
	if len(v) == 0 {
		panic("empty maximum")
	}
	r := v[0]
	for _, x := range v[1:] {
		if x > r {
			r = x
		}
	}
	return r
}
func MinInt(v ...int) int {
	if len(v) == 0 {
		panic("empty minimum")
	}
	r := v[0]
	for _, x := range v[1:] {
		if x < r {
			r = x
		}
	}
	return r
}
func Reverse(v interface{}) {
	n := reflect.ValueOf(v).Len()
	swap := reflect.Swapper(v)
	for a, b := 0, n-1; a < b; a, b = a+1, b-1 {
		swap(a, b)
	}
}
func OrderedDelete(p interface{}, i int) {
	v := reflect.ValueOf(p).Elem()
	n := v.Len()
	if i < 0 || i >= n {
		panic("slice index")
	}
	reflect.Copy(v.Slice(i, n-1), v.Slice(i+1, n))
	v.Index(n - 1).Set(reflect.Zero(v.Type().Elem()))
	v.SetLen(n - 1)
}
func UnorderedDelete(p interface{}, i int) {
	v := reflect.ValueOf(p).Elem()
	n := v.Len()
	if i < 0 || i >= n {
		panic("slice index")
	}
	v.Index(i).Set(v.Index(n - 1))
	v.Index(n - 1).Set(reflect.Zero(v.Type().Elem()))
	v.SetLen(n - 1)
}

type jointSort struct {
	n     int
	less  func(int, int) bool
	swaps []func(int, int)
}

func (s jointSort) Len() int           { return s.n }
func (s jointSort) Less(i, j int) bool { return s.less(i, j) }
func (s jointSort) Swap(i, j int) {
	for _, f := range s.swaps {
		f(i, j)
	}
}
func VoodooSort(first interface{}, less func(int, int) bool, more ...interface{}) {
	n := reflect.ValueOf(first).Len()
	swaps := []func(int, int){reflect.Swapper(first)}
	for _, v := range more {
		if reflect.ValueOf(v).Len() != n {
			panic("slice length")
		}
		swaps = append(swaps, reflect.Swapper(v))
	}
	sort.Sort(jointSort{n, less, swaps})
}
func AddCtx(label string, err error) error {
	if err == nil {
		return nil
	}
	return fmt.Errorf("%s: %w", label, err)
}
func AddCtxTo(label string, err *error) { *err = AddCtx(label, *err) }

func ConcurrentMap(max, n int, f func(int)) {
	StatefulConcurrentMap(max, n, func() func(int) { return f })
}
func StatefulConcurrentMap(max, n int, g func() func(int)) {
	ReduceConcurrentMap(max, n, func() (func(int), func()) { return g(), nil })
}
func ReduceConcurrentMap(max, n int, g func() (func(int), func())) {
	if n <= 0 {
		return
	}
	if max == 0 {
		max = runtime.GOMAXPROCS(0)
	}
	if max < 1 {
		panic("worker count")
	}
	if max > n {
		max = n
	}
	var next int64
	var done sync.WaitGroup
	var reduction sync.Mutex
	done.Add(max)
	for w := 0; w < max; w++ {
		go func() {
			defer done.Done()
			apply, finish := g()
			for {
				idx := int(atomic.AddInt64(&next, 1)) - 1
				if idx >= n {
					break
				}
				apply(idx)
			}
			if finish != nil {
				reduction.Lock()
				finish()
				reduction.Unlock()
			}
		}()
	}
	done.Wait()
}
