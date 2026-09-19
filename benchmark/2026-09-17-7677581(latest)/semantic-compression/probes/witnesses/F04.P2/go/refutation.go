// Refutation witness for F04.P2 / Go: Go offers no non-copying READ-ONLY view
// over the elements of a growable integer sequence.
package main

import "fmt"

func main() {
	xs := []int{4, 5, 6}

	// Candidate 1: the slice expression xs[:] -- non-copying, but fully writable.
	window := xs[:]
	window[0] = 99
	fmt.Println("slice view: non-copying =", &window[0] == &xs[0], "; read-only =", xs[0] != 99)

	// Candidate 2: a second name for the same slice -- identical result.
	ys := []int{4, 5, 6}
	alias := ys
	alias[0] = 99
	fmt.Println("alias:      non-copying =", &alias[0] == &ys[0], "; read-only =", ys[0] != 99)

	// Candidate 3: an array copy -- read-only in practice only because it is a COPY.
	zs := []int{4, 5, 6}
	var arr [3]int
	copy(arr[:], zs)
	arr[0] = 99
	fmt.Println("array copy: non-copying =", zs[0] == 99, "; read-only =", true)
}
