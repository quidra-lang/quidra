package main

import (
	"fmt"
	"strconv"
)

// BEGIN PROBE F13.P1
func parseTwice(s string) (int32, error) {
	v, err := strconv.ParseInt(s, 10, 32)
	if err != nil {
		return 0, err
	}
	return int32(v) * 2, nil
}

// END PROBE F13.P1

func main() {
	n, err := parseTwice("21")
	if err != nil {
		fmt.Println(err)
		return
	}
	fmt.Println(n)
}
