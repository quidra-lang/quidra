// MB-02 - Factorial: recompute k! mod M from scratch for every k.
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

const (
	modulus = 1000003
	n       = 20000
)

// workload is the entire measured body (methodology 06 section 5.2).
func workload() int64 {
	var total int64
	for k := int64(1); k <= n; k++ {
		f := int64(1)
		for j := int64(2); j <= k; j++ {
			f = (f * j) % modulus
		}
		total = (total + f) % modulus
	}
	return total
}

// parseMode reads the section 5.2 program mode from argv[1] and the steady
// iteration count K from argv[2] (default 7).
func parseMode() (string, int) {
	mode := "once"
	if len(os.Args) > 1 {
		mode = os.Args[1]
	}
	k := 7
	if len(os.Args) > 2 {
		if v, err := strconv.Atoi(os.Args[2]); err == nil && v > 0 {
			k = v
		}
	}
	return mode, k
}

func main() {
	mode, k := parseMode()
	var total int64
	if mode == "steady" {
		for i := 0; i < k; i++ {
			t0 := time.Now() // section 5.2 pins time.Now/time.Since for Go
			total = workload()
			elapsed := time.Since(t0).Nanoseconds()
			// os.Stdout is unbuffered in Go, so this Printf is an immediate write.
			fmt.Printf("ITER %d %d\n", i, elapsed)
		}
	} else {
		total = workload()
	}
	fmt.Printf("MB02 %d\n", total)
}
