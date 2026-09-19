package main

import "fmt"

func probe() {
	mid := func(s []int) int { return s[1] }
	xs := []int{1, 2, 3}
	y := mid(xs)
	var f float64 = y
	fmt.Println(f)
}

func main() { probe() }
