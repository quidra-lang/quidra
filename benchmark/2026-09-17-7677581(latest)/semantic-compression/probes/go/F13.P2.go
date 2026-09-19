package main

import (
	"fmt"
	"strconv"
)

func recovered(s string) int32 {
	// BEGIN PROBE F13.P2
	v, err := strconv.ParseInt(s, 10, 32)
	var n int32
	if err == nil {
		n = int32(v)
	}
	return n
	// END PROBE F13.P2
}

func main() {
	fmt.Println(recovered("21"))
}
