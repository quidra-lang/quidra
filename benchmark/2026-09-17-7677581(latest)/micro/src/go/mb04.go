// MB-04 - Floating-point arithmetic: four accumulators over two arrays.
package main

import (
	"fmt"
	"math"
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
	s1, s2, s3, s4 float64
}

// workload is the entire measured body: it re-seeds the generator and
// regenerates A and B, as methodology 06 section 5.2 requires of every
// steady iteration.
func workload() result {
	const (
		m = 4000
		r = 75000
	)

	gen := lcg{state: 20264917}
	a := make([]float64, m)
	b := make([]float64, m)
	for i := 0; i < m; i++ {
		a[i] = 0.5 + gen.nextUnit()
	}
	for i := 0; i < m; i++ {
		b[i] = 0.5 + gen.nextUnit()
	}

	var s1, s2, s3, s4 float64
	for round := 0; round < r; round++ {
		a[round%m] = a[round%m] + 1.0e-9 // anti-elimination, part of the algorithm
		for i := 0; i < m; i++ {
			av := a[i]
			bv := b[i]
			s1 = s1 + av*bv
			s2 = s2 + av/(bv+2.0)
			s3 = s3 + math.Sqrt(av*av+bv*bv)
			s4 = s4 + (av-bv)*(av-bv)
		}
	}

	return result{s1: s1, s2: s2, s3: s3, s4: s4}
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
	fmt.Printf("MB04 s1=%.16e s2=%.16e s3=%.16e s4=%.16e\n", r.s1, r.s2, r.s3, r.s4)
}
