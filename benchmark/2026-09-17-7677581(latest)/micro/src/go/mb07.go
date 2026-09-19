// MB-07 - Sorting: bottom-up iterative merge sort, ascending, stable, ping-pong buffers.
package main

import (
	"fmt"
	"os"
	"slices"
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

// msort sorts a in place using buf as the alternate ping-pong buffer.
func msort(a, buf []int64) {
	n := len(a)
	src, dst := a, buf
	for width := 1; width < n; width *= 2 {
		for lo := 0; lo < n; lo += 2 * width {
			mid := min(lo+width, n)
			hi := min(lo+2*width, n)
			i, j, k := lo, mid, lo
			for i < mid && j < hi {
				if src[i] <= src[j] {
					dst[k] = src[i]
					i++
				} else {
					dst[k] = src[j]
					j++
				}
				k++
			}
			for i < mid {
				dst[k] = src[i]
				i++
				k++
			}
			for j < hi {
				dst[k] = src[j]
				j++
				k++
			}
		}
		src, dst = dst, src
	}
	if &src[0] != &a[0] {
		copy(a, src)
	}
}

type result struct {
	total, ssum, inv int64
}

// workload is the entire measured body: it re-seeds the generator and
// regenerates src, as methodology 06 section 5.2 requires of every steady
// iteration.
func workload() result {
	const (
		n = 2000000
		r = 4
	)

	gen := lcg{state: 20267917}
	src := make([]int64, n)
	for i := 0; i < n; i++ {
		src[i] = gen.nextInt()
	}
	buf := make([]int64, n)

	var total, ssum, inv int64
	for round := 0; round < r; round++ {
		src[round] = src[round] + 1 // anti-elimination
		a := slices.Clone(src)
		msort(a, buf)

		var chk int64
		for i := 0; i < n; i++ {
			chk = (chk*31 + a[i]%1000003) % 1000003
		}
		total = (total*7 + chk) % 1000003

		ssum = 0
		for i := 0; i < n; i++ {
			ssum = ssum + a[i]
		}
		for i := 1; i < n; i++ {
			if a[i-1] > a[i] {
				inv++
			}
		}
	}

	return result{total: total, ssum: ssum, inv: inv}
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
	fmt.Printf("MB07 %d %d %d\n", r.total, r.ssum, r.inv)
}
