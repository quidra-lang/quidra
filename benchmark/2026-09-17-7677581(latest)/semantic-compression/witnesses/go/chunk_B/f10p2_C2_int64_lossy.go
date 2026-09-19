package main

import "fmt"

func main() {
	var i int64 = 9007199254740993
	var d float64 = 0
	sum := float64(i) + d
	fmt.Printf("%.0f\n", sum)
}
