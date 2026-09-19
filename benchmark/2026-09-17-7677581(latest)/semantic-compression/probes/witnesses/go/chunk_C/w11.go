package main

import "fmt"

func main() {
	xs := []int{10, 20, 30, 40, 50}
	part := xs[1:4]
	part = append(part, 999)
	fmt.Println(xs, part)
}
