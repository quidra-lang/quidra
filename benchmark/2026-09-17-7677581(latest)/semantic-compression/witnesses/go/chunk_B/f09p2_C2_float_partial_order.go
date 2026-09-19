package main

import (
	"fmt"
	"math"
	"sort"
)

func main() {
	xs := []int{3, 1, 2}
	a := math.NaN()
	b := math.NaN()
	sort.Sort(sort.Reverse(sort.IntSlice(xs)))
	lt := a < b
	fmt.Println(xs, lt, a != b)
}
