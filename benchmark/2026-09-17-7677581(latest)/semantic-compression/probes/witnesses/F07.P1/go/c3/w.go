package main

import "fmt"

const a, b, c = 2147483647, 2, 0

func probe() int {
// BEGIN PROBE F07.P1
r := a*b + c
// END PROBE F07.P1
	return r
}

func main() { fmt.Println(probe()) }
