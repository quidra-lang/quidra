package main

import "fmt"

func probe() (int32, int32, float64) {
	var a int32 = -7
	var b int32 = 2
	// BEGIN PROBE F07.P2
	q := a / b
	m := a % b
	d := float64(a) / float64(b)
	// END PROBE F07.P2
	return q, m, d
}

func main() {
	fmt.Println(probe())
}
