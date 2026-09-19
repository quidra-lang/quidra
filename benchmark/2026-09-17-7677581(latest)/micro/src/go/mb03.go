// MB-03 - Integer arithmetic: a five-operation mix driven by the frozen generator.
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

type result struct {
	sAdd, sXor, sMul, sDiv int64
}

// workload is the entire measured body (methodology 06 section 5.2).
func workload() result {
	const n = 120000000

	x := int64(20263917) // the seed itself is the initial generator state
	var sAdd, sXor, sDiv int64
	sMul := int64(1)

	for i := 0; i < n; i++ {
		x = (48271 * x) % 2147483647
		sAdd = (sAdd + x) % 2147483647
		sXor ^= x
		sMul = (sMul*33 + x%97) % 1000003
		sDiv += x / 1000
	}

	return result{sAdd: sAdd, sXor: sXor, sMul: sMul, sDiv: sDiv}
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
	var r result
	if mode == "steady" {
		for i := 0; i < k; i++ {
			t0 := time.Now() // section 5.2 pins time.Now/time.Since for Go
			r = workload()
			elapsed := time.Since(t0).Nanoseconds()
			// os.Stdout is unbuffered in Go, so this Printf is an immediate write.
			fmt.Printf("ITER %d %d\n", i, elapsed)
		}
	} else {
		r = workload()
	}
	fmt.Printf("MB03 %d %d %d %d\n", r.sAdd, r.sXor, r.sMul, r.sDiv)
}
