package main

import "fmt"

var released int32

// BEGIN PROBE F17.P2
type Handle struct{ id int32 }

func (h Handle) Close() { released++ }

func lifetime() int32 {
	func() {
		h := Handle{1}
		defer h.Close()
	}()
	return released
}

// END PROBE F17.P2

func main() {
	fmt.Println(lifetime())
}
