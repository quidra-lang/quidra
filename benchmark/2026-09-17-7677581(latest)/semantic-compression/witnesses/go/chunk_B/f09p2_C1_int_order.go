package main

import (
	"fmt"
	"sort"
)

func main() {
	xs := []int{3, 1, 2}
	alias := xs
	var a int32 = 4
	var b int32 = 9
	sort.Sort(sort.Reverse(sort.IntSlice(xs)))
	lt := a < b
	fmt.Println(xs, alias, lt)
}
