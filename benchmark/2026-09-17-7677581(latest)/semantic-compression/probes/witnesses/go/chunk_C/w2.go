package main

import "fmt"

func main() {
	xs := []int{10, 20, 30, 40, 50}
	part := xs[1:4]
	xs[1] = 99
	fmt.Println(part[0], len(part), cap(part))
}
