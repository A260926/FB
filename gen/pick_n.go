package gen

import (
	"reflect"
	"sort"

	"github.com/leanovate/gopter"
)

// PickN takes a slice and a number, and returns a generator that produces
// subsets of exactly n items in their original order, randomly selected without duplicates
// Example: PickN([]int{1,2,3,4,5}, 3) might produce []int{1,3,5} or []int{2,4,5}
// Note: This generator has no shrinker. Use small slices to keep failing test feedback readable.
func PickN(items interface{}, number int) gopter.Gen {
	itemsVal := reflect.ValueOf(items)
	if itemsVal.Kind() != reflect.Slice {
		panic("PickN requires a slice")
	}

	length := itemsVal.Len()

	if number <= 0 || length == 0 {
		return Const(reflect.MakeSlice(itemsVal.Type(), 0, 0).Interface())
	}

	if number >= length {
		return Const(items)
	}

	sliceType := itemsVal.Type()

	return func(genParams *gopter.GenParameters) *gopter.GenResult {
		available := make([]int, length)
		for i := range available {
			available[i] = i
		}

		selected := make([]int, number)
		usedMask := make([]bool, length)
		for i := 0; i < number; i++ {
			pickIdx := genParams.Rng.Intn(length - i)
			count := 0
			actualIdx := -1
			for j := 0; j < length; j++ {
				if !usedMask[j] && count == pickIdx {
					actualIdx = j
					usedMask[j] = true
					break
				}
				if !usedMask[j] {
					count++
				}
			}
			selected[i] = actualIdx
		}

		sort.Ints(selected)

		result := reflect.MakeSlice(sliceType, number, number)
		for i, idx := range selected {
			result.Index(i).Set(itemsVal.Index(idx))
		}

		return gopter.NewGenResult(result.Interface(), gopter.NoShrinker)
	}
}
