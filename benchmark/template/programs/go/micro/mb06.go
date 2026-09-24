// MB-06 - Matrix multiplication: classical i-j-k over flat row-major storage.
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

// lcg is the Lehmer / MINSTD generator, frozen for every language in the suite.
type lcg struct {
	state int64
}

func (g *lcg) nextInt() int64 {
	g.state = (48271 * g.state) % 2147483647
	return g.state
}

func (g *lcg) nextUnit() float64 {
	return float64(g.nextInt()) / 2147483647.0
}

type result struct {
	sumC, cFirst, cLast float64
}

// workload is the entire measured body: it re-seeds the generator and
// regenerates A and B, as methodology 06 section 5.2 requires of every
// steady iteration.
func workload() result {
	const (
		n = 512
		r = 3
	)

	gen := lcg{state: 20266917}
	a := make([]float64, n*n)
	b := make([]float64, n*n)
	c := make([]float64, n*n)
	for i := 0; i < n*n; i++ {
		a[i] = gen.nextUnit()
	}
	for i := 0; i < n*n; i++ {
		b[i] = gen.nextUnit()
	}

	for round := 0; round < r; round++ {
		a[round] = a[round] + 1.0e-9 // anti-elimination
		for i := 0; i < n; i++ {
			for j := 0; j < n; j++ {
				s := 0.0
				for k := 0; k < n; k++ {
					s = s + a[i*n+k]*b[k*n+j]
				}
				c[i*n+j] = s
			}
		}
	}

	sumC := 0.0
	for i := 0; i < n*n; i++ {
		sumC = sumC + c[i]
	}

	return result{sumC: sumC, cFirst: c[0], cLast: c[n*n-1]}
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
	fmt.Printf("MB06 sumC=%.16e c_first=%.16e c_last=%.16e\n", r.sumC, r.cFirst, r.cLast)
}
