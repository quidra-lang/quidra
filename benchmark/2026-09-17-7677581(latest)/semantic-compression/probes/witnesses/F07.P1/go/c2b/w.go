package main

import "fmt"

func probe() uint32 {
	var a uint32 = 2147483647
	var b uint32 = 3
	var c uint32 = 0
// BEGIN PROBE F07.P1
r := a*b + c
// END PROBE F07.P1
	return r
}

func main() { fmt.Println(probe()) }
