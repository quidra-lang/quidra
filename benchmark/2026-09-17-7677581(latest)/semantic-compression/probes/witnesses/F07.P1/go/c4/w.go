package main

import "fmt"

func probe() float64 {
	var a float64 = 1e308
	var b float64 = 10
	var c float64 = 0
// BEGIN PROBE F07.P1
r := a*b + c
// END PROBE F07.P1
	return r
}

func main() { fmt.Println(probe()) }
