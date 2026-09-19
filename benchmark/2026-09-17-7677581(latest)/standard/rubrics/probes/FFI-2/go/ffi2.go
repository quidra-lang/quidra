package main

import "C"

//export ffi2_add
func ffi2_add(a C.int, b C.int) C.int { return a + b }

func main() {}
