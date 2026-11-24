package gen_test

import (
	"reflect"
	"testing"

	"github.com/leanovate/gopter/gen"
)

func TestPickNDeterministic(t *testing.T) {
	items := []int{1, 2, 3, 4, 5}
	pickGen := gen.PickN(items, 3)

	for seed := int64(0); seed < 100; seed++ {
		params1 := fixedParameters(10, seed)
		params2 := fixedParameters(10, seed)

		result, ok := pickGen(params1).Retrieve()
		if !ok {
			t.Fatalf("Sample failed for seed %d", seed)
		}
		result1Slice, ok := result.([]int)
		if !ok {
			t.Fatalf("Result 1 not slice for seed %d: %#v", seed, result)
		}

		result, ok = pickGen(params2).Retrieve()
		if !ok {
			t.Fatalf("Sample failed for seed %d", seed)
		}
		result2Slice, ok := result.([]int)
		if !ok {
			t.Fatalf("Result 2 not slice for seed %d: %#v", seed, result)
		}

		if !reflect.DeepEqual(result1Slice, result2Slice) {
			t.Errorf("Same seed produced different results for seed %d:\nSeed %d result 1: %v\nSeed %d result 2: %v", seed, seed, result1Slice, seed, result2Slice)
		}
	}
}

func TestPickN(t *testing.T) {
	items := []int{4, 2, 3, 5, 9, 1, 6, 8, 7}
	pickGen := gen.PickN(items, 5)

	for i := 0; i < 100; i++ {
		value, ok := pickGen.Sample()
		if !ok {
			t.Error("Sample was not ok")
			continue
		}

		result, ok := value.([]int)
		if !ok {
			t.Errorf("Sample not slice of int: %#v", value)
			continue
		}

		if len(result) != 5 {
			t.Errorf("Expected length 5, got %d: %v", len(result), result)
			continue
		}

		for _, item := range result {
			if !intSliceContains(items, item) {
				t.Errorf("Result contains item not in original: %d (result: %v, original: %v)", item, result, items)
				break
			}
		}

		lastIndex := -1
		for _, item := range result {
			currentIndex := intSliceIndex(items, item)
			if currentIndex == -1 {
				t.Errorf("Result item not found in original: %d", item)
				break
			}
			if currentIndex <= lastIndex {
				t.Errorf("Result not in original order: item %d at index %d should come after previous item at index %d (result: %v)",
					item, currentIndex, lastIndex, result)
				break
			}
			lastIndex = currentIndex
		}
	}
}

func TestPickNZero(t *testing.T) {
	items := []int{1, 2, 3, 4, 5}
	commonGeneratorTest(t, "pick n zero", gen.PickN(items, 0), func(value interface{}) bool {
		result, ok := value.([]int)
		return ok && len(result) == 0
	})
}

func TestPickNFromEmpty(t *testing.T) {
	commonGeneratorTest(t, "pick n from empty", gen.PickN([]int{}, 5), func(value interface{}) bool {
		result, ok := value.([]int)
		return ok && len(result) == 0
	})
}

func TestPickNMoreThanAvailable(t *testing.T) {
	items := []int{1, 2, 3}
	commonGeneratorTest(t, "pick n more than available", gen.PickN(items, 10), func(value interface{}) bool {
		result, ok := value.([]int)
		return ok && reflect.DeepEqual(result, items)
	})
}

func TestPickNVariety(t *testing.T) {
	items := []int{1, 2, 3, 4, 5, 6, 7, 8, 9}
	gen := gen.PickN(items, 5)

	first, ok := gen.Sample()
	if !ok {
		t.Fatal("First sample failed")
	}
	firstSlice := first.([]int)

	foundDifferent := false
	for i := 0; i < 10; i++ {
		sample, ok := gen.Sample()
		if !ok {
			t.Fatal("Sample failed")
		}
		slice := sample.([]int)
		if !reflect.DeepEqual(slice, firstSlice) {
			foundDifferent = true
			break
		}
	}

	if !foundDifferent {
		t.Error("Generator produced same selection 10 times")
	}
}

func intSliceContains(slice []int, val int) bool {
	for _, item := range slice {
		if item == val {
			return true
		}
	}
	return false
}

func intSliceIndex(slice []int, val int) int {
	for i, item := range slice {
		if item == val {
			return i
		}
	}
	return -1
}
