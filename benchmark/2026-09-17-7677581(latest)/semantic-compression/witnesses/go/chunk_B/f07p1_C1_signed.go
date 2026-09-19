package main

import "fmt"

func main() {
	var a int32 = 2147483647
	var b int32 = 2
	var c int32 = 0
	r := a*b + c
	fmt.Println(r)
}
