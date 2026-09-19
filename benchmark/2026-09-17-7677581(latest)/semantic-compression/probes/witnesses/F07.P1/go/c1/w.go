package main

import "fmt"

func probe() int32 {
	var a int32 = 2147483647
	var b int32 = 2
	var c int32 = 0
// BEGIN PROBE F07.P1
r := a*b + c
// END PROBE F07.P1
	return r
}

func main() { fmt.Println(probe()) }
