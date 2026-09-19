package main

import (
	"fmt"
	"sort"
)

func main() {
	xs := []int{3, 1, 2}
	a := "apple"
	b := "banana"
	sort.Sort(sort.Reverse(sort.IntSlice(xs)))
	lt := a < b
	fmt.Println(xs, lt)
}
