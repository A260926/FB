// Package splaytree supplies the ordered-tree API used by model3d.
// This independently authored adapter uses AVL balancing, not upstream code.
package splaytree

import "fmt"

type Value[T any] interface{ Compare(T) int }
type Node[T Value[T]] struct {
	Value       T
	Left, Right *Node[T]
	level       int
}
type Tree[T Value[T]] struct{ Root *Node[T] }

func height[T Value[T]](n *Node[T]) int {
	if n == nil {
		return 0
	}
	return n.level
}
func update[T Value[T]](n *Node[T]) {
	n.level = height(n.Left) + 1
	if h := height(n.Right) + 1; h > n.level {
		n.level = h
	}
}
func left[T Value[T]](n *Node[T]) *Node[T] {
	x := n.Right
	n.Right = x.Left
	x.Left = n
	update(n)
	update(x)
	return x
}
func right[T Value[T]](n *Node[T]) *Node[T] {
	x := n.Left
	n.Left = x.Right
	x.Right = n
	update(n)
	update(x)
	return x
}
func balance[T Value[T]](n *Node[T]) *Node[T] {
	if n == nil {
		return nil
	}
	update(n)
	b := height(n.Left) - height(n.Right)
	if b > 1 {
		if height(n.Left.Right) > height(n.Left.Left) {
			n.Left = left(n.Left)
		}
		return right(n)
	}
	if b < -1 {
		if height(n.Right.Left) > height(n.Right.Right) {
			n.Right = right(n.Right)
		}
		return left(n)
	}
	return n
}
func insert[T Value[T]](n *Node[T], v T) *Node[T] {
	if n == nil {
		return &Node[T]{Value: v, level: 1}
	}
	if v.Compare(n.Value) < 0 {
		n.Left = insert(n.Left, v)
	} else {
		n.Right = insert(n.Right, v)
	}
	return balance(n)
}
func remove[T Value[T]](n *Node[T], v Value[T]) *Node[T] {
	if n == nil {
		panic("missing tree value")
	}
	cmp := v.Compare(n.Value)
	if cmp < 0 {
		n.Left = remove[T](n.Left, v)
	} else if cmp > 0 {
		n.Right = remove[T](n.Right, v)
	} else {
		if n.Left == nil {
			return n.Right
		}
		if n.Right == nil {
			return n.Left
		}
		x := n.Right
		for x.Left != nil {
			x = x.Left
		}
		n.Value = x.Value
		n.Right = remove[T](n.Right, x.Value)
	}
	return balance(n)
}
func (t *Tree[T]) Insert(v T)        { t.Root = insert(t.Root, v) }
func (t *Tree[T]) Delete(v Value[T]) { t.Root = remove[T](t.Root, v) }
func (t *Tree[T]) Min() T {
	n := t.Root
	if n == nil {
		panic("empty tree")
	}
	for n.Left != nil {
		n = n.Left
	}
	return n.Value
}
func (t *Tree[T]) Max() T {
	n := t.Root
	if n == nil {
		panic("empty tree")
	}
	for n.Right != nil {
		n = n.Right
	}
	return n.Value
}
func (t *Tree[T]) Len() int               { return t.Root.Len() }
func (t *Tree[T]) Height() int            { return height(t.Root) }
func (t *Tree[T]) Iterate(f func(T) bool) { t.Root.Iterate(f) }
func (n *Node[T]) Len() int {
	if n == nil {
		return 0
	}
	return 1 + n.Left.Len() + n.Right.Len()
}
func (n *Node[T]) Iterate(f func(T) bool) bool {
	if n == nil {
		return true
	}
	return n.Left.Iterate(f) && f(n.Value) && n.Right.Iterate(f)
}
func (n *Node[T]) String() string {
	if n == nil {
		return "<nil>"
	}
	return fmt.Sprint(n.Value, "(", n.Left, ",", n.Right, ")")
}
func (t *Tree[T]) String() string { return t.Root.String() }
