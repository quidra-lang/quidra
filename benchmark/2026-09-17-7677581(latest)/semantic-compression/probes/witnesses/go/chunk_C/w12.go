package main

import (
	"fmt"
	"strconv"
)

func main() {
	v, err := strconv.ParseInt("99999999999", 10, 32)
	fmt.Println(v, err)
	fmt.Println(int32(v) * 2)
}
