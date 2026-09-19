package main

import "fmt"

const big = 2147483648

func probe() int32 {
// BEGIN PROBE F10.P1
small := int32(big)
return small
// END PROBE F10.P1
}

func main() { fmt.Println(probe()) }
