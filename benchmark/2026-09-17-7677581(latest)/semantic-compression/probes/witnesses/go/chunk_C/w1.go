package main

import "fmt"

func readAt(xs []int, i int) int {
	e := xs[i]
	return e
}

func main() {
	fmt.Println(readAt([]int{1, 2, 3}, 5))
}
