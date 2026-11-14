package main

var Input = 19
var Result int

func leaf(x int) int {
	if x&1 == 0 {
		return x*3 + 1
	}
	return x ^ 0x55
}

func middle(x int) int {
	return leaf(x) + leaf(x+2)
}

//go:noinline
func root(x int) int {
	a := middle(x)
	b := middle(x + 9)
	return a + b + leaf(x+1)
}

func main() { Result = root(Input) }
