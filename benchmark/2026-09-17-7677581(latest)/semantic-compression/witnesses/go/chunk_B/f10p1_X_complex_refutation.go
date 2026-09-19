package main

import "fmt"

func main() {
	var c complex128 = 3 + 0i
	small := int32(c)
	fmt.Println(small)
}
