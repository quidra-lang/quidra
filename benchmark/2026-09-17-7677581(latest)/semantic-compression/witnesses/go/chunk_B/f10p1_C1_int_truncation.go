package main

import "fmt"

func narrow(big int64) int32 {
	small := int32(big)
	return small
}

func main() { fmt.Println(narrow(2147483648)) }
