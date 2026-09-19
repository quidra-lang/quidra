package main

type I32 int32

func mapped(o *I32) int32 {
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

func main() { _ = mapped(nil) }
