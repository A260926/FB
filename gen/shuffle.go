package gen

import (
	"reflect"

	"github.com/leanovate/gopter"
)

// Shuffle takes a slice and returns a generator that produces shuffled copies
// Example: Shuffle([]int{1,2,3}) might produce []int{3,1,2} or []int{2,3,1}
// Note: This generator has no shrinker. Use small slices to keep failing test feedback readable.
func Shuffle(items interface{}) gopter.Gen {
	itemsVal := reflect.ValueOf(items)
	if itemsVal.Kind() != reflect.Slice {
		panic("Shuffle requires a slice")
	}

	n := itemsVal.Len()
	sliceType := itemsVal.Type()

	if n == 0 {
		return Const(reflect.MakeSlice(sliceType, 0, 0).Interface())
	}
	if n == 1 {
		return Const(items)
	}

	return func(genParams *gopter.GenParameters) *gopter.GenResult {
		result := reflect.MakeSlice(sliceType, n, n)
		for i := 0; i < n; i++ {
			result.Index(i).Set(itemsVal.Index(i))
		}

		for i := n - 1; i > 0; i-- {
			j := genParams.Rng.Intn(i + 1)
			tmp := result.Index(i).Interface()
			result.Index(i).Set(result.Index(j))
			result.Index(j).Set(reflect.ValueOf(tmp))
		}

		return gopter.NewGenResult(result.Interface(), gopter.NoShrinker)
	}
}
