package main

import "fmt"

func mapped(o *int32) int32 {
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
}

func main() {
	x := int32(41)
	fmt.Println(mapped(&x), mapped(nil))
	m := int32(2147483647)
	fmt.Println("overflow wraps:", mapped(&m))
}
