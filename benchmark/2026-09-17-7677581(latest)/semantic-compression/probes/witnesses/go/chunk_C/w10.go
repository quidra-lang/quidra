package main

import "fmt"

func main() {
	xs := []int{10, 20}
	defer func() { fmt.Println("recovered:", recover()) }()
	fmt.Println(xs[1:4])
}
