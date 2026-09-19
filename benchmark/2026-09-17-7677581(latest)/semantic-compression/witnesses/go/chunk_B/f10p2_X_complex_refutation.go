package main

import "fmt"

func main() {
	var c complex128 = 3 + 0i
	var d float64 = 0.5
	sum := float64(c) + d
	fmt.Println(sum)
}
