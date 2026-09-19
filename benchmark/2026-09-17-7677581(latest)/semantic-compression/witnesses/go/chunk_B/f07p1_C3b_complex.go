package main

import "fmt"

func main() {
	var c complex128 = 3 + 0i
	var b complex128 = 2
	var a complex128 = 1
	r := a*b + c
	fmt.Println(r)
}
