package main

import "fmt"

func probe() []int {
	mid := func(s []int) int { s[0] = 99; return s[1] }
	xs := []int{1, 2, 3}
	y := mid(xs)
	_ = y
	return xs
}

func main() { fmt.Println(probe()) }
