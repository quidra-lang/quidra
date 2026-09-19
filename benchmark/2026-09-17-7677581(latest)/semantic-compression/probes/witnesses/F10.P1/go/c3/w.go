package main

import "fmt"

func probe() int32 {
	var big float64 = 1e300
// BEGIN PROBE F10.P1
small := int32(big)
return small
// END PROBE F10.P1
}

func main() { fmt.Println(probe()) }
