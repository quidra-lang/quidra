package main

import "fmt"

func probe() float64 {
	var i float32 = 0.1
	var d float64 = 0
// BEGIN PROBE F10.P2
sum := float64(i) + d
// END PROBE F10.P2
	return sum
}

func main() { fmt.Println(probe()) }
