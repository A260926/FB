package jar

import (
	"testing"

	"github.com/headzoo/surf/internal/testassert"
)

func TestMemoryHistory(t *testing.T) {

	stack := NewMemoryHistory()

	page1 := &State{}
	stack.Push(page1)
	testassert.Equal(t, 1, stack.Len())
	testassert.Equal(t, page1, stack.Top())

	page2 := &State{}
	stack.Push(page2)
	testassert.Equal(t, 2, stack.Len())
	testassert.Equal(t, page2, stack.Top())

	page := stack.Pop()
	testassert.Equal(t, page, page2)
	testassert.Equal(t, 1, stack.Len())
	testassert.Equal(t, page1, stack.Top())

	page = stack.Pop()
	testassert.Equal(t, page, page1)
	testassert.Equal(t, 0, stack.Len())
}

func TestMemoryHistoryWithMax(t *testing.T) {

	stack := NewMemoryHistory()
	stack.SetMax(2)

	testassert.Equal(t, 2, stack.maxHist)

	page1 := &State{}
	stack.Push(page1)
	testassert.Equal(t, 1, stack.Len())
	testassert.Equal(t, page1, stack.Top())

	page2 := &State{}
	stack.Push(page2)
	testassert.Equal(t, 2, stack.Len())
	testassert.Equal(t, page2, stack.Top())

	page3 := &State{}
	stack.Push(page3)
	testassert.Equal(t, 2, stack.Len())
	testassert.Equal(t, page3, stack.Top())

	page4 := &State{}
	stack.Push(page4)
	testassert.Equal(t, 2, stack.Len())
	testassert.Equal(t, page4, stack.Top())

	page := stack.Pop()
	testassert.Equal(t, page, page4)
	testassert.Equal(t, 1, stack.Len())
	testassert.Equal(t, page3, stack.Top())

	page = stack.Pop()
	testassert.Equal(t, page, page3)
	testassert.Equal(t, 0, stack.Len())
}

func TestMemoryHistoryClear(t *testing.T) {

	stack := NewMemoryHistory()

	stack.Push(&State{})
	stack.Push(&State{})
	testassert.Equal(t, 2, stack.Len())
	stack.Clear()
	testassert.Equal(t, 0, stack.Len())
}
