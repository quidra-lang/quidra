package main

import "cmp"

func maxOf[T cmp.Ordered](a, b T) T {
	if a > b {
		return a
	}
	return b
}

func main() { _ = maxOf[[]int](nil, nil) }
