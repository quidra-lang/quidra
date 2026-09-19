package main

import (
	"fmt"
	"sort"
)

func probe() ([]int, bool) {
	xs := []int{3, 1, 2}
	var a int32 = 4
	var b int32 = 9
	// BEGIN PROBE F09.P2
	sort.Sort(sort.Reverse(sort.IntSlice(xs)))
	lt := a < b
	// END PROBE F09.P2
	return xs, lt
}

func main() {
	fmt.Println(probe())
}
