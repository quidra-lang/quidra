package main

import "fmt"

func narrow(big float64) int32 {
	small := int32(big)
	return small
}

func main() { fmt.Println(narrow(3e18), narrow(-2.9)) }
