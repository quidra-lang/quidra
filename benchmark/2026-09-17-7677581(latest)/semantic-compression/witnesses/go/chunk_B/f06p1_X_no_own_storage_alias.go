package main

import "fmt"

func probe() []int {
	mid := func(s []int) int { s = nil; _ = s; return 7 }
	xs := []int{1, 2, 3}
	y := mid(xs)
	_ = y
	return xs
}

func main() { fmt.Println(probe()) }
