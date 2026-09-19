package main

import "fmt"

func mapped(o *int32) int32 {
	// BEGIN PROBE F12.P2
	h := func(v int32) int32 { return v + 1 }
	var p *int32
	if o != nil {
		r := h(*o)
		p = &r
	}
	var n int32
	if p != nil {
		n = *p
	}
	return n
	// END PROBE F12.P2
}

func main() {
	fmt.Println(mapped(nil))
}
